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
import java.util.concurrent.atomic.AtomicBoolean

/** System-server-only content ownership bridge. No hooks are installed in target Apps. */
@SuppressLint("PrivateApi", "BlockedPrivateApi", "SoonBlockedPrivateApi")
internal class CentralBridge(private val module: XposedModule, loader: ClassLoader) : AutoCloseable {
    private val access = Access()
    private val snapshots = SnapshotGuard(module, loader)
    private val preStarting = PreStartingSurfaces(module, loader, ::surfacesChanged)
    private val events = SurfaceEvents(module, loader, ::surfaceShowing, ::surfacesChanged)
    private val snapshotReleases = HashMap<Int, SnapshotRelease>()
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
    private val nativeWriteSurface = runCatching {
        scClass.getDeclaredMethod("nativeWriteToParcel", Long::class.javaPrimitiveType,
            Parcel::class.java).apply { isAccessible = true }
    }.getOrNull()
    private val transactionClass = Class.forName("android.view.SurfaceControl\$Transaction")
    private val transactionObject = transactionClass.getDeclaredField("mNativeObject").apply { isAccessible = true }
    private val newTransaction = transactionClass.getConstructor()
    private val applyTransaction = transactionClass.getMethod("apply")
    private val closeTransaction = transactionClass.getMethod("close")
    private val requests = ConcurrentHashMap<Int, Envelope>()
    private val owners = ConcurrentHashMap<Long, Owner>()
    private val earlyPublications = ConcurrentHashMap<Int, Long>()
    private val sessions = LinkedHashMap<Int, Active>() // Bridge worker only.
    private val lastEvents = HashMap<Int, Long>()
    private val changeQueued = AtomicBoolean()
    private val worker = HandlerThread("HHA.Bridge").apply { start() }
    private val handler = Handler(worker.looper)
    private var hook: HookHandle? = null
    private var generation = 0L
    @Volatile private var closed = false
    @Volatile private var rendererReady = false

    fun install() {
        check(!closed && hook == null)
        try {
            snapshots.install()
            preStarting.install()
            events.install()
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
            var prepared = false
            val accepted = runCatching {
                input.enforceInterface(DESCRIPTOR)
                val request = Request.read(input) ?: return@runCatching false
                if (closed) return@runCatching false
                if (request.operation == PREPARE_PREVIEW) {
                    prepared = preparePreview(service, context, request, reply)
                    return@runCatching prepared
                }
                if (!rendererReady && request.operation != CANCEL) {
                    handler.post { prepareRenderer() }
                    return@runCatching false
                }
                // Publish immutable authenticated geometry before the worker
                // runs, so a concurrent first-show hook can join the same t.
                if (request.operation == TRANSITION) {
                    publish(Envelope(service, context, request))
                }
                handler.post { process(service, context, request) }
            }.onFailure {
                report(Log.WARN, "request rejected; keeping stock animation", it)
            }.getOrDefault(false)
            if (!prepared) {
                reply.writeNoException()
                reply.writeInt(if (accepted) 1 else 0)
            }
        } catch (error: Throwable) {
            report(Log.ERROR, "bridge transaction failed", error)
            reply.writeNoException()
            reply.writeInt(0)
        } finally {
            Binder.restoreCallingIdentity(identity)
        }
    }

    private fun publish(next: Envelope) {
        requests.compute(next.request.leash) { _, old ->
            when {
                old == null || old.request.eventNs < next.request.eventNs -> next
                old.request.eventNs > next.request.eventNs -> old
                old.request.taskId >= 0 -> old // Never downgrade an established Task anchor.
                else -> next
            }
        }
    }

