package ng.lst.hha

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Rect
import android.os.Binder
import android.os.Handler
import android.os.HandlerThread
import android.os.Parcel
import android.provider.Settings
import android.util.Log
import android.view.WindowManager
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedInterface.HookHandle
import io.github.libxposed.api.XposedModule
import java.lang.reflect.Field
import java.lang.reflect.Method
import java.util.concurrent.ConcurrentHashMap

/** System-server-only content ownership bridge. No hooks are installed in target Apps. */
@SuppressLint("PrivateApi", "BlockedPrivateApi", "SoonBlockedPrivateApi")
internal class CentralBridge(private val module: XposedModule, loader: ClassLoader) : AutoCloseable {
    private val access = Access()
    private val snapshots = SnapshotGuard(module, loader)
    private val preStarting = PreStartingSurfaces(module, loader)
    private val snapshotReleases = HashMap<Int, Any>()
    private val wmsClass = Class.forName("com.android.server.wm.WindowManagerService", false, loader)
    private val scClass = Class.forName("android.view.SurfaceControl")
    private val copySurface = scClass.getConstructor(scClass, String::class.java)
    private val validSurface = scClass.getMethod("isValid")
    private val surfaceLayer = scClass.getMethod("getLayerId")
    private val releaseSurface = scClass.getMethod("release")
    private val surfaceWidth = scClass.getMethod("getWidth")
    private val surfaceHeight = scClass.getMethod("getHeight")
    private val surfaceLock = scClass.getDeclaredField("mLock").apply { isAccessible = true }
    private val nativeObject = scClass.getDeclaredField("mNativeObject").apply { isAccessible = true }
    private val stateLock = Any()
    private val worker = HandlerThread("HHA.Bridge").apply { start() }
    private val handler = Handler(worker.looper)
    private var hook: HookHandle? = null
    private var generation = 0L
    private var lastEventNs = 0L
    private var active: Active? = null
    @Volatile private var closed = false
    @Volatile private var rendererReady = false

    fun install() {
        check(!closed && hook == null)
        try {
            snapshots.install()
            preStarting.install()
            installHook()
        } catch (error: Throwable) {
            close()
            throw error
        }
    }

    private fun installHook() {
        val method = wmsClass.getDeclaredMethod("onTransact", Int::class.javaPrimitiveType,
            Parcel::class.java, Parcel::class.java, Int::class.javaPrimitiveType)
        hook = module.hook(method)
            .setPriority(XposedInterface.PRIORITY_DEFAULT)
            .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
            .setId("central-content-bridge-v2")
            .intercept { chain ->
                if (chain.getArg(0) != TRANSACTION) return@intercept chain.proceed()
                val reply = chain.getArg(2) as? Parcel ?: return@intercept false
                val service = chain.thisObject ?: return@intercept false
                val input = chain.getArg(1) as? Parcel ?: return@intercept false
                transact(service, input, reply)
                true
            }
        handler.post { prepareRenderer() }
        report(Log.INFO, "WMS bridge installed; requests are queued without taking the WM lock")
    }

    private fun transact(service: Any, input: Parcel, reply: Parcel) {
        val caller = Binder.getCallingUid()
        val identity = Binder.clearCallingIdentity()
        try {
            val context = access.field(service, "mContext") as Context
            if (!authorized(context, caller)) {
                reply.writeException(SecurityException("HHA bridge is restricted to the launcher UID"))
                return
            }
            val accepted = runCatching {
                input.enforceInterface(DESCRIPTOR)
                val request = Request.read(input) ?: return@runCatching false
                if (closed) return@runCatching false
                if (!rendererReady && request.operation != CANCEL) {
                    handler.post { prepareRenderer() }
                    return@runCatching false
                }
                // Native launcher submission can be reached from a WM remote
                // callback. Never acquire mGlobalLock or call native rendering
                // synchronously on this Binder thread.
                handler.post { process(service, context, request) }
            }.onFailure {
                report(Log.WARN, "request rejected; keeping stock animation", it)
            }.getOrDefault(false)
            reply.writeNoException()
            reply.writeInt(if (accepted) 1 else 0)
        } catch (error: Throwable) {
            report(Log.ERROR, "bridge transaction failed", error)
            reply.writeNoException()
            reply.writeInt(0)
        } finally {
            Binder.restoreCallingIdentity(identity)
        }
    }

