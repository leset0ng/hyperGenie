package ng.lst.genie

import android.annotation.SuppressLint
import android.graphics.Point
import android.graphics.Rect
import android.util.Log
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedInterface.HookHandle
import io.github.libxposed.api.XposedModule
import java.lang.ref.WeakReference
import java.lang.reflect.Method
import java.util.WeakHashMap
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.locks.ReentrantLock

/** Copies only OEM previews whose local domain matches the selected fullscreen window. */
@SuppressLint("PrivateApi", "BlockedPrivateApi", "SoonBlockedPrivateApi")
internal class PreStartingSurfaces(
    private val module: XposedModule,
    private val loader: ClassLoader,
    private val onChanged: () -> Unit = {},
) : AutoCloseable {
    /** Independently owned copies; neither object is borrowed from an OEM registry. */
    data class Preview(val surface: Any, val parent: Any, val identity: RegisteredIdentity) : AutoCloseable {
        override fun close() {
            runCatching { surface.javaClass.getMethod("release").invoke(surface) }
            runCatching { parent.javaClass.getMethod("release").invoke(parent) }
        }
    }
    /** Identity evidence must not extend the OEM animation's merged lifetime. */
    class RegisteredIdentity(
        animation: Any, val token: Long, producer: Any, val producerLayer: Int,
        val bufferLayer: Int, parent: Any, val parentLayer: Int, display: Any,
        val userId: Int, val width: Int, val height: Int,
    ) {
        val animation = WeakReference(animation)
        val producer = WeakReference(producer)
        val parent = WeakReference(parent)
        val display = WeakReference(display)

        private fun sameLiveObject(left: WeakReference<Any>, right: WeakReference<Any>): Boolean {
            val original = left.get() ?: return false
            return original === (right.get() ?: return false)
        }

        fun sameSource(other: RegisteredIdentity): Boolean =
            sameLiveObject(animation, other.animation) && token == other.token &&
                sameLiveObject(producer, other.producer) &&
                producerLayer == other.producerLayer && bufferLayer == other.bufferLayer &&
                sameLiveObject(parent, other.parent) && parentLayer == other.parentLayer &&
                sameLiveObject(display, other.display) &&
                userId == other.userId && width == other.width && height == other.height
    }
    /** The caller must retry without retiring its currently attached content. */
    class Busy : RuntimeException()

    private val members = OptionalMembers()
    private val surfaceClass = Class.forName("android.view.SurfaceControl")
    private val copySurface = surfaceClass.getConstructor(surfaceClass, String::class.java)
    private val surfaceValid = surfaceClass.getMethod("isValid")
    private val surfaceLayer = surfaceClass.getMethod("getLayerId")
    private val surfaceWidth = surfaceClass.getMethod("getWidth")
    private val surfaceHeight = surfaceClass.getMethod("getHeight")
    private val surfaceRelease = surfaceClass.getMethod("release")
    private val surfaceLock = surfaceClass.getDeclaredField("mLock").apply { isAccessible = true }
    private val managerFactory = factory("com.android.server.wm.PreStartingManager")
    private val controllerFactory = factory("com.android.server.wm.PreStartupAnimationController")
    private val controllerInstance = runCatching {
        controllerFactory?.declaringClass?.getDeclaredField("sInstance")?.apply { isAccessible = true }
    }.getOrNull()
    @Volatile private var manager: Any? = null
    @Volatile private var controller: Any? = null
    private val hooks = ArrayList<HookHandle>()
    private val launchTokens = ConcurrentHashMap<Any, Set<Long>>()
    private val currentLaunchTokens = ConcurrentHashMap<Any, Long>()
    private val registeredAnimations = ConcurrentHashMap<Long, Any>()
    private val openingExclusions = ConcurrentHashMap<Int, String>()
    // Registry keys outlive both Activity.mStartingData and the pending map.
    // Weak keys/references retain no Activity or StartingData after OEM removal.
    private data class ManagerOwner(
        val activity: WeakReference<Any>, val task: WeakReference<Any>,
        val display: WeakReference<Any>?, val frame: Rect?,
    )
    private val managerOwners = WeakHashMap<Any, ManagerOwner>()
    private data class PendingManagerData(
        val version: Long, val entries: List<Pair<WeakReference<Any>, WeakReference<Any>>>,
    )
    private val pendingManagerVersion = AtomicLong()
    private val pendingManagerChanges = AtomicInteger()
    @Volatile private var pendingManagerData = PendingManagerData(-1, emptyList())
    private val managerDiagnostics = ConcurrentHashMap<String, String>()

    fun install() {
        // WMS consumes mPendingRemoteTransition before some manager previews
        // show. Retain its exact token at the assignment event, not by package.
        runCatching {
            val activity = Class.forName("com.android.server.wm.ActivityRecord", false, loader)
            val options = activity.declaredMethods.single { it.name == "setOptions" && it.parameterCount == 1 }
            hooks += module.hook(options)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("prestarting-activity-transition-token")
                .intercept { chain ->
                    val owner = chain.thisObject
                    val argument = chain.getArg(0)
                    val remote = argument?.let { members.call(it, "getRemoteTransition") }
                    val token = remote?.let { members.call(it, "getHyperToken") as? Long }
                    val result = chain.proceed()
                    if (owner != null) recordCurrentLaunch(owner, token)
                    if (owner != null && token != null && token != 0L) {
                        // A reused Activity can receive newer options while an
                        // older opening is still being retargeted. Bind both by
                        // assignment; matching below still requires the exact
                        // live animation token and opening leash.
                        launchTokens.compute(owner) { _, old ->
                            (old.orEmpty() + token).toList().takeLast(MAX_DRAW_DOMAINS).toSet()
                        }
                        while (launchTokens.size > MAX_DRAW_DOMAINS) launchTokens.remove(launchTokens.keys.first())
                    }
                    result
                }
        }.onFailure { module.log(Log.WARN, TAG, "early Activity transition observer unavailable", it) }
        managerEvents()
        animationEvents()
        // prepare() queues this draw before constructing/starting the remote
        // animation. Record the exact planned identity domain before enqueue:
        // waiting for draw completion can reject an otherwise valid first t.
        runCatching {
            val type = Class.forName("com.android.server.wm.PreStartupSnapshotHandler", false, loader)
            val schedule = type.declaredMethods.single {
                it.name == "drawSnapshotAsync" && it.parameterCount == 5
            }
            hooks += module.hook(schedule)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("prestarting-scheduled-snapshot-domain")
                .intercept { chain ->
                    val surface = chain.getArg(1)
                    val bounds = identitySnapshot(surface, chain.getArg(2), chain.getArg(0), chain.getArg(4))
                    val layer = surface?.let { runCatching { layer(it) }.getOrNull() }
                    if (layer != null) recordDomain(layer, bounds)
                    try {
                        chain.proceed()
                    } catch (error: Throwable) {
                        if (layer != null) recordDomain(layer, null)
                        throw error
                    }
                }
        }.onFailure { module.log(Log.WARN, TAG, "scheduled snapshot domain observer unavailable", it) }
        // Recheck the actual draw in case rotation/domain changed while queued.
        runCatching {
            val type = Class.forName("com.android.server.wm.PreStartupSnapshotHandler", false, loader)
            val draw = type.declaredMethods.single {
                it.name == "drawSnapshotOnSurface" && it.parameterCount == 4
            }
            hooks += module.hook(draw)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("prestarting-snapshot-domain")
                .intercept { chain ->
                    val surface = chain.getArg(0)
                    val bounds = identitySnapshot(surface, chain.getArg(1), chain.getArg(2), chain.getArg(3))
                    val layer = surface?.let { runCatching { layer(it) }.getOrNull() }
                    if (layer != null) recordDomain(layer, bounds)
                    val result = try { chain.proceed() } catch (error: Throwable) {
                        if (layer != null) recordDomain(layer, null)
                        throw error
                    }
                    if (layer != null && (surface == null || this.layer(surface) != layer)) recordDomain(layer, null)
                    // Notify after releasing SC/domain locks. The callback only
                    // wakes the bridge worker; it must not run a WMS lookup here.
                    runCatching { onChanged() }
                    result
                }
        }.onFailure {
            module.log(Log.WARN, TAG, "opening snapshot domain observer unavailable; excluding unverified previews", it)
        }
    }

    private fun identitySnapshot(surface: Any?, snapshot: Any?, display: Any?, value: Any?): Rect? = runCatching {
        val bounds = (value as? Rect)?.let(::Rect) ?: return@runCatching null
        bounds.takeIf {
            surface != null && snapshot != null && display != null && layer(surface) >= 0 &&
                bounds.left == 0 && bounds.top == 0 && bounds.width() > 0 && bounds.width() < bounds.height() &&
                members.call(display, "getRotation") == 0 && members.call(display, "getBounds") == bounds &&
                members.call(snapshot, "getRotation") == 0 && members.call(snapshot, "isBufferValid") == true &&
                members.call(snapshot, "getHardwareBufferWidth") == bounds.width() &&
                members.call(snapshot, "getHardwareBufferHeight") == bounds.height()
        }
    }.getOrNull()

    private fun recordDomain(layer: Int, bounds: Rect?) = synchronized(drawDomains) {
        drawDomains.remove(layer)
        if (layer >= 0 && bounds != null) drawDomains[layer] = bounds
        while (drawDomains.size > MAX_DRAW_DOMAINS) drawDomains.remove(drawDomains.keys.first())
    }

    private fun animationEvents() {
        runCatching {
            val type = Class.forName("com.android.server.wm.PreStartupAnimationController", false, loader)
            val add = type.declaredMethods.single { it.name == "addAnimation" && it.parameterCount == 1 }
            hooks += module.hook(add)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("prestarting-registered-animation")
                .intercept { chain ->
                    val result = chain.proceed()
                    runCatching {
                        val animation = chain.getArg(0) ?: return@runCatching
                        val token = members.call(animation, "getToken") as? Long ?: return@runCatching
                        // addAnimation removes old entries when merging them
                        // into a successor, before their leashes are cleaned.
                        // Retain registration until that exact object's cleanup.
                        registeredAnimations[token] = animation
                        while (registeredAnimations.size > MAX_DRAW_DOMAINS)
                            registeredAnimations.remove(registeredAnimations.keys.first())
                    }
                    result
                }
            val animationType = Class.forName("com.android.server.wm.PreStartupAnimation", false, loader)
            val cleanup = animationType.declaredMethods.single { it.name == "cleanUp" && it.parameterCount == 1 }
            hooks += module.hook(cleanup)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("prestarting-animation-registration-retired")
                .intercept { chain ->
                    runCatching {
                        val animation = chain.thisObject ?: return@runCatching
                        val token = members.call(animation, "getToken") as? Long ?: return@runCatching
                        registeredAnimations.remove(token, animation)
                        launchTokens.keys.forEach { activity ->
                            launchTokens.computeIfPresent(activity) { _, current ->
                                (current - token).takeIf { it.isNotEmpty() }
                            }
                        }
                        members.call(animation, "getPreStartingSurface")?.let { recordDomain(layer(it), null) }
                    }
                    chain.proceed()
                }
        }.onFailure { module.log(Log.WARN, TAG, "animation registration observer unavailable", it) }
    }

    /** Concurrent registry plus objects it moved into merged-animation lifetime. */
    private fun animations(): List<Any> {
        val live = (controller?.let { members.call(it, "getAnimations") } as? Map<*, *>)
            ?.values?.filterNotNull().orEmpty()
        return (live + registeredAnimations.values).distinct().filter {
            (members.field(it, "mCleanedUp") as? AtomicBoolean)?.get() == false
        }
    }

    /** Observe exact OEM ownership before current/pending keys can be cleared. */
    private fun managerEvents() {
        val type = managerFactory?.declaringClass ?: return
        for (name in listOf("removePendingPreStartings", "removePending", "resetIfNeeded")) {
            runCatching {
                for (method in type.declaredMethods.filter { it.name == name }) {
                    hooks += module.hook(method)
                        .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                        .setId("prestarting-pending-version-$name-${method.parameterCount}")
                        .intercept { chain ->
                            pendingManagerChanges.incrementAndGet()
                            pendingManagerVersion.incrementAndGet()
                            try {
                                chain.proceed()
                            } finally {
                                pendingManagerVersion.incrementAndGet()
                                pendingManagerChanges.decrementAndGet()
                            }
                        }
                }
            }.onFailure { module.log(Log.WARN, TAG, "pending manager observer unavailable: $name", it) }
        }
        for ((name, count) in listOf(
            "makePreColorIfNeeded" to 4, "resetIfNeeded" to 3, "reset" to 4,
        )) {
            runCatching {
                val method = type.declaredMethods.single {
                    it.name == name && it.parameterCount == count
                }
                hooks += module.hook(method)
                    .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                    .setId("prestarting-manager-owner-$name")
                    .intercept { chain ->
                        runCatching {
                            val activity = chain.getArg(0)
                            val data = chain.getArg(1)
                            if (activity != null && data != null) rememberManagerOwner(activity, data)
                        }
                        chain.proceed()
                    }
            }.onFailure { module.log(Log.WARN, TAG, "manager owner observer unavailable: $name", it) }
        }
        runCatching {
            val transfer = type.declaredMethods.single {
                it.name == "transferPreStarting" && it.parameterCount == 2
            }
            hooks += module.hook(transfer)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("prestarting-manager-owner-transfer")
                .intercept { chain ->
                    val result = chain.proceed()
                    runCatching {
                        val activity = chain.getArg(1) ?: return@runCatching
                        val data = members.field(activity, "mStartingData") ?: return@runCatching
                        val stub = members.field(data, "mStub") ?: return@runCatching
                        // Only this branch actually reparents the registry
                        // surfaces; plain key assignment alone proves no move.
                        if (members.call(stub, "getForceAdded") == true)
                            rememberManagerOwner(activity, data, transferred = true)
                    }
                    result
                }
        }.onFailure { module.log(Log.WARN, TAG, "manager transfer observer unavailable", it) }
    }

    private fun rememberManagerOwner(activity: Any, data: Any, transferred: Boolean = false) {
        val task = members.call(activity, "getTask") ?: return
        val frame = (members.call(activity, "getBounds") as? Rect)?.let(::Rect)
        val display = frame?.let { identityDisplay(activity, task, it, it.width(), it.height()) }
        synchronized(managerOwners) {
            val previous = managerOwners[data]
            if (!transferred && previous != null) return
            managerOwners[data] = ManagerOwner(WeakReference(activity), WeakReference(task),
                display?.let { WeakReference(it) }, frame.takeIf { display != null })
        }
    }

    /** Resolve OEM singleton/pending-map locks before the caller enters mGlobalLock. */
    fun prepare() {
        manager = managerFactory?.invoke(null)
        controller = controllerFactory?.invoke(null)
        val pending = manager?.let { members.field(it, "mPendingRemoveStartingData") } as? Map<*, *>
        // ROM nests pending-map -> surface lock. Never take this monitor in
        // copy(), where the caller already owns WMS.mGlobalLock.
        val version = pendingManagerVersion.get()
        val entries = pending?.let { synchronized(it) { it.entries.map { entry -> entry.key to entry.value } } }
            .orEmpty()
        entries.forEach { (activity, data) ->
            if (activity != null && data != null) rememberManagerOwner(activity, data)
        }
        pendingManagerData = PendingManagerData(version, entries.mapNotNull { (activity, data) ->
            if (activity == null || data == null) null else WeakReference(activity) to WeakReference(data)
        }.takeIf { pendingManagerChanges.get() == 0 && pendingManagerVersion.get() == version }.orEmpty())
    }

    /** Called after every successful options assignment, including tokenless clearing. */
    private fun recordCurrentLaunch(activity: Any, token: Long?) {
        if (token == null || token == 0L) currentLaunchTokens.remove(activity)
        else currentLaunchTokens[activity] = token
        while (currentLaunchTokens.size > MAX_DRAW_DOMAINS)
            currentLaunchTokens.keys.firstOrNull()?.let(currentLaunchTokens::remove)
    }

    /** Latest assigned launch only; the retained historical token set cannot authorize a close. */
    fun launchToken(activity: Any): Long? = currentLaunchTokens[activity]

    /** Bind an early request using the OEM's exact remote-transition token. */
    fun matchesOpening(activity: Any, leash: Int, width: Int, height: Int): Boolean {
        val remote = members.field(activity, "mPendingRemoteTransition")
        val pendingToken = remote?.let { members.call(it, "getHyperToken") as? Long }
        val tokens = launchTokens[activity].orEmpty() + listOfNotNull(pendingToken).filter { it != 0L }
        if (tokens.isEmpty()) return false
        val task = members.call(activity, "getTask") ?: return false
        val frame = Rect(0, 0, width, height)
        val display = identityDisplay(activity, task, frame, width, height) ?: return false
        val animation = animations().singleOrNull {
            (members.call(it, "getToken") as? Long ?: 0L) in tokens && openingChange(it, leash, frame) != null
        } ?: return false
        return members.field(animation, "mDisplayContent") === display &&
            members.call(animation, "getUserId") == members.field(activity, "mUserId")
    }

    /** Called outside WMS/manager locks, before a Task anchor exists. */
    fun opening(leash: Int, width: Int, height: Int): Preview? {
        if (leash < 0 || width <= 0 || height <= width) return unavailable(leash, "request-domain")
        // Initialization may finish after renderer warmup. Read the OEM's
        // volatile singleton directly; never acquire its class/WMS monitor in
        // the synchronous pre-start capability endpoint.
        if (controller == null) controller = controllerInstance?.get(null)
        if (controller == null) return unavailable(leash, "controller-not-ready")
        val matches = animations().filter { animation ->
            val parent = members.field(animation, "mOpeningLeash")
            parent != null && runCatching { layer(parent) == leash }.getOrDefault(false)
        }
        val animation = matches.singleOrNull() ?: return unavailable(leash, "registered-matches=${matches.size}")
        val token = (members.call(animation, "getToken") as? Long)?.takeIf { it > 0L }
            ?: return unavailable(leash, "missing-token")
        val userId = members.call(animation, "getUserId") as? Int
            ?: return unavailable(leash, "missing-user")
        val frame = Rect(0, 0, width, height)
        val display = members.field(animation, "mDisplayContent") ?: return unavailable(leash, "missing-display")
        if (members.call(display, "getRotation") != 0 || members.call(display, "getBounds") != frame)
            return unavailable(leash, "display-domain")
        if (openingChange(animation, leash, frame) == null) return unavailable(leash, "transition-domain")
        val source = alignedOpening(animation, leash, frame) ?: return unavailable(leash,
            "buffer-domain type=${members.call(animation, "getPreStartingType")}")
        val parent = members.field(animation, "mOpeningLeash") ?: return unavailable(leash, "missing-parent")
        val copies = ArrayList<Any>(1)
        var parentCopy: Any? = null
        try {
            addCopy(source, width, height, false, mutableSetOf(), copies)
            val surfaceCopy = copies.singleOrNull() ?: return unavailable(leash, "surface-extent")
            synchronized(checkNotNull(surfaceLock.get(parent))) {
                if (surfaceValid.invoke(parent) != true || surfaceLayer.invoke(parent) != leash)
                    return unavailable(leash, "parent-retired")
                parentCopy = copySurface.newInstance(parent, "Genie.central.prestarting.parent")
            }
            val identity = RegisteredIdentity(animation, token, source, layer(source), layer(surfaceCopy),
                parent, leash, display, userId, width, height)
            return Preview(surfaceCopy, checkNotNull(parentCopy), identity).also {
                openingExclusions.remove(leash)
                copies.clear()
                parentCopy = null
            }
        } finally {
            copies.forEach(::release)
            parentCopy?.let(::release)
        }
    }

    /** A close must retain the same registered producer and the Activity's latest launch. */
    fun closing(identity: RegisteredIdentity, activity: Any, task: Any): Preview? {
        fun reject(reason: String): Preview? {
            module.log(Log.DEBUG, TAG, "registered close rejected leash=${identity.parentLayer} " +
                "token=${identity.token} current=${launchToken(activity)} producer=${identity.producerLayer} " +
                "buffer=${identity.bufferLayer} reason=$reason")
            return null
        }
        if (launchToken(activity) != identity.token) return reject("launch-token")
        val animation = identity.animation.get() ?: return reject("animation-collected")
        val producer = identity.producer.get() ?: return reject("producer-collected")
        val parent = identity.parent.get() ?: return reject("parent-collected")
        val display = identity.display.get() ?: return reject("display-collected")
        val frame = Rect(0, 0, identity.width, identity.height)
        if (identityDisplay(activity, task, frame, identity.width, identity.height) !== display)
            return reject("activity-task-display-domain")
        if (members.field(activity, "mUserId") != identity.userId) return reject("user")
        val preview = opening(identity.parentLayer, identity.width, identity.height)
            ?: return reject("registration-or-buffer-domain")
        if (preview.identity.animation.get() !== animation || preview.identity.producer.get() !== producer ||
            preview.identity.parent.get() !== parent || preview.identity.display.get() !== display ||
            !identity.sameSource(preview.identity)) {
            val live = preview.identity
            preview.close()
            return reject("producer-changed liveToken=${live.token} liveProducer=${live.producerLayer} " +
                "liveBuffer=${live.bufferLayer}")
        }
        return preview
    }

    private fun unavailable(leash: Int, reason: String): Preview? {
        if (openingExclusions.put(leash, reason) != reason)
            module.log(Log.DEBUG, TAG, "opening excluded leash=$leash reason=$reason")
        while (openingExclusions.size > MAX_DRAW_DOMAINS) openingExclusions.remove(openingExclusions.keys.first())
        return null
    }

    /** Exact opening ownership and identity-domain proof, shared by both lookup paths. */
    private fun openingChange(animation: Any, leash: Int, frame: Rect?): Any? {
        if (frame == null) return null
        val parent = members.field(animation, "mOpeningLeash") ?: return null
        if (layer(parent) != leash) return null
        // OEM parallel and nonparallel paths reparent the same local-domain
        // producer directly into their own opening leash. isParallel changes
        // outer root/closing-leash ownership, not this producer mapping. The
        // common OPEN Change proves the domain; visual rotation still fails
        // the explicit rotation checks below.
        val info = members.field(animation, "mTransitionInfo") ?: return null
        val changes = members.call(info, "getChanges") as? List<*> ?: return null
        val opening = changes.filterNotNull().singleOrNull { change ->
            val control = members.call(change, "getLeash")
            control != null && runCatching { layer(control) == leash }.getOrDefault(false)
        } ?: return null
        return opening.takeIf {
            members.call(opening, "getMode") == 1 &&
                members.call(opening, "getEndRelOffset") == Point(0, 0) &&
                members.call(opening, "getStartRotation") == 0 &&
                members.call(opening, "getEndRotation") == 0 &&
                members.call(opening, "getStartAbsBounds") == frame &&
                members.call(opening, "getEndAbsBounds") == frame
        }
    }

    private fun alignedOpening(animation: Any, leash: Int, frame: Rect?): Any? {
        openingChange(animation, leash, frame) ?: return null
        val surface = members.call(animation, "getPreStartingSurface") ?: return null
        val type = members.call(animation, "getPreStartingType")
        // Snapshot may rotate or scale its SC after allocation. Splash paints
        // a full-size Canvas and rotates only the pixels inside that domain.
        return surface.takeIf {
            type == PRESTART_SPLASH ||
                (type == PRESTART_SNAPSHOT && observedDomain(layer(surface)) == frame)
        }
    }

    /** Caller holds WMS.mGlobalLock. Returned surfaces are exclusively owned copies. */
    fun copy(activity: Any, task: Any, frame: Rect?, width: Int, height: Int,
             leash: Int, excludedLayers: Set<Int>): List<Any> {
        val current = members.field(activity, "mStartingData")
        val display = identityDisplay(activity, task, frame, width, height) ?: run {
            managerDiagnostic(activity, task, current, current, -1, "request-domain")
            return emptyList()
        }
        val owned = ArrayList<Any>(3)
        val seen = excludedLayers.toMutableSet()
        try {
            if (current != null) rememberManagerOwner(activity, current)
            val currentManager = manager
            if (currentManager != null) {
                val lock = members.field(currentManager, "mSurfaceLock") as? ReentrantLock ?: throw Busy()
                if (!lock.tryLock()) throw Busy()
                try {
                    val snapshots = members.field(currentManager, "mPreSurfaceControlMap") as? Map<*, *> ?: throw Busy()
                    val colors = members.field(currentManager, "mColorSurfaceControlMap") as? Map<*, *> ?: throw Busy()
                    val positions = members.field(currentManager, "mPreSurfaceInitPosMap") as? Map<*, *>
                    val scales = members.field(currentManager, "mPreSurfaceScaleMap") as? Map<*, *>
                    // Match showPreStarting's selected key. Historical registry
                    // entries can remain visible while an older launch retires;
                    // they must not acquire the next launch's motion.
                    val pending = pendingManagerData
                    val selected = current ?: pending.takeIf {
                        pendingManagerChanges.get() == 0 && pending.version == pendingManagerVersion.get()
                    }?.entries?.firstOrNull { it.first.get() === activity }?.second?.get()
                    val owner = synchronized(managerOwners) {
                        managerOwners.entries.removeAll { (data, _) ->
                            !snapshots.containsKey(data) && !colors.containsKey(data)
                        }
                        managerOwners[selected]?.takeIf { it.activity.get() === activity }
                    }
                    if (selected == null || owner == null) {
                        managerDiagnostic(activity, task, current, selected, -1, "no-current-registry-key")
                    } else {
                        val data = selected
                        val color = colors[data]
                        val colorLayer = color?.let(::layer) ?: -1
                        if (owner.task.get() !== task || owner.display?.get() !== display || owner.frame != frame) {
                            managerDiagnostic(activity, task, current, data, colorLayer, "owner-domain")
                        } else {
                            if (positions != null && scales != null &&
                                identityPair(positions[data], 0f) && identityPair(scales[data], 1f)) {
                                val snapshot = members.call(data, "getSnapshot")
                                // A cached TaskSnapshot can outlive its HardwareBuffer.
                                // Its expired metadata must not discard the current color layer.
                                val snapshotMatches = try {
                                    snapshot != null && members.call(snapshot, "getRotation") == 0 &&
                                        members.call(snapshot, "getHardwareBufferWidth") == width &&
                                        members.call(snapshot, "getHardwareBufferHeight") == height
                                } catch (_: Exception) {
                                    false
                                }
                                if (snapshotMatches) {
                                    addCopy(snapshots[data], width, height, false, seen, owned)
                                }
                            }
                            // Color is independent of snapshot buffer availability.
                            val result = addCopy(color, width, height, true, seen, owned)
                            managerDiagnostic(activity, task, current, data, colorLayer, result)
                        }
                    }
                } finally {
                    lock.unlock()
                }
            }
            val matches = animations().filter { animation ->
                val opening = members.field(animation, "mOpeningLeash")
                opening != null && runCatching { layer(opening) == leash }.getOrDefault(false)
            }
            val animation = matches.singleOrNull()
            if (animation != null && members.field(animation, "mDisplayContent") === display) {
                addCopy(alignedOpening(animation, leash, frame), width, height, false, seen, owned)
            }
            return owned
        } catch (error: Throwable) {
            owned.forEach(::release)
            throw error
        }
    }

    private fun identityDisplay(activity: Any, task: Any, frame: Rect?, width: Int, height: Int): Any? {
        if (width <= 0 || height <= width || frame != Rect(0, 0, width, height) ||
            members.call(activity, "getTask") !== task ||
            members.call(task, "getWindowingMode") != 1 ||
            members.call(activity, "getWindowingMode") != 1 ||
            members.call(activity, "isEmbedded") != false ||
            members.call(activity, "hasFixedRotationTransform") != false ||
            members.call(activity, "isLetterboxedForFixedOrientationAndAspectRatio") != false ||
            members.call(activity, "getBounds") != frame || members.call(task, "getBounds") != frame) return null
        val display = members.call(activity, "getDisplayContent") ?: return null
        return display.takeIf {
            members.call(it, "getRotation") == 0 && members.call(it, "getBounds") == frame
        }
    }

    private fun identityPair(value: Any?, expected: Float): Boolean {
        if (value == null) return true // OEM omits entries for its identity defaults.
        val pair = value as? FloatArray ?: return false
        return pair.size == 2 && pair.all { it.isFinite() && it == expected }
    }

    private fun addCopy(source: Any?, width: Int, height: Int, color: Boolean,
                        seen: MutableSet<Int>, owned: MutableList<Any>): String {
        if (source == null) return "no-color"
        synchronized(checkNotNull(surfaceLock.get(source))) {
            if (surfaceValid.invoke(source) != true) return "surface-retired"
            val layer = surfaceLayer.invoke(source) as Int
            if (layer < 0) return "invalid-layer"
            if (layer in seen) return "already-covered"
            val actualWidth = surfaceWidth.invoke(source) as Int
            val actualHeight = surfaceHeight.invoke(source) as Int
            if ((actualWidth != width || actualHeight != height) &&
                !(color && actualWidth <= 0 && actualHeight <= 0)) return "extent=${actualWidth}x$actualHeight"
            // A manager color has no producer/BBQ child and no buffer extent.
            // Copy its exact registry SC. Buffer previews still resolve BBQ
            // from the original producer before copying away its Java owner.
            val copy = if (color) copySurface.newInstance(source, "Genie.central.prestarting.color")
                else ContentSurfaces.copy(source, "Genie.central.prestarting", width, height)
            try {
                val contentLayer = this.layer(copy)
                if (contentLayer < 0 || contentLayer in seen) {
                    release(copy)
                    return "already-covered"
                }
                owned += copy
                seen += contentLayer
                return "copied"
            } catch (error: Throwable) {
                release(copy)
                throw error
            }
        }
    }

    private fun managerDiagnostic(activity: Any, task: Any, current: Any?, data: Any?, layer: Int, reason: String) {
        fun identity(value: Any?) = value?.let { Integer.toHexString(System.identityHashCode(it)) } ?: "null"
        val key = "${identity(activity)}:${identity(data)}:$layer"
        val message = "manager color activity=${identity(activity)} task=${identity(task)} " +
            "current=${identity(current)} key=${identity(data)} layer=$layer reason=$reason"
        if (managerDiagnostics.put(key, message) != message) module.log(Log.DEBUG, TAG, message)
        while (managerDiagnostics.size > MAX_DRAW_DOMAINS) {
            managerDiagnostics.keys.firstOrNull()?.let { managerDiagnostics.remove(it) }
        }
    }

    private fun layer(surface: Any): Int = synchronized(checkNotNull(surfaceLock.get(surface))) {
        if (surfaceValid.invoke(surface) == true) surfaceLayer.invoke(surface) as Int else -1
    }

    private fun release(surface: Any) { runCatching { surfaceRelease.invoke(surface) } }

    override fun close() {
        hooks.asReversed().forEach { runCatching { it.unhook() } }
        hooks.clear()
        launchTokens.clear()
        currentLaunchTokens.clear()
        registeredAnimations.clear()
        openingExclusions.clear()
        managerDiagnostics.clear()
        synchronized(managerOwners) { managerOwners.clear() }
        pendingManagerData = PendingManagerData(-1, emptyList())
        synchronized(drawDomains) { drawDomains.clear() }
    }

    private fun factory(name: String): Method? = runCatching {
        Class.forName(name, false, loader).getDeclaredMethod("getInstance").apply { isAccessible = true }
    }.getOrNull()

    companion object {
        private const val TAG = "Genie.PreStarting"
        private const val PRESTART_SNAPSHOT = 1
        private const val PRESTART_SPLASH = 2
        private const val MAX_DRAW_DOMAINS = 64
        private val drawDomains = LinkedHashMap<Int, Rect>()

        /** Shared with show observers: verified scheduled or completed identity draws only. */
        fun observedDomain(layer: Int): Rect? = synchronized(drawDomains) {
            drawDomains[layer]?.let(::Rect)
        }
    }
}
