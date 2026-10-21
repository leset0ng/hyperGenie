package ng.lst.hha

import android.annotation.SuppressLint
import android.graphics.Rect
import android.graphics.Point
import android.util.Log
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedInterface.HookHandle
import io.github.libxposed.api.XposedModule
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.locks.ReentrantLock

/** Observes actual publication transactions without applying or retaining them. */
@SuppressLint("PrivateApi", "BlockedPrivateApi", "SoonBlockedPrivateApi")
internal class SurfaceEvents(
    private val module: XposedModule,
    private val loader: ClassLoader,
    private val onShow: (Show) -> Unit,
    private val onChanged: () -> Unit,
    private val onBufferCreated: (Any) -> Unit,
    private val onSourceRemoved: (Int) -> Unit,
) : AutoCloseable {
    data class Show(
        val surface: Any,
        val transaction: Any? = null,
        val window: Any? = null,
        val activity: Any? = null,
        val parent: Any? = null,
        val leash: Int = -1,
        val verifiedManager: Boolean = false,
    )

    private val members = OptionalMembers()
    private val hooks = ArrayList<HookHandle>()
    private data class Producer(val show: Show, val frame: Rect, val registeredNs: Long)
    private val localProducers = ConcurrentHashMap<Int, Producer>()
    private val surfaceClass = Class.forName("android.view.SurfaceControl")
    private val surfaceValid = surfaceClass.getMethod("isValid")
    private val surfaceLayer = surfaceClass.getMethod("getLayerId")
    private val surfaceWidth = surfaceClass.getMethod("getWidth")
    private val surfaceHeight = surfaceClass.getMethod("getHeight")
    private val surfaceLock = surfaceClass.getDeclaredField("mLock").apply { isAccessible = true }
    @Volatile private var closed = false

    fun install() {
        check(!closed && hooks.isEmpty())
        try {
            val animator = Class.forName("com.android.server.wm.WindowStateAnimator", false, loader)
            val show = animator.declaredMethods.single { it.name == "showRobustly" && it.parameterCount == 1 }
            hooks += module.hook(show)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("content-first-show-transaction")
                .intercept { chain ->
                    val owner = chain.thisObject
                    val wasShown = owner?.let { members.field(it, "mSurfaceShown") } == true
                    val result = chain.proceed()
                    observe {
                        if (!wasShown && owner != null && members.field(owner, "mSurfaceShown") == true) {
                            val window = members.field(owner, "mWin")
                            // The transaction may show the client/container
                            // ancestor. SELF still belongs on the retained BLAST.
                            val source = members.field(owner, "mSurfaceControl")
                            val transaction = chain.getArg(0)
                            if (source != null && transaction != null && valid(source)) {
                                onShow(Show(source, transaction, window,
                                    window?.let { members.field(it, "mActivityRecord") }))
                            }
                        }
                    }
                    changed()
                    result
                }
            optional("content source removal") {
                for (destroy in animator.declaredMethods.filter { it.name == "destroySurfaceLocked" }) {
                    hooks += module.hook(destroy)
                        .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                        .setId("content-source-destroy-${destroy.parameterCount}")
                        .intercept { chain ->
                            val source = chain.thisObject?.let { members.field(it, "mSurfaceControl") }
                            val id = source?.let { runCatching { layer(it) }.getOrDefault(-1) } ?: -1
                            val result = chain.proceed()
                            observe { if (id >= 0) onSourceRemoved(id) }
                            result
                        }
                }
            }
            creationEvents(animator)
            optional("local legacy buffer creation") {
                val surface = Class.forName("android.view.Surface", false, loader)
                val copy = surface.getDeclaredMethod("copyFrom", surfaceClass)
                hooks += module.hook(copy)
                    .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                    .setId("content-local-bbq-creation")
                    .intercept { chain ->
                        val result = chain.proceed()
                        observe {
                            val source = chain.getArg(0) ?: return@observe
                            val producer = localProducers[layer(source)]
                            if (producer != null && producer.show.surface === source &&
                                System.nanoTime() - producer.registeredNs in 0..1_500_000_000L &&
                                identityFrame(checkNotNull(producer.show.activity)) == producer.frame) {
                                onShow(producer.show)
                            }
                            onBufferCreated(source)
                        }
                        result
                    }
            }
            for (name in listOf("finishDrawingLocked", "destroySurfaceLocked", "hide")) {
                lifecycle(animator, name)
            }
            managerEvents()
            startupEvents()
            module.log(Log.INFO, TAG, "surface lifecycle and publication observers installed")
        } catch (error: Throwable) {
            close()
            throw error
        }
    }

    private fun creationEvents(animator: Class<*>) {
        for (create in animator.declaredMethods.filter { it.name == "createSurfaceLocked" }) {
            hooks += module.hook(create)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("content-created-before-client-${create.parameterCount}")
                .intercept { chain ->
                    val result = chain.proceed()
                    observe { chain.thisObject?.let { created(it, null) } }
                    changed()
                    result
                }
        }
        optional("first client layout") {
            val windowState = Class.forName("com.android.server.wm.WindowState", false, loader)
            for (layout in windowState.declaredMethods.filter { it.name == "setFrames" }) {
                hooks += module.hook(layout)
                    .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                    .setId("content-first-layout-${layout.parameterCount}")
                    .intercept { chain ->
                        val result = chain.proceed()
                        observe {
                            val window = chain.thisObject ?: return@observe
                            val owner = members.field(window, "mWinAnimator") ?: return@observe
                            // Client attachment can precede the first layout.
                            // Once the real local frame exists, protect the
                            // retained buffer before returning it to its producer.
                            if (members.field(owner, "mSurfaceShown") != true) created(owner, null)
                        }
                        changed()
                        result
                    }
            }
        }
        optional("client surface publication") {
            val flags = Class.forName(
                "com.android.internal.hidden_from_bootclasspath.com.android.window.flags.Flags", false, loader)
            val attachWithSync = flags.getDeclaredMethod("attachClientSurfaceWithSync")
            val snapshotData = Class.forName("com.android.server.wm.SnapshotStartingData", false, loader)
            val attach = animator.getDeclaredMethod("attachClientSurfaceWithSync", surfaceClass)
            hooks += module.hook(attach)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("content-client-attach-transaction")
                .intercept { chain ->
                    observe {
                        val owner = chain.thisObject ?: return@observe
                        val source = chain.getArg(0) ?: return@observe
                        val window = members.field(owner, "mWin") ?: return@observe
                        val retained = members.field(owner, "mSurfaceControl") ?: return@observe
                        if (!valid(source) || layer(source) != layer(retained)) return@observe
                        // A snapshot producer may submit its first buffer before
                        // WMS marks it drawn/shown. Protect it before reparenting
                        // makes that already-buffered client surface reachable.
                        created(owner, source)
                        val pending = members.call(window, "getPendingTransaction") ?: return@observe
                        created(owner, source, pending)
                        // Mirror only publication paths the platform will use;
                        // gating on an unused sync transaction would stall vsync.
                        val syncEnabled = attachWithSync.invoke(null) == true
                        val snapshotSync = !syncEnabled &&
                            snapshotData.isInstance(members.field(window, "mStartingData")) &&
                            (members.field(window, "mSyncState") as? Int)?.let { it != 0 } == true
                        if (syncEnabled || snapshotSync) {
                            val sync = members.call(window, "getSyncTransaction")
                            if (sync != null && sync !== pending) created(owner, source, sync)
                        }
                    }
                    val result = chain.proceed()
                    changed()
                    result
                }
        }
    }

    private fun created(owner: Any, client: Any?, transaction: Any? = null) {
        val window = members.field(owner, "mWin") ?: return
        val source = client ?: members.field(owner, "mSurfaceControl") ?: return
        if (valid(source)) onShow(Show(source, transaction, window,
            members.field(window, "mActivityRecord")))
    }

    private fun managerEvents() {
        optional("manager preview show") {
            val manager = Class.forName("com.android.server.wm.PreStartingManager", false, loader)
            val show = manager.declaredMethods.single { it.name == "showPreSurface" && it.parameterCount == 5 }
            hooks += module.hook(show)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("prestarting-manager-show-transaction")
                .intercept { chain ->
                    val result = chain.proceed()
                    observe {
                        val owner = chain.thisObject
                        val surface = chain.getArg(0)
                        val transaction = chain.getArg(1)
                        val data = chain.getArg(3)
                        val activity = chain.getArg(4)
                        if (owner != null && surface != null && transaction != null && data != null && activity != null) {
                            managerShow(owner, surface, transaction, data, activity, chain.getArg(2))
                        }
                    }
                    changed()
                    result
                }
            val showStarting = manager.declaredMethods.single { it.name == "showPreStarting" && it.parameterCount == 2 }
            hooks += module.hook(showStarting)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("prestarting-manager-reshow-transaction")
                .intercept { chain ->
                    val owner = chain.thisObject
                    val activity = chain.getArg(1)
                    val data = if (owner != null && activity != null) runCatching {
                        members.field(activity, "mStartingData") ?: run {
                            val pending = members.field(owner, "mPendingRemoveStartingData") as? Map<*, *>
                            if (pending == null) null else synchronized(pending) { pending[activity] }
                        }
                    }.getOrNull() else null
                    val result = chain.proceed()
                    observe {
                        val transaction = chain.getArg(0)
                        if (owner != null && activity != null && data != null && transaction != null) {
                            // The original method shows exactly one of these;
                            // when both existed it removed the color first.
                            withManagerLock(owner) {
                                val snapshots = members.field(owner, "mPreSurfaceControlMap") as? Map<*, *>
                                val colors = members.field(owner, "mColorSurfaceControlMap") as? Map<*, *>
                                val surface = snapshots?.get(data) ?: colors?.get(data)
                                if (surface != null) managerShowLocked(owner, surface, transaction, data, activity, null)
                            }
                        }
                    }
                    changed()
                    result
                }
            for (name in listOf("onStartingAdded", "resetIfNeeded", "onStartingWindowDrawn")) lifecycle(manager, name)
        }
    }

    private fun startupEvents() {
        optional("independent preview show") {
            val animation = Class.forName("com.android.server.wm.PreStartupAnimation", false, loader)
            val build = animation.getDeclaredMethod("buildTransitionInfoAndTransactions")
            hooks += module.hook(build)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("prestartup-show-transaction")
                .intercept { chain ->
                    val result = chain.proceed()
                    observe { chain.thisObject?.let(::openingShow) }
                    changed()
                    result
                }
            lifecycle(animation, "cleanUp")
            val controller = Class.forName("com.android.server.wm.PreStartupAnimationController", false, loader)
            lifecycle(controller, "addAnimation")
        }
    }

    /** Runs under the OEM caller's existing locks; never takes WMS/bridge locks. */
    private fun managerShow(owner: Any, surface: Any, transaction: Any, data: Any, activity: Any, parent: Any?) {
        withManagerLock(owner) { managerShowLocked(owner, surface, transaction, data, activity, parent) }
    }

    private fun managerShowLocked(owner: Any, surface: Any, transaction: Any, data: Any, activity: Any, parent: Any?) {
        val frame = identityFrame(activity) ?: return
        val snapshots = members.field(owner, "mPreSurfaceControlMap") as? Map<*, *> ?: return
        val colors = members.field(owner, "mColorSurfaceControlMap") as? Map<*, *> ?: return
        val color = colors[data] === surface
        if (!color) {
            if (snapshots[data] !== surface) return
            val positions = members.field(owner, "mPreSurfaceInitPosMap") as? Map<*, *> ?: return
            val scales = members.field(owner, "mPreSurfaceScaleMap") as? Map<*, *> ?: return
            if (!identityPair(positions[data], 0f) || !identityPair(scales[data], 1f)) return
            val snapshot = members.call(data, "getSnapshot") ?: return
            if (members.call(snapshot, "getRotation") != 0 || members.call(snapshot, "isBufferValid") != true ||
                members.call(snapshot, "getHardwareBufferWidth") != frame.width() ||
                members.call(snapshot, "getHardwareBufferHeight") != frame.height()) return
        }
        if (extent(surface, frame, color)) {
            val show = Show(surface, transaction, activity = activity, parent = parent, verifiedManager = true)
            val now = System.nanoTime()
            // Retain no borrowed transaction. If Surface(sc) materializes its
            // local BBQ after show, protect that buffer before its first post.
            localProducers[layer(surface)] = Producer(show.copy(transaction = null), Rect(frame), now)
            localProducers.entries.removeIf { now - it.value.registeredNs > 1_500_000_000L }
            onShow(show)
        }
    }

    private inline fun withManagerLock(owner: Any, action: () -> Unit) {
        val lock = members.field(owner, "mSurfaceLock") as? ReentrantLock ?: return
        // Usually already held/reentrant. A missing show-time proof is an
        // exclusion, never a reason to wait under a WMS publication callback.
        if (!lock.tryLock()) return
        try { action() } finally { lock.unlock() }
    }

    private fun openingShow(animation: Any) {
        val display = members.field(animation, "mDisplayContent") ?: return
        val frame = (members.call(display, "getBounds") as? Rect)?.let(::Rect) ?: return
        if (frame.left != 0 || frame.top != 0 || frame.width() <= 0 || frame.height() <= frame.width() ||
            members.call(display, "getRotation") != 0) return
        val parent = members.field(animation, "mOpeningLeash") ?: return
        val source = members.call(animation, "getPreStartingSurface") ?: return
        val transaction = members.field(animation, "mStartT") ?: return
        val leash = layer(parent)
        if (leash < 0 || !extent(source, frame, false)) return
        val type = members.call(animation, "getPreStartingType")
        if (type != 2 && (type != 1 || PreStartingSurfaces.observedDomain(layer(source)) != frame)) return
        val info = members.field(animation, "mTransitionInfo") ?: return
        val changes = members.call(info, "getChanges") as? List<*> ?: return
        val change = changes.filterNotNull().singleOrNull {
            members.call(it, "getLeash")?.let(::layer) == leash
        } ?: return
        if (members.call(change, "getMode") != 1 || members.call(change, "getEndRelOffset") != Point(0, 0) ||
            members.call(change, "getStartRotation") != 0 || members.call(change, "getEndRotation") != 0 ||
            members.call(change, "getStartAbsBounds") != frame || members.call(change, "getEndAbsBounds") != frame) return
        onShow(Show(source, transaction, parent = parent, leash = leash))
    }

    private fun identityFrame(activity: Any): Rect? {
        val task = members.call(activity, "getTask") ?: return null
        val display = members.call(activity, "getDisplayContent") ?: return null
        val frame = (members.call(activity, "getBounds") as? Rect)?.let(::Rect) ?: return null
        return frame.takeIf {
            frame.left == 0 && frame.top == 0 && frame.width() > 0 && frame.height() > frame.width() &&
                members.call(activity, "getWindowingMode") == 1 && members.call(task, "getWindowingMode") == 1 &&
                members.call(activity, "isEmbedded") == false &&
                members.call(activity, "hasFixedRotationTransform") == false &&
                members.call(activity, "isLetterboxedForFixedOrientationAndAspectRatio") == false &&
                members.call(task, "getBounds") == frame && members.call(display, "getBounds") == frame &&
                members.call(display, "getRotation") == 0
        }
    }

    private fun extent(surface: Any, frame: Rect, color: Boolean): Boolean = synchronized(checkNotNull(surfaceLock.get(surface))) {
        if (surfaceValid.invoke(surface) != true) return@synchronized false
        val width = surfaceWidth.invoke(surface) as Int
        val height = surfaceHeight.invoke(surface) as Int
        (width == frame.width() && height == frame.height()) || (color && width <= 0 && height <= 0)
    }

    private fun valid(surface: Any): Boolean = synchronized(checkNotNull(surfaceLock.get(surface))) {
        surfaceValid.invoke(surface) == true
    }

    private fun layer(surface: Any): Int = synchronized(checkNotNull(surfaceLock.get(surface))) {
        if (surfaceValid.invoke(surface) == true) surfaceLayer.invoke(surface) as Int else -1
    }

    private fun identityPair(value: Any?, expected: Float): Boolean = value == null ||
        (value is FloatArray && value.size == 2 && value.all { it.isFinite() && it == expected })

    private fun lifecycle(type: Class<*>, name: String) {
        for (method in type.declaredMethods.filter { it.name == name }) {
            hooks += module.hook(method)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("surface-event-${type.simpleName}-$name-${method.parameterCount}")
                .intercept { chain ->
                    val result = chain.proceed()
                    changed()
                    result
                }
        }
    }

    private fun observe(action: () -> Unit) {
        if (!closed) runCatching(action).onFailure {
            module.log(Log.WARN, TAG, "surface publication observation failed; retaining stock state", it)
        }
    }

    private fun changed() { if (!closed) runCatching { onChanged() } }

    private inline fun optional(name: String, action: () -> Unit) {
        runCatching(action).onFailure {
            module.log(Log.WARN, TAG, "$name observer unavailable", it)
        }
    }

    override fun close() {
        closed = true
        hooks.asReversed().forEach { runCatching { it.unhook() } }
        hooks.clear()
        localProducers.clear()
    }

    private companion object { const val TAG = "HHA.SurfaceEvents" }
}