    /** Resolve package ownership outside the WMS lock; reject shared-UID callers. */
    private fun authorized(context: Context, caller: Int): Boolean {
        if (caller == 0) return true
        val pm = context.packageManager
        val packages = pm.getPackagesForUid(caller) ?: return false
        if (packages.size != 1 || packages[0] != LAUNCHER) return false
        val getUid = pm.javaClass.getMethod("getPackageUidAsUser", String::class.java,
            Int::class.javaPrimitiveType)
        return (getUid.invoke(pm, LAUNCHER, caller / 100000) as Int) == caller
    }

    private fun animationsEnabled(context: Context): Boolean = SCALE_KEYS.all { key ->
        val scale = Settings.Global.getFloat(context.contentResolver, key, 1f)
        scale.isFinite() && scale > 0f
    }

    private fun cancel(request: Request): Boolean {
        val current = active
        if (request.leash != -1 && current?.parentLayer != request.leash) return false
        // A stop for the previous leash can arrive after the next pre-start
        // began but before its Task anchor is delivered. Do not move the global
        // transition watermark past that queued replacement's original clock.
        if (request.leash == -1) {
            lastEventNs = request.eventNs
            stopLocked("cancelled")
        } else if (current != null) {
            // Launcher emits stop(old) immediately before start/adjust(new).
            // Defer retirement briefly so the successor can inherit all 19
            // live states; an isolated cancellation still detaches promptly.
            val cancelledGeneration = current.generation
            handler.postDelayed({
                synchronized(stateLock) {
                    if (active === current && generation == cancelledGeneration &&
                        lastEventNs <= request.eventNs) stopLocked("cancelled")
                }
            }, CANCEL_GRACE_MS)
        }
        return true
    }

    private fun start(match: Snapshot, request: Request, service: Any): Boolean {
        val content = match.content ?: return false
        // Capture pristine content before a close can deform it. Skip recording
        // while any prior generation still has an uncommitted detach.
        if (request.closing && !match.starting && !ContentAnimator.running())
            snapshots.refresh(service, match.taskId)
        snapshotReleases.remove(match.taskId)
        snapshots.block(match.taskId)
        // These are bridge-owned copies. The WMS global lock was released before
        // acquiring these locks or calling JNI, which may transact with SF.
        // Fixed order: parent, primary content, then companions in layer order.
        // Copies normally have independent mLocks; remove identity duplicates.
        val locks = ArrayList<Any>(2 + match.companions.size)
        for (surface in listOf(match.parent, content) + match.companions) {
            val lock = surfaceLock.get(surface) ?: return false
            if (locks.none { it === lock }) locks += lock
        }
        fun locked(index: Int): Boolean {
            if (index < locks.size) return synchronized(locks[index]) { locked(index + 1) }
            val pointer = nativeObject.getLong(content)
            val parent = nativeObject.getLong(match.parent)
            val companions = LongArray(match.companions.size) { nativeObject.getLong(match.companions[it]) }
            if (pointer == 0L || parent == 0L || companions.any { it == 0L }) return false
            return ContentAnimator.transition(pointer, parent, match.width, match.height,
                request.geometry, request.closing, request.eventNs, companions, match.starting)
        }
        return locked(0)
    }

    private fun prepareRenderer() {
        if (closed || rendererReady) return
        rendererReady = runCatching { ContentAnimator.initialize() }.onFailure {
            report(Log.WARN, "native renderer unavailable; keeping stock animation", it)
        }.getOrDefault(false)
    }

    private fun process(service: Any, context: Context, request: Request) {
        synchronized(stateLock) {
            if (closed || request.eventNs < lastEventNs) return
            if (request.operation == CANCEL) {
                cancel(request)
                return
            }
            lastEventNs = request.eventNs
            val previous = active
            // Keep a still-live content mesh until the replacement is resolved.
            // Native compares actual content identity and can preserve all 19
            // states even when Shell has installed a new transition leash.
            val next = Active(++generation, service, context, request)
            // Metadata is reused only for the same exact parent, to prevent a
            // pending retry from attaching to another Activity under that leash.
            if (previous?.parentLayer == request.leash) {
                next.activityToken = previous.activityToken
                next.taskId = previous.taskId
                next.uid = previous.uid
            }
            active = next
            if (previous != null) releaseSnapshotAfterDetach(previous)
            checkCurrent(next)
        }
    }

    private fun schedule(current: Active, delay: Long) {
        handler.postDelayed({
            synchronized(stateLock) { checkCurrent(current) }
        }, delay)
    }