    /** Exact pre-start capability only; no WMS lock, native render or worker wait. */
    private fun preparePreview(service: Any, context: Context, request: Request, reply: Parcel): Boolean {
        val writer = nativeWriteSurface ?: return false
        if (!rendererReady || !animationsEnabled(context) || request.closing ||
            (request.taskId < 0 && request.containerLayer != request.leash)) return false
        val width = request.geometry[6].toInt()
        val height = request.geometry[7].toInt()
        preStarting.opening(request.leash, width, height)?.use { preview ->
            val encoded = Parcel.obtain()
            try {
                synchronized(checkNotNull(surfaceLock.get(preview.surface))) {
                    writer.invoke(null, nativeObject.getLong(preview.surface), encoded)
                }
                val transition = Request(TRANSITION, request.leash, request.containerLayer,
                    request.taskId, request.closing, request.eventNs, request.geometry)
                publish(Envelope(service, context, transition))
                handler.post { process(service, context, transition) }
                reply.writeNoException()
                reply.writeInt(1)
                reply.appendFrom(encoded, 0, encoded.dataSize())
                return true
            } finally { encoded.recycle() }
        }
        return false
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

    private fun cancel(request: Request) {
        if (request.leash == -1) {
            requests.clear()
            earlyPublications.clear()
            sessions.values.toList().forEach { stop(it, "disabled") }
            ContentAnimator.stop()
            return
        }
        val current = sessions[request.leash] ?: return
        requests.computeIfPresent(request.leash) { _, entry ->
            entry.takeIf { it.request.eventNs > request.eventNs }
        }
        handler.postDelayed({
            if (sessions[request.leash] === current &&
                (lastEvents[request.leash] ?: 0L) <= request.eventNs) stop(current, "cancelled")
        }, CANCEL_GRACE_MS)
    }

    private fun start(match: Snapshot, request: Request, service: Any): Boolean {
        val content = match.content ?: return false
        // Capture pristine content before a close can deform it. Skip recording
        // while any prior generation still has an uncommitted detach.
        if (request.closing && !match.starting && !ContentAnimator.running())
            snapshots.refresh(service, match.taskId)
        if (!snapshots.block(match.taskId, request.eventNs)) return false
        if ((snapshotReleases[match.taskId]?.eventNs ?: 0L) <= request.eventNs)
            snapshotReleases.remove(match.taskId)
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
        preStarting.prepare()
        rendererReady = runCatching { ContentAnimator.initialize() }.onFailure {
            report(Log.WARN, "native renderer unavailable; keeping stock animation", it)
        }.getOrDefault(false)
    }

    private fun process(service: Any, context: Context, request: Request) {
        if (closed || request.eventNs < (lastEvents[request.leash] ?: 0L)) return
        lastEvents[request.leash] = request.eventNs
        if (request.operation == CANCEL) {
            cancel(request)
            return
        }
        val resolved = requests[request.leash]?.request?.takeIf {
            it.eventNs == request.eventNs && it.taskId >= 0
        } ?: request
        val previous = sessions[request.leash]
        if (previous != null && previous.request.eventNs == resolved.eventNs &&
            previous.request.taskId == resolved.taskId && previous.request.containerLayer == resolved.containerLayer) {
            checkCurrent(previous)
            return
        }
        val next = Active(++generation, service, context, resolved)
        if (previous != null) {
            next.activityToken = previous.activityToken
            next.taskId = previous.taskId
            next.uid = previous.uid
            next.task = previous.task
            next.nativeParent = previous.nativeParent
            next.contentLayer = previous.contentLayer
            next.companionLayers = previous.companionLayers
            next.starting = previous.starting
            next.width = previous.width
            next.height = previous.height
            next.snapshotBlocked = previous.snapshotBlocked
        }
        sessions[request.leash] = next
        checkCurrent(next)
        // A single deadline is a cleanup watchdog, never a content lookup clock.
        val remaining = ((SESSION_NS - (System.nanoTime() - request.eventNs)) / 1_000_000L).coerceAtLeast(1L)
        handler.postDelayed({ if (sessions[request.leash] === next) stop(next, "watchdog") }, remaining)
    }

    private fun surfacesChanged() {
        if (closed || !changeQueued.compareAndSet(false, true)) return
        handler.post {
            changeQueued.set(false)
            if (!closed) sessions.values.toList().forEach(::checkCurrent)
        }
    }

    /** Events wake this lookup. No retry or periodic surface discovery timer. */
    private fun checkCurrent(current: Active) {
        if (closed || sessions[current.parentLayer] !== current) return
        val updated = requests[current.parentLayer]?.request
        if (updated != null && updated.eventNs == current.request.eventNs &&
            current.request.taskId < 0 && updated.taskId >= 0) {
            process(current.service, current.context, updated)
            return
        }
        try {
            if (!animationsEnabled(current.context)) {
                stop(current, "disabled")
                ContentAnimator.stop()
                return
            }
            val match = lookup(current.service, current.request)
            if (match == null) {
                if (!current.request.closing && current.contentLayer < 0) startOpeningPreview(current)
                // Creation, first draw, anchor and removal events drive the next
                // check. A transient empty tree does not prove visible removal.
                return
            }
            match.use {
                if (current.taskId >= 0 && (it.taskId != current.taskId || it.uid != current.uid ||
                        it.task !== current.task || (it.content != null && current.width > 0 &&
                        (it.width != current.width || it.height != current.height)))) {
                    stop(current, "owner-changed")
                    return
                }
                if (current.activityToken != null && it.activityToken !== current.activityToken &&
                    !verifiedActivityHandoff(current, it)) {
                    stop(current, "owner-changed")
                    return
                }
                val changed = current.contentLayer != it.contentLayer ||
                    current.companionLayers != it.companionLayers || current.nativeParent != it.parentLayer ||
                    current.appliedEventNs != current.request.eventNs
                if (it.content != null && changed) {
                    if (!start(it, current.request, current.service)) {
                        stop(current, "native-rejected")
                        return
                    }
                    report(Log.INFO, "attached parent=${current.parentLayer} content=${it.contentLayer} " +
                        "companions=${it.companionLayers} starting=${it.starting} " +
                        "task=${it.taskId} uid=${it.uid} closing=${current.request.closing}")
                }
                owners[current.request.eventNs] = Owner(it.task, it.uid, it.width, it.height)
                current.activityToken = it.activityToken
                current.taskId = it.taskId
                current.task = it.task
                current.uid = it.uid
                if (it.content != null) rememberLayers(current, it)
                // A newer verified request for the same actual Task supersedes
                // discovery only; native preserves same-layer state and guards.
                sessions.values.filter { old -> old !== current && old.task === it.task &&
                    old.request.eventNs < current.request.eventNs }.toList().forEach { old ->
                    sessions.remove(old.parentLayer)
                    requests.computeIfPresent(old.parentLayer) { _, envelope ->
                        envelope.takeIf { entry -> entry.request.eventNs > old.request.eventNs }
                    }
                }
            }
        } catch (_: PreStartingSurfaces.Busy) {
            // A later OEM lifecycle event retries after lock contention.
        } catch (error: Throwable) {
            report(Log.WARN, "lifecycle check failed; releasing effect", error)
            stop(current, "lookup-failed")
        }
    }

    private fun startOpeningPreview(current: Active) {
        val request = current.request
        val full = if (request.closing) 0 else 4
        val width = request.geometry[full + 2].toInt()
        val height = request.geometry[full + 3].toInt()
        preStarting.opening(request.leash, width, height)?.use { preview ->
            val content = nativeObject.getLong(preview.surface)
            val parent = nativeObject.getLong(preview.parent)
            if (ContentAnimator.transition(content, parent, width, height, request.geometry,
                    false, request.eventNs, starting = true)) {
                current.contentLayer = surfaceLayer.invoke(preview.surface) as Int
                current.nativeParent = surfaceLayer.invoke(preview.parent) as Int
                current.starting = true
                current.width = width
                current.height = height
                report(Log.INFO, "early preview parent=${request.leash} content=${current.contentLayer}")
            }
        }
    }

    private fun verifiedActivityHandoff(current: Active, next: Snapshot): Boolean =
        !current.request.closing && current.task != null && current.task === next.task &&
            current.taskId == next.taskId && current.uid == next.uid &&
            current.nativeParent == next.parentLayer && next.identityDomain &&
            (current.width == 0 || (current.width == next.width && current.height == next.height))

    private fun rememberLayers(current: Active, match: Snapshot) {
        current.appliedEventNs = current.request.eventNs
        current.contentLayer = match.contentLayer
        current.nativeParent = match.parentLayer
        current.companionLayers = match.companionLayers
        current.starting = match.starting
        current.snapshotBlocked = true
        current.width = match.width
        current.height = match.height
    }

    /** Runs at content creation/attachment or show, without waiting on bridge/WM work. */
    private fun surfaceShowing(show: SurfaceEvents.Show) {
        if (closed || !rendererReady || requests.isEmpty()) return
        runCatching {
            val now = System.nanoTime()
            val entries = requests.values.filter { now - it.request.eventNs in 0..SESSION_NS }
            val activity = show.activity
            val task = activity?.let { access.call(it, "getTask") }
            val taskId = task?.let { access.field(it, "mTaskId") as? Int }
            val uid = activity?.let { access.field(it, "info") as? android.content.pm.ActivityInfo }
                ?.applicationInfo?.uid
            val envelope = entries.filter { entry ->
                val request = entry.request
                if (show.leash >= 0) request.leash == show.leash
                else activity != null && task != null && uid != null &&
                    ((taskId == request.taskId && (request.containerLayer < 0 ||
                        isLayer(access.field(task, "mSurfaceControl"), request.containerLayer) ||
                        (show.window != null && matchingParent(show.window, request.containerLayer) != null))) ||
                        (request.taskId < 0 && preStarting.matchesOpening(activity, request.leash,
                            request.geometry[6].toInt(), request.geometry[7].toInt())))
            }.maxByOrNull { it.request.eventNs } ?: return
            val request = envelope.request
            if (request.closing) return // Newly opening buffers are never attached to an old close.
            val full = 4
            val width = request.geometry[full + 2].toInt()
            val height = request.geometry[full + 3].toInt()
            val frame = Rect(0, 0, width, height)
            var parent = show.parent
            if (show.leash < 0) {
                if (activity == null || task == null || taskId == null || uid == null ||
                    !identityDomain(activity, task, frame, width, height)) return
                val owner = owners[request.eventNs]
                if (owner != null && (owner.task !== task || owner.uid != uid)) return
                val window = show.window
                if (window != null) {
                    val attrs = access.field(window, "mAttrs") as? WindowManager.LayoutParams ?: return
                    if (attrs.type !in 1..3 || attrs.packageName == LAUNCHER ||
                        attrs.flags and WindowManager.LayoutParams.FLAG_SECURE != 0 ||
                        (attrs.type != 3 && access.field(window, "mOwnerUid") != uid) ||
                        access.call(window, "getFrame") != frame) return
                } else {
                    // OEM manager callbacks carry no WindowState. Reuse only
                    // an identity already validated by a WM-owned lookup.
                    if (owner == null && request.taskId >= 0) return
                    if (owner != null && owner.width > 0 && (owner.width != width || owner.height != height)) return
                }
                parent = access.field(task, "mSurfaceControl") ?: return
                if (request.taskId < 0) {
                    // The server-owned remote token proves this Task belongs to
                    // the early opening before Shell's later reparent callback.
                    val bound = Request(request.operation, request.leash, surfaceLayer.invoke(parent) as Int,
                        taskId, request.closing, request.eventNs, request.geometry)
                    val next = Envelope(envelope.service, envelope.context, bound)
                    var published = false
                    requests.compute(request.leash) { _, old ->
                        if (old?.request?.eventNs == request.eventNs && old.request.taskId < 0) {
                            published = true
                            next
                        } else old
                    }
                    if (published) handler.post { process(next.service, next.context, bound) }
                }
                if (!snapshots.tryBlock(taskId, request.eventNs)) return
                val blockedTask = taskId
                val blockedParent = surfaceLayer.invoke(parent) as Int
                handler.post {
                    // Publication may be the only moment ownership is visible:
                    // cancellation can remove the window before worker lookup.
                    val current = sessions[request.leash]?.takeIf { it.request.eventNs == request.eventNs }
                    if (current != null && (current.taskId < 0 || current.taskId == blockedTask)) {
                        current.taskId = blockedTask
                        current.task = task
                        current.uid = uid
                        current.nativeParent = blockedParent
                        current.snapshotBlocked = true
                    } else {
                        val cleanup = Active(++generation, envelope.service, envelope.context, request)
                        cleanup.taskId = blockedTask
                        cleanup.snapshotBlocked = true
                        releaseSnapshotAfterDetach(cleanup)
                    }
                }
            }
            parent ?: return
            if (!isValid(show.surface) || !isValid(parent)) return
            val actualWidth = surfaceWidth.invoke(show.surface) as Int
            val actualHeight = surfaceHeight.invoke(show.surface) as Int
            if ((actualWidth != width || actualHeight != height) &&
                !(show.leash < 0 && actualWidth <= 0 && actualHeight <= 0)) return
            val layer = surfaceLayer.invoke(show.surface) as Int
            // Layout may run repeatedly before mSurfaceShown flips. One early
            // publication per request/layer suffices; real show/attach
            // transactions must still receive their own atomic protection.
            if (show.transaction == null && earlyPublications[layer] == request.eventNs) return
            val surface = nativeObject.getLong(show.surface)
            val parentPointer = nativeObject.getLong(parent)
            if (surface == 0L || parentPointer == 0L) return
            // A null transaction denotes creation, before the control is handed
            // to its producer or attached below a visible window. Submit only
            // effects through our own transaction, then also join the actual
            // pending/sync publication. Borrowed transactions are never applied.
            val owned = if (show.transaction == null) newTransaction.newInstance() else null
            val publication = show.transaction ?: checkNotNull(owned)
            try {
                val transaction = transactionObject.getLong(publication)
                if (transaction == 0L) return
                val attached = ContentAnimator.attach(transaction, surface, parentPointer, width, height, request.eventNs) ||
                    ContentAnimator.prime(transaction, surface, parentPointer, width, height,
                        request.geometry, false, request.eventNs,
                        starting = show.window == null || (access.field(show.window, "mAttrs") as? WindowManager.LayoutParams)?.type == 3)
                if (attached) {
                    if (owned != null) {
                        applyTransaction.invoke(owned)
                        earlyPublications[layer] = request.eventNs
                        earlyPublications.entries.removeIf { now - it.value > SESSION_NS }
                    }
                    report(Log.INFO, "${if (owned == null) "show" else "created"} primed " +
                        "leash=${request.leash} layer=$layer")
                    surfacesChanged()
                }
            } finally {
                if (owned != null) closeTransaction.invoke(owned)
            }
        }.onFailure { report(Log.WARN, "first-show priming skipped", it) }
    }

    private fun identityDomain(activity: Any, task: Any, frame: Rect?, width: Int, height: Int): Boolean {
        if (width <= 0 || height <= width || frame != Rect(0, 0, width, height) ||
            access.call(task, "getWindowingMode") != 1 || access.call(activity, "getWindowingMode") != 1 ||
            access.call(activity, "isEmbedded") != false || access.call(activity, "hasFixedRotationTransform") != false ||
            access.call(activity, "isLetterboxedForFixedOrientationAndAspectRatio") != false ||
            access.call(activity, "getBounds") != frame || access.call(task, "getBounds") != frame) return false
        val display = access.call(activity, "getDisplayContent") ?: return false
        return access.call(display, "getRotation") == 0 && access.call(display, "getBounds") == frame
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
                    selected.taskId, selected.uid, selected.task,
                    size != null && identityDomain(selected.activity, selected.task, frame, size.first, size.second)).also {
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

    private fun stop(current: Active, reason: String) {
        if (sessions[current.parentLayer] !== current) return
        sessions.remove(current.parentLayer)
        requests.computeIfPresent(current.parentLayer) { _, envelope ->
            envelope.takeIf { it.request.eventNs > current.request.eventNs }
        }
        owners.remove(current.request.eventNs)
        earlyPublications.entries.removeIf { it.value == current.request.eventNs }
        ContentAnimator.stopRequest(current.nativeParent, current.request.eventNs)
        releaseSnapshotAfterDetach(current)
        report(Log.INFO, "stop parent=${current.parentLayer} reason=$reason")
    }

    private fun releaseSnapshotAfterDetach(previous: Active) {
        if (previous.taskId < 0) return
        if ((snapshotReleases[previous.taskId]?.eventNs ?: 0L) > previous.request.eventNs) return
        val token = SnapshotRelease(previous.request.eventNs)
        snapshotReleases[previous.taskId] = token
        fun check() {
            if (closed || snapshotReleases[previous.taskId] !== token) return
            if (sessions.values.any { it.taskId == previous.taskId && it.snapshotBlocked }) {
                snapshotReleases.remove(previous.taskId)
                return
            }
            if (ContentAnimator.running()) handler.postDelayed({ check() }, CHECK_MS)
            else {
                snapshotReleases.remove(previous.taskId)
                snapshots.unblock(previous.taskId, previous.request.eventNs)
                if (!previous.request.closing) snapshots.refresh(previous.service, previous.taskId)
            }
        }
        handler.postDelayed({ check() }, CHECK_MS)
    }

    override fun close() {
        if (closed) return
        closed = true
        requests.clear()
        owners.clear()
        earlyPublications.clear()
        ContentAnimator.stop()
        runCatching { events.close() }
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
        val task: Any, val identityDomain: Boolean,
    ) : AutoCloseable {
        val parentLayer = surfaceLayer.invoke(parent) as Int
        val contentLayer = content?.let { surfaceLayer.invoke(it) as Int } ?: -1
        val companionLayers = companions.map { surfaceLayer.invoke(it) as Int }
        override fun close() { companions.forEach(::release); release(content); release(parent) }
    }

    private class Candidate(val window: Any, val parent: Any, val content: Any?,
        val activity: Any, val task: Any, val activityToken: Any, val taskId: Int, val uid: Int, val type: Int)

    private class Active(val generation: Long, val service: Any, val context: Context,
        val request: Request) {
        val parentLayer = request.leash
        var nativeParent = -1
        var appliedEventNs = 0L
        var task: Any? = null
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

    private class SnapshotRelease(val eventNs: Long)
    private data class Envelope(val service: Any, val context: Context, val request: Request)
    private data class Owner(val task: Any, val uid: Int, val width: Int, val height: Int)

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
                if (parcel.dataAvail() != 0 || operation !in TRANSITION..PREPARE_PREVIEW ||
                    direction !in 0..1 || eventNs <= 0 || age !in -FUTURE_NS..MAX_EVENT_AGE_NS ||
                    geometry.any { !it.isFinite() }) return null
                if (operation != CANCEL && (leash < 0 || taskId < -1 ||
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
        const val PREPARE_PREVIEW = 3
        const val MAX_EXTENT = 16384
        const val CANCEL_GRACE_MS = 48L
        const val CHECK_MS = 50L // Detachment completion cleanup only.
        const val MAX_EVENT_AGE_NS = 1_500_000_000L
        const val FUTURE_NS = 100_000_000L
        const val SESSION_NS = 1_500_000_000L
        val SCALE_KEYS = arrayOf(Settings.Global.ANIMATOR_DURATION_SCALE,
            Settings.Global.WINDOW_ANIMATION_SCALE, Settings.Global.TRANSITION_ANIMATION_SCALE)
    }
}