    /** Worker-thread only. accepted=1 means queued, never a synchronous takeover. */
    private fun checkCurrent(current: Active) {
        if (closed || active !== current || current.generation != generation) return
        try {
            val age = System.nanoTime() - current.request.eventNs
            if (current.snapshotBlocked && age > 150_000_000L && !ContentAnimator.running()) {
                current.snapshotBlocked = false
                snapshots.unblock(current.taskId)
                if (!current.request.closing) snapshots.refresh(current.service, current.taskId)
            }
            if (!animationsEnabled(current.context) || age > SESSION_NS) {
                stopLocked("watchdog")
                return
            }
            val match = lookup(current.service, current.request)
            if (match == null) {
                if (current.contentLayer < 0 && age < SURFACE_WAIT_NS) schedule(current, RETRY_MS)
                else stopLocked("leash-removed")
                return
            }
            match.use {
                if (current.activityToken != null && (it.activityToken !== current.activityToken ||
                        it.taskId != current.taskId || it.uid != current.uid)) {
                    stopLocked("owner-changed")
                    return
                }
                current.activityToken = it.activityToken
                current.taskId = it.taskId
                current.uid = it.uid
                if (current.contentLayer < 0) {
                    if (it.content != null) {
                        if (!start(it, current.request, current.service)) {
                            stopLocked("native-rejected")
                            return
                        }
                        rememberLayers(current, it)
                        report(Log.INFO, "attached parent=${current.parentLayer} content=${it.contentLayer} " +
                            "companions=${it.companionLayers} starting=${it.starting} " +
                            "task=${it.taskId} uid=${it.uid} closing=${current.request.closing}")
                        schedule(current, CHECK_MS)
                    } else if (age < SURFACE_WAIT_NS) {
                        schedule(current, RETRY_MS)
                    } else {
                        stopLocked("first-draw-timeout")
                    }
                } else if (it.width != current.width || it.height != current.height) {
                    stopLocked("extent-changed")
                } else if (it.contentLayer != current.contentLayer ||
                    it.companionLayers != current.companionLayers) {
                    val contentChanged = it.contentLayer != current.contentLayer
                    // The same Activity's real content replaces its starting
                    // BLAST without resetting the request or 19-state model.
                    val onlyRemoved = !contentChanged &&
                        it.companionLayers.all { layer -> layer in current.companionLayers }
                    if (onlyRemoved && !ContentAnimator.running()) {
                        // A removed preview after settlement needs no new native
                        // session. Reattaching would expose the already-finished mesh.
                        current.companionLayers = it.companionLayers
                        schedule(current, CHECK_MS)
                    } else if (it.content == null || (contentChanged && (!current.starting || it.starting))) {
                        stopLocked("surface-replaced")
                    } else if (!start(it, current.request, current.service)) {
                        stopLocked("handoff-rejected")
                    } else {
                        report(Log.INFO, "handoff parent=${current.parentLayer} " +
                            "content=${current.contentLayer}->${it.contentLayer} " +
                            "companions=${current.companionLayers}->${it.companionLayers} " +
                            "starting=${current.starting}->${it.starting}")
                        rememberLayers(current, it)
                        schedule(current, CHECK_MS)
                    }
                } else {
                    schedule(current, CHECK_MS)
                }
            }
        } catch (_: PreStartingSurfaces.Busy) {
            // OEM preload may acquire its lock before WMS. Never wait for it
            // under mGlobalLock or mistake contention for a removed leash.
            schedule(current, RETRY_MS)
        } catch (error: Throwable) {
            report(Log.WARN, "lifecycle check failed; releasing effect", error)
            stopLocked("lookup-failed")
        }
    }

    private fun rememberLayers(current: Active, match: Snapshot) {
        current.contentLayer = match.contentLayer
        current.companionLayers = match.companionLayers
        current.starting = match.starting
        current.snapshotBlocked = true
        current.width = match.width
        current.height = match.height
    }

    /** Copies only while WM's ownership is stable, never returns borrowed surfaces. */
    private fun lookup(service: Any, request: Request): Snapshot? {
        preStarting.prepare()
        val lock = access.field(service, "mGlobalLock") ?: return null
        return synchronized(lock) {
            val windows = access.field(service, "mWindowMap") as? Map<*, *> ?: return@synchronized null
            val candidates = windows.values.mapNotNull { win ->
                win ?: return@mapNotNull null
                if (access.field(win, "mRemoved") == true ||
                    access.field(win, "mViewVisibility") != 0) return@mapNotNull null
                val attrs = access.field(win, "mAttrs") as? WindowManager.LayoutParams
                    ?: return@mapNotNull null
                if (attrs.type !in 1..3 || attrs.packageName == LAUNCHER ||
                    attrs.flags and WindowManager.LayoutParams.FLAG_SECURE != 0) return@mapNotNull null
                val activity = access.field(win, "mActivityRecord") ?: return@mapNotNull null
                val task = access.call(win, "getTask") ?: return@mapNotNull null
                if (access.call(task, "getWindowingMode") != 1) return@mapNotNull null
                val parent = if (request.containerLayer >= 0) matchingParent(win, request.containerLayer)
                    else if (request.taskId == access.field(task, "mTaskId"))
                        access.field(task, "mSurfaceControl")?.takeIf(::isValid) else null
                parent ?: return@mapNotNull null
                val info = access.field(activity, "info") as? android.content.pm.ActivityInfo
                    ?: return@mapNotNull null
                val uid = info.applicationInfo?.uid ?: return@mapNotNull null
                val ownerUid = access.field(win, "mOwnerUid") as? Int ?: return@mapNotNull null
                if (attrs.type != 3 && ownerUid != uid) return@mapNotNull null
                val token = access.field(activity, "token") ?: return@mapNotNull null
                val taskId = access.field(task, "mTaskId") as? Int ?: return@mapNotNull null
                if (request.taskId >= 0 && taskId != request.taskId) return@mapNotNull null
                // Both ordinary and starting windows can have real BLAST
                // buffers. Only the animator's retained SC supplies content;
                // the WindowContainer SC above remains a parent capability.
                val animator = access.field(win, "mWinAnimator")
                val content = if (access.field(win, "mHasSurface") == true)
                    animator?.let { access.field(it, "mSurfaceControl") }?.takeIf(::isValid) else null
                Candidate(win, parent, content, activity, task, token, taskId, uid, attrs.type)
            }
            val normals = candidates.filter { it.type != 3 }
            val normalContents = normals.filter { it.content != null }
            val startingContents = candidates.filter { it.type == 3 && it.content != null }
            val preferred = normalContents.filter { it.type == 1 }.ifEmpty { normalContents }
                .ifEmpty { startingContents }
                .ifEmpty { normals.filter { it.type == 1 } }.ifEmpty { normals }
                .ifEmpty { candidates.filter { it.type == 3 } }
            // A task-level leash may contain more than one Activity. Do not pick
            // an arbitrary package/window from such an ambiguous subtree.
            if (preferred.isEmpty() || preferred.any {
                    it.activityToken !== preferred[0].activityToken || it.taskId != preferred[0].taskId
                } || preferred.count { it.content != null } > 1) return@synchronized null
            val selected = preferred.firstOrNull { it.content != null } ?: preferred[0]
            val size = if (selected.content != null) contentSize(selected)
                ?: return@synchronized null else null
            val frame = access.call(selected.window, "getFrame") as? Rect
            val primaryLayer = selected.content?.let { surfaceLayer.invoke(it) as Int } ?: -1
            val companion = if (selected.type != 3 && size != null) startingContents.filter {
                it.activityToken === selected.activityToken && it.taskId == selected.taskId &&
                    it.uid == selected.uid && !isLayer(it.content, primaryLayer) &&
                    contentSize(it) == size && framesAligned(frame, access.call(it.window, "getFrame") as? Rect)
            }.singleOrNull() else null
            var contentCopy: Any? = null
            var companionCopy: Any? = null
            var oemCopies: List<Any> = emptyList()
            var parentCopy: Any? = null
            try {
                parentCopy = copySurface.newInstance(selected.parent, "HHA.central.parent")
                contentCopy = selected.content?.let { copySurface.newInstance(it, "HHA.central.content") }
                companionCopy = companion?.content?.let { copySurface.newInstance(it, "HHA.central.starting") }
                val excluded = mutableSetOf(primaryLayer)
                companionCopy?.let { excluded += surfaceLayer.invoke(it) as Int }
                if (size != null && !request.closing) {
                    oemCopies = preStarting.copy(selected.activity, selected.task, frame,
                        size.first, size.second, request.leash, excluded)
                }
                val companions = (listOfNotNull(companionCopy) + oemCopies)
                    .sortedBy { surfaceLayer.invoke(it) as Int }
                Snapshot(parentCopy, contentCopy, companions, selected.type == 3,
                    size?.first ?: 0, size?.second ?: 0, selected.activityToken,
                    selected.taskId, selected.uid).also {
                    parentCopy = null; contentCopy = null; companionCopy = null
                    oemCopies = emptyList()
                }
            } finally {
                oemCopies.forEach(::release)
                release(companionCopy)
                release(contentCopy)
                release(parentCopy)
            }
        }
    }

    private fun contentSize(candidate: Candidate): Pair<Int, Int>? {
        val content = candidate.content ?: return null
        var width = surfaceWidth.invoke(content) as Int
        var height = surfaceHeight.invoke(content) as Int
        if (width <= 0 || height <= 0) {
            val frame = access.call(candidate.window, "getFrame") as? Rect ?: return null
            width = frame.width()
            height = frame.height()
        }
        return if (width in 1..MAX_EXTENT && height in 1..MAX_EXTENT) width to height else null
    }

    private fun framesAligned(primary: Rect?, companion: Rect?): Boolean =
        primary != null && companion != null && primary == companion

    private fun matchingParent(window: Any, wanted: Int): Any? {
        var node: Any? = window
        repeat(32) {
            val current = node ?: return null
            val direct = access.field(current, "mSurfaceControl")
            if (isLayer(direct, wanted)) return direct
            val animationLeash = access.field(current, "mAnimationLeash")
                ?: access.call(current, "getAnimationLeash")
            if (isLayer(animationLeash, wanted)) return animationLeash
            for (field in arrayOf("mSurfaceAnimator", "mSurfaceFreezer")) {
                val holder = access.field(current, field) ?: continue
                val leash = access.field(holder, "mLeash")
                if (isLayer(leash, wanted)) return leash
            }
            node = access.call(current, "getParent")
        }
        return null
    }

    private fun isValid(surface: Any?): Boolean = surface != null && validSurface.invoke(surface) == true
    private fun isLayer(surface: Any?, wanted: Int): Boolean =
        isValid(surface) && surfaceLayer.invoke(surface) == wanted

    private fun release(surface: Any?) {
        if (surface != null) runCatching { releaseSurface.invoke(surface) }
    }

    private fun stopLocked(reason: String) {
        generation++
        val previous = active
        active = null
        ContentAnimator.stop()
        if (previous != null) {
            releaseSnapshotAfterDetach(previous)
            report(Log.INFO, "stop parent=${previous.parentLayer} reason=$reason")
        }
    }

    private fun releaseSnapshotAfterDetach(previous: Active) {
        if (previous.taskId < 0) return
        val token = Any()
        snapshotReleases[previous.taskId] = token
        fun check() {
            if (closed || snapshotReleases[previous.taskId] !== token) return
            val current = active
            if (current?.taskId == previous.taskId && current.snapshotBlocked) {
                snapshotReleases.remove(previous.taskId)
                return
            }
            if (ContentAnimator.running()) handler.postDelayed({ check() }, CHECK_MS)
            else {
                snapshotReleases.remove(previous.taskId)
                snapshots.unblock(previous.taskId)
                if (!previous.request.closing) snapshots.refresh(previous.service, previous.taskId)
            }
        }
        handler.postDelayed({ check() }, CHECK_MS)
    }

    override fun close() {
        synchronized(stateLock) {
            if (closed) return
            closed = true
            stopLocked("module-unloaded")
        }
        runCatching { hook?.unhook() }
        hook = null
        handler.removeCallbacksAndMessages(null)
        snapshotReleases.clear()
        snapshots.close()
        preStarting.close()
        worker.quitSafely()
    }

    private fun report(priority: Int, message: String, error: Throwable? = null) {
        if (error == null) module.log(priority, TAG, message)
        else module.log(priority, TAG, message, error)
    }

    private inner class Snapshot(
        val parent: Any, val content: Any?, val companions: List<Any>, val starting: Boolean,
        val width: Int, val height: Int, val activityToken: Any, val taskId: Int, val uid: Int,
    ) : AutoCloseable {
        val contentLayer = content?.let { surfaceLayer.invoke(it) as Int } ?: -1
        val companionLayers = companions.map { surfaceLayer.invoke(it) as Int }
        override fun close() { companions.forEach(::release); release(content); release(parent) }
    }

    private class Candidate(val window: Any, val parent: Any, val content: Any?,
        val activity: Any, val task: Any, val activityToken: Any, val taskId: Int, val uid: Int, val type: Int)

    private class Active(val generation: Long, val service: Any, val context: Context,
        val request: Request) {
        val parentLayer = request.leash
        var activityToken: Any? = null
        var taskId = -1
        var uid = -1
        var contentLayer = -1
        var companionLayers: List<Int> = emptyList()
        var starting = false
        var snapshotBlocked = false
        var width = 0
        var height = 0
    }

    private class Request(val operation: Int, val leash: Int, val containerLayer: Int,
        val taskId: Int, val closing: Boolean, val eventNs: Long, val geometry: FloatArray) {
        companion object {
            fun read(parcel: Parcel): Request? {
                if (parcel.readInt() != MAGIC || parcel.readInt() != VERSION) return null
                val operation = parcel.readInt()
                val leash = parcel.readInt()
                val containerLayer = parcel.readInt()
                val taskId = parcel.readInt()
                val direction = parcel.readInt()
                val eventNs = parcel.readLong()
                val geometry = FloatArray(12) { parcel.readFloat() }
                val age = System.nanoTime() - eventNs
                if (parcel.dataAvail() != 0 || operation !in TRANSITION..CANCEL ||
                    direction !in 0..1 || eventNs <= 0 || age !in -FUTURE_NS..MAX_EVENT_AGE_NS ||
                    geometry.any { !it.isFinite() }) return null
                if (operation == TRANSITION && (leash < 0 || taskId < -1 ||
                    (containerLayer < 0 && taskId < 0) ||
                    intArrayOf(2, 3, 6, 7).any { geometry[it] <= 0f || geometry[it] > MAX_EXTENT } ||
                    intArrayOf(0, 1, 4, 5).any { kotlin.math.abs(geometry[it]) > 32768f } ||
                    kotlin.math.abs(geometry[8]) > 100000f || kotlin.math.abs(geometry[9]) > 100000f ||
                    geometry[10] !in 0f..8192f || geometry[11] !in 0f..8192f)) return null
                if (operation == CANCEL && leash < -1) return null
                return Request(operation, leash, containerLayer, taskId, direction == 1, eventNs, geometry)
            }
        }
    }

    /** Cache both found and absent optional members across Binder/worker calls. */
    private class Access {
        private data class Key(val type: Class<*>, val name: String)
        private data class Member<T>(val value: T?)
        private val fields = ConcurrentHashMap<Key, Member<Field>>()
        private val methods = ConcurrentHashMap<Key, Member<Method>>()
        fun field(target: Any, name: String): Any? = fields.getOrPut(Key(target.javaClass, name)) {
            Member(find(target.javaClass) { it.getDeclaredField(name).apply { isAccessible = true } })
        }.value?.get(target)
        fun call(target: Any, name: String): Any? = methods.getOrPut(Key(target.javaClass, name)) {
            Member(find(target.javaClass) { it.getDeclaredMethod(name).apply { isAccessible = true } })
        }.value?.invoke(target)
        private fun <T> find(type: Class<*>, member: (Class<*>) -> T): T? {
            var current: Class<*>? = type
            while (current != null) {
                try { return member(current) }
                catch (_: NoSuchFieldException) { current = current.superclass }
                catch (_: NoSuchMethodException) { current = current.superclass }
            }
            return null
        }
    }

    private companion object {
        const val TAG = "HHA.Bridge"
        const val LAUNCHER = "com.miui.home"
        const val DESCRIPTOR = "android.view.IWindowManager"
        const val TRANSACTION = 0x484841
        const val MAGIC = 0x48484131
        const val VERSION = 2
        const val TRANSITION = 1
        const val CANCEL = 2
        const val MAX_EXTENT = 16384
        const val CANCEL_GRACE_MS = 48L
        const val RETRY_MS = 16L
        const val CHECK_MS = 50L
        const val SURFACE_WAIT_NS = 200_000_000L
        const val MAX_EVENT_AGE_NS = 500_000_000L
        const val FUTURE_NS = 100_000_000L
        const val SESSION_NS = 1_500_000_000L
        val SCALE_KEYS = arrayOf(Settings.Global.ANIMATOR_DURATION_SCALE,
            Settings.Global.WINDOW_ANIMATION_SCALE, Settings.Global.TRANSITION_ANIMATION_SCALE)
    }
}
