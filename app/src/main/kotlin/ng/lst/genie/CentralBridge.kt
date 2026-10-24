package ng.lst.genie

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Rect
import android.os.Binder
import android.os.Handler
import android.os.HandlerThread
import android.os.Parcel
import android.os.Parcelable
import android.provider.Settings
import android.util.Log
import android.view.WindowManager
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedInterface.HookHandle
import io.github.libxposed.api.XposedModule
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean

/** System-server-only content ownership bridge. No hooks are installed in target Apps. */
@SuppressLint("PrivateApi", "BlockedPrivateApi", "SoonBlockedPrivateApi")
internal class CentralBridge(private val module: XposedModule, loader: ClassLoader) : AutoCloseable {
    private val access = OptionalMembers()
    private val snapshots = SnapshotGuard(module, loader)
    private val preStarting = PreStartingSurfaces(module, loader, ::surfacesChanged)
    private val subtrees = LiveSubtrees(module, loader, ::surfacesChanged)
    private val groupPublicationLock = Any()
    private val groupOwners = ConcurrentHashMap<Int, Long>()
    private val groupTasks = ConcurrentHashMap<Int, Int>()
    private val taskEvents = ConcurrentHashMap<Int, Long>()
    private val taskRequests = ConcurrentHashMap<Int, Request>()
    private val taskOpenEvents = ConcurrentHashMap<Int, Long>()
    private val events = SurfaceEvents(module, loader, ::surfaceShowing, ::surfacesChanged, ::bufferCreated,
        ::sourceRemoved, ::taskReturned)
    private val snapshotReleases = HashMap<Int, SnapshotRelease>()
    private val wmsClass = Class.forName("com.android.server.wm.WindowManagerService", false, loader)
    private val scClass = Class.forName("android.view.SurfaceControl")
    private val copySurface = scClass.getConstructor(scClass, String::class.java)
    private val validSurface = scClass.getMethod("isValid")
    private val surfaceLayer = scClass.getMethod("getLayerId")
    private val surfaceName = scClass.getMethod("getName")
    private val surfaceCreator = scClass.getField("CREATOR").get(null) as Parcelable.Creator<*>
    private val releaseSurface = scClass.getMethod("release")
    private val surfaceWidth = scClass.getMethod("getWidth")
    private val surfaceHeight = scClass.getMethod("getHeight")
    private val surfaceLock = scClass.getDeclaredField("mLock").apply { isAccessible = true }
    private val nativeObject = scClass.getDeclaredField("mNativeObject").apply { isAccessible = true }
    private val nativeWriteSurface = runCatching {
        scClass.getDeclaredMethod("nativeWriteToParcel", Long::class.javaPrimitiveType,
            Parcel::class.java).apply { isAccessible = true }
    }.getOrNull()
    private val writeTransaction = scClass.getDeclaredMethod("nativeWriteTransactionToParcel",
        Long::class.javaPrimitiveType, Parcel::class.java).apply { isAccessible = true }
    private val transactionClass = Class.forName("android.view.SurfaceControl\$Transaction")
    private val transactionObject = transactionClass.getDeclaredField("mNativeObject").apply { isAccessible = true }
    private val newTransaction = transactionClass.getConstructor()
    private val applyTransaction = transactionClass.getMethod("apply")
    private val closeTransaction = transactionClass.getMethod("close")
    private val requests = ConcurrentHashMap<Int, Envelope>()
    private val owners = ConcurrentHashMap<Long, Owner>()
    private val earlyPublications = ConcurrentHashMap<Int, Long>()
    private val openingPreviews = ConcurrentHashMap<Int, PreviewCloseIdentity>()
    private val closingPreviews = ConcurrentHashMap<Long, PreviewCloseIdentity>()
    private val registeredOpeningPreviews = ConcurrentHashMap<Int, RegisteredGrant>()
    private val registeredClosingPreviews = ConcurrentHashMap<Long, RegisteredCloseIdentity>()
    private val sessions = LinkedHashMap<Int, Active>() // Bridge worker only.
    private val lastEvents = HashMap<Int, Long>()
    private val changeQueued = AtomicBoolean()
    private val worker = HandlerThread("Genie.Bridge").apply { start() }
    private val handler = Handler(worker.looper)
    private var hook: HookHandle? = null
    @Volatile private var closed = false
    @Volatile private var rendererReady = false
    @Volatile private var contentEnabled = true

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
                reply.writeException(SecurityException("Genie bridge is restricted to the launcher UID"))
                return
            }
            var prepared = false
            val accepted = runCatching {
                input.enforceInterface(DESCRIPTOR)
                val request = Request.read(input) ?: return@runCatching false
                if (closed) return@runCatching false
                if (request.taskId >= 0 && request.operation in arrayOf(TRANSITION, PREPARE_GROUP, PREPARE_PREVIEW)) {
                    recordTaskRequest(request)
                    surfacesChanged()
                }
                if (request.operation == PREPARE_GROUP) {
                    prepared = prepareGroup(service, context, request, reply)
                    return@runCatching prepared
                }
                if (request.operation == PREPARE_ICON) {
                    prepared = prepareIcon(context, request, input, reply)
                    return@runCatching prepared
                }
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

    /** Exact pre-start capability and first shader state; no WM lock or worker wait. */
    private fun preparePreview(service: Any, context: Context, request: Request, reply: Parcel): Boolean {
        val writer = nativeWriteSurface ?: return false
        if (!rendererReady || !animationsEnabled(context) || request.closing ||
            (request.taskId < 0 && request.containerLayer != request.leash)) return false
        contentEnabled = true
        val width = request.geometry[6].toInt()
        val height = request.geometry[7].toInt()
        preStarting.opening(request.leash, width, height)?.use { preview ->
            val encoded = Parcel.obtain()
            val publication = newTransaction.newInstance()
            try {
                synchronized(checkNotNull(surfaceLock.get(preview.parent))) {
                    synchronized(checkNotNull(surfaceLock.get(preview.surface))) {
                        if (!ContentAnimator.prime(transactionObject.getLong(publication),
                                nativeObject.getLong(preview.surface), nativeObject.getLong(preview.parent),
                                width, height, request.geometry, false, request.eventNs, starting = true)) return false
                        writer.invoke(null, nativeObject.getLong(preview.surface), encoded)
                    }
                }
                writeTransaction.invoke(null, transactionObject.getLong(publication), encoded)
                rememberRegisteredPreview(preview, request)
                val transition = Request(TRANSITION, request.leash, request.containerLayer,
                    request.taskId, request.closing, request.eventNs, request.geometry)
                publish(Envelope(service, context, transition))
                handler.post { process(service, context, transition) }
                reply.writeNoException()
                reply.writeInt(1)
                reply.appendFrom(encoded, 0, encoded.dataSize())
                return true
            } finally {
                closeTransaction.invoke(publication)
                encoded.recycle()
            }
        }
        return false
    }

    /** An already prepared group is published before Launcher changes its host. */
    private fun prepareGroup(service: Any, context: Context, request: Request, reply: Parcel): Boolean {
        fun reject(reason: String): Boolean {
            report(Log.DEBUG, "group rejected leash=${request.leash} task=${request.taskId} " +
                "closing=${request.closing} reason=$reason")
            return false
        }
        fun defer(reason: String = "first-buffer"): Boolean {
            report(Log.DEBUG, "group deferred leash=${request.leash} task=${request.taskId} reason=$reason")
            reply.writeNoException()
            reply.writeInt(2) // Retry is allowed; this reply changes no host geometry.
            return true
        }
        if (!rendererReady || nativeWriteSurface == null || !animationsEnabled(context) || request.taskId < 0) {
            return reject("capability-or-task-unavailable")
        }
        if (!taskCurrent(request)) return reject("superseded-task")
        contentEnabled = true
        if (request.closing && registeredClosingPreviews.containsKey(request.eventNs))
            return prepareRegisteredPreviewClose(service, context, request, reply)
        // LRU retirement and domain invalidation can outlive their Task mapping.
        // Resolve the current producer again instead of rejecting a stale source.
        val cachedSource = groupTasks[request.taskId]
        if (cachedSource != null && !subtrees.prepared(cachedSource))
            groupTasks.remove(request.taskId, cachedSource)
        val source = groupTasks[request.taskId] ?: run {
            // A normal window can appear after the last lifecycle callback.
            // Resolve it now so its verified subtree can start producing a
            // first buffer before this host transaction is admitted.
            try {
                lookup(service, request)?.close()
            } catch (_: PreStartingSurfaces.Busy) {
                return if (taskCurrent(request)) defer("companion-registry-busy") else reject("superseded-task")
            }
            groupTasks[request.taskId]
        } ?: return if (request.closing && (prepareRegisteredPreviewClose(service, context, request, reply) ||
                preparePreviewClose(service, context, request, reply))) true
            else reject("task-source-unavailable")
        // This endpoint holds no WMS/SC locks. Give the already-running VD a
        // bounded chance to commit its first real buffer while Launcher's
        // original host transaction is still untouched and owned by its caller.
        if (!subtrees.awaitReady(source, FIRST_BUFFER_WAIT_MS)) {
            if (request.closing && prepareRegisteredPreviewClose(service, context, request, reply)) return true
            return if (taskCurrent(request) && groupTasks[request.taskId] == source && subtrees.prepared(source))
                defer() else reject("source-retired source=$source")
        }
        val prepared = try {
            lookup(service, request)
        } catch (_: PreStartingSurfaces.Busy) {
            return if (taskCurrent(request)) defer("companion-registry-busy") else reject("superseded-task")
        } ?: return reject("task-content-unavailable")
        prepared.use { match ->
            val entry = match.group ?: return reject("source-not-ready source=$source")
            if (entry.sourceLayer != source || entry.task !== match.task ||
                access.field(entry.task, "mTaskId") != request.taskId ||
                (request.containerLayer >= 0 && surfaceLayer.invoke(entry.parent) != request.containerLayer)) {
                return reject("task-parent-mismatch")
            }
            // A close starts at the gesture's translated/scaled presentation,
            // while the owned buffer keeps its original fullscreen dimensions.
            // Require uniform scaling of that verified domain; opens still
            // need an exact full-size endpoint before the host can be changed.
            val full = if (request.closing) 0 else 4
            val width = request.geometry[full + 2]
            val height = request.geometry[full + 3]
            val domainMatches = if (request.closing) {
                width <= entry.width + 2f && height <= entry.height + 2f &&
                    kotlin.math.abs(height - width * entry.height / entry.width) < 2f
            } else {
                kotlin.math.abs(width - entry.width) < 2f &&
                    kotlin.math.abs(height - entry.height) < 2f
            }
            if (!domainMatches) {
                report(Log.DEBUG, "group domain rejected leash=${request.leash} closing=${request.closing} " +
                    "presentation=${width}x$height buffer=${entry.width}x${entry.height}")
                return false
            }
            if (!subtrees.presentationReady(entry)) return defer("previous-host-publication")
            if (request.closing && match.closingArea == null) return reject("closing-display-domain")
            val transition = Request(TRANSITION, request.leash, request.containerLayer, request.taskId,
                request.closing, request.eventNs, request.geometry)
            // Unlike a WMS show hook, this authenticated Binder endpoint holds
            // neither WM nor SC/renderer locks. Serialize with an already-running
            // pristine snapshot instead of reverting a ready group to stock.
            if (!snapshots.block(request.taskId, request.eventNs)) return false
            val publication = newTransaction.newInstance()
            val encoded = Parcel.obtain()
            var host: LiveSubtrees.ClosingHostLease? = null
            var activated = false
            var primed = false
            var exported = false
            try {
                if (!taskCurrent(request)) return reject("superseded-task")
                if (request.closing) {
                    host = subtrees.closingHost(entry, request.eventNs, checkNotNull(match.closingArea))
                        ?: return defer("closing-host-unavailable")
                }
                // The logical Task authenticates the content. Only our composed
                // output moves to the independent host in this same publication.
                val locks = (listOfNotNull(entry.parent, entry.output, entry.source, host?.surface) + match.companions)
                    .map { checkNotNull(surfaceLock.get(it)) }.distinct()
                fun prime(index: Int): Boolean {
                    if (index < locks.size) return synchronized(locks[index]) { prime(index + 1) }
                    return synchronized(groupPublicationLock) {
                        if (!taskCurrent(request) || !subtrees.activate(entry, publication, request.eventNs, host)) {
                            subtrees.discardPublication(publication)
                            return@synchronized false
                        }
                        activated = true
                        primed = ContentAnimator.prime(transactionObject.getLong(publication),
                            nativeObject.getLong(entry.output), nativeObject.getLong(entry.parent), entry.width, entry.height,
                            request.geometry, request.closing, request.eventNs,
                            companions = LongArray(match.companions.size) { nativeObject.getLong(match.companions[it]) },
                            unwarpedSource = nativeObject.getLong(entry.source))
                        if (primed) groupOwners.merge(source, request.eventNs) { old, next -> maxOf(old, next) }
                        primed
                    }
                }
                if (!prime(0)) return reject("native-prime-rejected")
                val writer = nativeWriteSurface ?: return false
                writer.invoke(null, nativeObject.getLong(entry.output), encoded)
                host?.let { writer.invoke(null, nativeObject.getLong(it.surface), encoded) }
                // Visibility, host topology and shader state reach SF only in
                // Launcher's matching geometry transaction.
                writeTransaction.invoke(null, transactionObject.getLong(publication), encoded)
                reply.writeNoException()
                reply.writeInt(1)
                reply.writeInt(if (host != null) 3 else 1)
                reply.writeInt(entry.width)
                reply.writeInt(entry.height)
                reply.appendFrom(encoded, 0, encoded.dataSize())
                exported = true
                earlyPublications[surfaceLayer.invoke(entry.output) as Int] = request.eventNs
                publish(Envelope(service, context, transition))
                handler.post { process(service, context, transition) }
                report(Log.INFO, "group before host leash=${request.leash} source=$source " +
                    "output=${surfaceLayer.invoke(entry.output)} ownedHost=${host?.surface?.let { surfaceLayer.invoke(it) }} " +
                    "companions=${match.companionLayers} closing=${request.closing}")
                return true
            } finally {
                if (activated && !exported) {
                    // A failed native prime or parcel write must never leave an
                    // applied output reparent without its matching Launcher host.
                    runCatching {
                        if (primed) ContentAnimator.revokeRequest(match.parentLayer, request.eventNs)
                        synchronized(groupPublicationLock) {
                            if (subtrees.restore(entry, publication)) applyTransaction.invoke(publication)
                            else subtrees.discardPublication(publication)
                        }
                    }.onFailure {
                        subtrees.restoreFailed(source)
                        report(Log.WARN, "unpublished group rollback failed source=$source", it)
                    }
                }
                host?.close()
                handler.post {
                    val cleanup = Active(service, context, transition)
                    cleanup.taskId = request.taskId
                    cleanup.task = match.task
                    cleanup.snapshotBlocked = true
                    releaseSnapshotAfterDetach(cleanup)
                }
                closeTransaction.invoke(publication)
                encoded.recycle()
            }
        }
    }

    private fun rememberRegisteredPreview(preview: PreStartingSurfaces.Preview, request: Request) {
        if (request.closing || preview.identity.parentLayer != request.leash) return
        registeredOpeningPreviews.compute(request.leash) { _, old ->
            if (old == null || old.openingNs < request.eventNs) RegisteredGrant(preview.identity, request.eventNs)
            else old
        }
        while (registeredOpeningPreviews.size > MAX_PREVIEW_GRANTS)
            registeredOpeningPreviews.entries.minByOrNull { it.value.openingNs }
                ?.let { registeredOpeningPreviews.remove(it.key, it.value) }
    }

    /** Retarget the exact published OEM preview before allowing its host to fall back to stock. */
    private fun prepareRegisteredPreviewClose(service: Any, context: Context, request: Request, reply: Parcel): Boolean {
        val pinned = registeredClosingPreviews[request.eventNs]
        val grant = pinned?.grant ?: registeredOpeningPreviews[request.leash]
        fun reject(reason: String): Boolean {
            report(Log.DEBUG, "registered close rejected leash=${request.leash} task=${request.taskId} " +
                "parent=${request.containerLayer} epoch=${request.eventNs} opening=${grant?.openingNs} " +
                "token=${grant?.identity?.token} producer=${grant?.identity?.producerLayer} " +
                "buffer=${grant?.identity?.bufferLayer} reason=$reason")
            return false
        }
        if (grant == null) return reject("no-published-grant")
        val writer = nativeWriteSurface ?: return reject("surface-writer-unavailable")
        if (!request.closing || request.leash != grant.identity.parentLayer || request.containerLayer < 0 ||
            !grant.allowsClose(request.eventNs, request.geometry[2], request.geometry[3]) || !taskCurrent(request))
            return reject("request-domain-or-epoch")
        val match = lookupRegisteredPreview(service, request, grant, pinned) ?: return false
        match.use {
            val identity = checkNotNull(it.registeredIdentity)
            if (!snapshots.block(request.taskId, request.eventNs)) return reject("snapshot-busy")
            val publication = newTransaction.newInstance()
            val encoded = Parcel.obtain()
            try {
                // Recheck original OEM controls before taking publication locks.
                // WMS show hooks can already hold a producer's own SC monitor.
                preStarting.closing(grant.identity, identity.activity, it.task)?.close()
                    ?: return reject("producer-retired-before-prime")
                val locks = (listOf(it.parent, checkNotNull(it.content)) + it.companions)
                    .map { surface -> checkNotNull(surfaceLock.get(surface)) }.distinct()
                fun prime(index: Int): Boolean {
                    if (index < locks.size) return synchronized(locks[index]) { prime(index + 1) }
                    return synchronized(groupPublicationLock) {
                        if (!taskCurrent(request)) return@synchronized reject("superseded-task")
                        if (!identity.sameLaunch(identity.activity, it.task,
                                preStarting.launchToken(identity.activity), it.uid))
                            return@synchronized reject("launch-changed-before-prime")
                        if (!subtrees.previewReady(it.task)) return@synchronized reject("composed-output-pending")
                        ContentAnimator.prime(transactionObject.getLong(publication),
                            nativeObject.getLong(match.content), nativeObject.getLong(match.parent),
                            match.width, match.height, request.geometry, true, request.eventNs,
                            companions = LongArray(match.companions.size) { index ->
                                nativeObject.getLong(match.companions[index]) }, starting = true)
                    }
                }
                if (!prime(0)) return reject("identity-or-native-prime")
                writer.invoke(null, nativeObject.getLong(it.content), encoded)
                writeTransaction.invoke(null, transactionObject.getLong(publication), encoded)
                registeredClosingPreviews.entries.removeIf { entry -> System.nanoTime() - entry.key > SESSION_NS }
                registeredClosingPreviews[request.eventNs] = identity
                val remaining = ((SESSION_NS - (System.nanoTime() - request.eventNs)) / 1_000_000L).coerceAtLeast(1L)
                handler.postDelayed({ registeredClosingPreviews.remove(request.eventNs, identity) }, remaining)
                earlyPublications[it.contentLayer] = request.eventNs
                val transition = Request(TRANSITION, request.leash, request.containerLayer, request.taskId,
                    true, request.eventNs, request.geometry)
                publish(Envelope(service, context, transition))
                handler.post { process(service, context, transition) }
                reply.writeNoException()
                reply.writeInt(1)
                reply.writeInt(2) // Registered/starting preview.
                reply.writeInt(it.width)
                reply.writeInt(it.height)
                reply.appendFrom(encoded, 0, encoded.dataSize())
                report(Log.INFO, "registered close before host leash=${request.leash} task=${it.taskId} " +
                    "parent=${it.parentLayer} producer=${grant.identity.producerLayer} content=${it.contentLayer} " +
                    "token=${grant.identity.token} opening=${grant.openingNs} epoch=${request.eventNs} " +
                    "companions=${it.companionLayers}")
                return true
            } finally {
                handler.post {
                    val cleanup = Active(service, context, request)
                    cleanup.taskId = request.taskId
                    cleanup.task = it.task
                    cleanup.snapshotBlocked = true
                    releaseSnapshotAfterDetach(cleanup)
                }
                closeTransaction.invoke(publication)
                encoded.recycle()
            }
        }
    }

    /** Only a previously published, still-live starting WindowState can authorize this path. */
    private fun preparePreviewClose(service: Any, context: Context, request: Request, reply: Parcel): Boolean {
        val identity = openingPreviews[request.taskId] ?: return false
        if (!identity.allowsClose(request.taskId, request.eventNs, request.geometry[2], request.geometry[3]) ||
            request.containerLayer < 0 || !taskCurrent(request)) return false
        val match = lookup(service, request, identity) ?: return false
        match.use {
            if (!it.starting || !it.identityDomain || it.content == null || it.contentLayer != identity.bufferLayer ||
                it.group != null || it.previewIdentity !== identity) return false
            if (!snapshots.block(request.taskId, request.eventNs)) return false
            val publication = newTransaction.newInstance()
            val encoded = Parcel.obtain()
            try {
                if (!taskCurrent(request)) return false
                val locks = (listOf(it.parent, it.content) + it.companions)
                    .map { surface -> checkNotNull(surfaceLock.get(surface)) }.distinct()
                fun prime(index: Int): Boolean {
                    if (index < locks.size) return synchronized(locks[index]) { prime(index + 1) }
                    return ContentAnimator.prime(transactionObject.getLong(publication),
                        nativeObject.getLong(it.content), nativeObject.getLong(it.parent), it.width, it.height,
                        request.geometry, true, request.eventNs,
                        companions = LongArray(it.companions.size) { index -> nativeObject.getLong(it.companions[index]) },
                        starting = true)
                }
                if (!prime(0)) return false
                val writer = nativeWriteSurface ?: return false
                writer.invoke(null, nativeObject.getLong(it.content), encoded)
                writeTransaction.invoke(null, transactionObject.getLong(publication), encoded)
                closingPreviews.entries.removeIf { entry ->
                    System.nanoTime() - entry.key > SESSION_NS
                }
                closingPreviews[request.eventNs] = identity
                val remaining = ((SESSION_NS - (System.nanoTime() - request.eventNs)) / 1_000_000L).coerceAtLeast(1L)
                handler.postDelayed({ closingPreviews.remove(request.eventNs, identity) }, remaining)
                earlyPublications[it.contentLayer] = request.eventNs
                val transition = Request(TRANSITION, request.leash, request.containerLayer, request.taskId,
                    true, request.eventNs, request.geometry)
                publish(Envelope(service, context, transition))
                handler.post { process(service, context, transition) }
                reply.writeNoException()
                reply.writeInt(1)
                reply.writeInt(2) // Registered/starting preview.
                reply.writeInt(it.width)
                reply.writeInt(it.height)
                reply.appendFrom(encoded, 0, encoded.dataSize())
                report(Log.INFO, "preview close before host leash=${request.leash} task=${request.taskId} " +
                    "content=${it.contentLayer} launch=${identity.launchToken} companions=${it.companionLayers}")
                return true
            } finally {
                handler.post {
                    val cleanup = Active(service, context, request)
                    cleanup.taskId = request.taskId
                    cleanup.task = it.task
                    cleanup.snapshotBlocked = true
                    releaseSnapshotAfterDetach(cleanup)
                }
                closeTransaction.invoke(publication)
                encoded.recycle()
            }
        }
    }

    /** Capability-only: no WMS lock, worker wait, or publication apply on this Binder thread. */
    private fun prepareIcon(context: Context, request: Request, input: Parcel, reply: Parcel): Boolean {
        if (!rendererReady || !animationsEnabled(context)) return false
        val envelope = requests[request.leash] ?: return false
        val current = envelope.request
        val sameRequest = current.eventNs == request.eventNs && current.closing == request.closing &&
            current.geometry.contentEquals(request.geometry)
        // Adjust is dispatched asynchronously by Launcher. Its icon callback
        // can reach this capability endpoint before the new geometry envelope.
        // Permit only an ordered update of this exact established App stream.
        val advancing = current.eventNs < request.eventNs && current.taskId == request.taskId &&
            current.containerLayer == request.containerLayer
        if (!sameRequest && !advancing) return false
        val textureWidth = input.readInt()
        val textureHeight = input.readInt()
        val windowWidth = input.readInt()
        val windowHeight = input.readInt()
        if (textureWidth !in 1..1024 || textureHeight !in 1..1024 ||
            windowWidth !in 1..MAX_EXTENT || windowHeight !in 1..MAX_EXTENT || windowHeight <= windowWidth) return false
        val full = if (request.closing) 0 else 4
        val width = request.geometry[full + 2]
        val height = request.geometry[full + 3]
        if (request.closing) {
            if (width > windowWidth + 2f || height > windowHeight + 2f ||
                kotlin.math.abs(height - width * windowHeight / windowWidth) >= 2f) return false
        } else if (kotlin.math.abs(width - windowWidth) >= 2f ||
            kotlin.math.abs(height - windowHeight) >= 2f) return false
        owners[request.eventNs]?.let {
            if (it.width != windowWidth || it.height != windowHeight) return false
        }
        var surface: Any? = null
        var parent: Any? = null
        try {
            surface = surfaceCreator.createFromParcel(input) ?: return false
            parent = surfaceCreator.createFromParcel(input) ?: return false
            if (input.dataAvail() != 0 || !isValid(surface) || !isValid(parent)) return false
            val layer = surfaceLayer.invoke(surface) as Int
            val parentLayer = surfaceLayer.invoke(parent) as Int
            // Native getName retains these exact names across a parcel copy.
            // The producer's embedded parent ID must match the actual capability.
            val importedName = surfaceName.invoke(surface)
            val importedParentName = surfaceName.invoke(parent)
            val importedWidth = surfaceWidth.invoke(surface)
            val importedHeight = surfaceHeight.invoke(surface)
            if (layer < 0 || parentLayer < 0 || layer == parentLayer ||
                importedParentName != "FloatingIcon_all#$parentLayer" ||
                importedName != "[BBQ] FloatingIcon_all#$parentLayer#$layer" ||
                importedWidth != textureWidth || importedHeight != textureHeight) {
                report(Log.WARN, "icon capability rejected leash=${request.leash} layer=$layer " +
                    "parent=$parentLayer name=$importedName parentName=$importedParentName " +
                    "size=${importedWidth}x$importedHeight expected=${textureWidth}x$textureHeight")
                return false
            }
            val publication = newTransaction.newInstance()
            val encoded = Parcel.obtain()
            try {
                val primed = synchronized(checkNotNull(surfaceLock.get(parent))) {
                    synchronized(checkNotNull(surfaceLock.get(surface))) {
                        val latest = requests[request.leash]?.request
                        if (latest == null || latest.eventNs > request.eventNs ||
                            (latest.eventNs == request.eventNs && (latest.closing != request.closing ||
                                !latest.geometry.contentEquals(request.geometry)))) false
                        else ContentAnimator.primeIcon(transactionObject.getLong(publication),
                            nativeObject.getLong(surface), nativeObject.getLong(parent), textureWidth, textureHeight,
                            windowWidth, windowHeight, request.geometry, request.closing, request.eventNs,
                            request.eventNs, request.containerLayer)
                    }
                }
                if (!primed) {
                    report(Log.WARN, "icon motion rejected leash=${request.leash} layer=$layer epoch=${request.eventNs}")
                    return false
                }
                if (advancing) {
                    val transition = Request(TRANSITION, request.leash, request.containerLayer,
                        request.taskId, request.closing, request.eventNs, request.geometry)
                    publish(Envelope(envelope.service, context, transition))
                    handler.post { process(envelope.service, context, transition) }
                }
                // This includes the native commit listener. The host's eventual
                // commit, rather than this RPC returning, releases frame updates.
                writeTransaction.invoke(null, transactionObject.getLong(publication), encoded)
                reply.writeNoException()
                reply.writeInt(1)
                reply.appendFrom(encoded, 0, encoded.dataSize())
                report(Log.INFO, "icon prepared leash=${request.leash} layer=$layer parent=$parentLayer " +
                    "texture=${textureWidth}x$textureHeight domain=${windowWidth}x$windowHeight")
                return true
            } finally {
                encoded.recycle()
                closeTransaction.invoke(publication)
            }
        } finally {
            release(parent)
            release(surface)
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

    private fun cancel(request: Request) {
        if (request.leash == -1) {
            requests.clear()
            earlyPublications.clear()
            openingPreviews.clear()
            closingPreviews.clear()
            registeredOpeningPreviews.clear()
            registeredClosingPreviews.clear()
            sessions.values.toList().forEach { stop(it, "disabled") }
            ContentAnimator.stop()
            subtrees.clear()
            handler.post { restoreRetiredGroups() }
            contentEnabled = false
            return
        }
        requests.computeIfPresent(request.leash) { _, entry ->
            entry.takeIf { it.request.eventNs > request.eventNs }
        }
        val current = sessions[request.leash] ?: run {
            handler.postDelayed({ restoreRetiredGroups() }, CHECK_MS)
            return
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
        val locks = ArrayList<Any>(3 + match.companions.size)
        for (surface in listOfNotNull(match.parent, content, match.group?.source) + match.companions) {
            val lock = surfaceLock.get(surface) ?: return false
            if (locks.none { it === lock }) locks += lock
        }
        fun locked(index: Int): Boolean {
            if (index < locks.size) return synchronized(locks[index]) { locked(index + 1) }
            val pointer = nativeObject.getLong(content)
            val parent = nativeObject.getLong(match.parent)
            val companions = LongArray(match.companions.size) { nativeObject.getLong(match.companions[it]) }
            if (pointer == 0L || parent == 0L || companions.any { it == 0L }) return false
            val group = match.group
            if (group == null) return ContentAnimator.transition(pointer, parent, match.width, match.height,
                request.geometry, request.closing, request.eventNs, companions, match.starting)
            val source = nativeObject.getLong(group.source)
            if (source == 0L) return false
            if (earlyPublications[match.contentLayer] == request.eventNs) {
                // The first state already belongs to Launcher's borrowed host t.
                // Queue discovery updates behind its native commit dependency.
                return ContentAnimator.transition(pointer, parent, match.width, match.height,
                    request.geometry, request.closing, request.eventNs, companions, match.starting)
            }
            if (request.closing) return false
            val publication = newTransaction.newInstance()
            try {
                return synchronized(groupPublicationLock) {
                    if (!subtrees.activate(group, publication, request.eventNs, null)) {
                        subtrees.discardPublication(publication)
                        return@synchronized false
                    }
                    val primed = ContentAnimator.prime(transactionObject.getLong(publication), pointer, parent,
                        match.width, match.height, request.geometry, request.closing, request.eventNs,
                        companions, match.starting, source)
                    if (primed) groupOwners.merge(group.sourceLayer, request.eventNs) { old, next -> maxOf(old, next) }
                    else subtrees.restore(group, publication)
                    try { applyTransaction.invoke(publication) }
                    catch (error: Throwable) { subtrees.restoreFailed(group.sourceLayer); throw error }
                    primed
                }
            } finally { closeTransaction.invoke(publication) }
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

    private fun recordTaskRequest(request: Request) {
        if (!request.closing) taskOpenEvents.merge(request.taskId, request.eventNs) { old, next -> maxOf(old, next) }
        taskRequests.compute(request.taskId) { _, old ->
            if (old == null || old.eventNs <= request.eventNs) request else old
        }
        taskEvents.merge(request.taskId, request.eventNs) { old, next -> maxOf(old, next) }
        if (taskEvents.size > 64) {
            val now = System.nanoTime()
            taskEvents.entries.removeIf { now - it.value > SESSION_NS }
            taskRequests.entries.removeIf { now - it.value.eventNs > SESSION_NS }
            taskOpenEvents.keys.removeIf { !taskEvents.containsKey(it) }
        }
    }

    private fun taskCurrent(request: Request): Boolean =
        request.taskId < 0 || request.eventNs >= (taskEvents[request.taskId] ?: 0L)

    private fun process(service: Any, context: Context, request: Request) {
        if (closed || request.eventNs < (lastEvents[request.leash] ?: 0L)) return
        if (request.operation != CANCEL && !taskCurrent(request)) return
        if (request.operation != CANCEL && requests[request.leash]?.request?.eventNs != request.eventNs) return
        lastEvents[request.leash] = request.eventNs
        if (request.operation == CANCEL) {
            cancel(request)
            return
        }
        contentEnabled = true
        val resolved = requests[request.leash]?.request?.takeIf {
            it.eventNs == request.eventNs && it.taskId >= 0
        } ?: request
        val previous = sessions[request.leash]
        if (previous != null && previous.request.eventNs == resolved.eventNs &&
            previous.request.taskId == resolved.taskId && previous.request.containerLayer == resolved.containerLayer) {
            checkCurrent(previous)
            return
        }
        val next = Active(service, context, resolved)
        if (previous != null) {
            next.activityToken = previous.activityToken
            next.taskId = previous.taskId
            next.uid = previous.uid
            next.task = previous.task
            next.nativeParent = previous.nativeParent
            next.contentLayer = previous.contentLayer
            next.companionLayers = previous.companionLayers
            next.width = previous.width
            next.height = previous.height
            next.snapshotBlocked = previous.snapshotBlocked
            if (previous.request.eventNs != resolved.eventNs) owners.remove(previous.request.eventNs)
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
        val taskId = current.request.taskId.takeIf { it >= 0 } ?: current.taskId
        if (current.request.eventNs < (taskEvents[taskId] ?: 0L)) {
            // A reversal of this exact host still needs its current native
            // contour to seed the next prime. End discovery, but revoke only
            // when a different host or content owner takes over.
            val successor = taskRequests[taskId]
            stop(current, if (successor?.leash == current.parentLayer) "superseded-stream" else "superseded-task")
            return
        }
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
                if (registeredClosingPreviews.containsKey(current.request.eventNs)) {
                    stop(current, "owner-changed")
                    return
                }
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
                if (current.request.closing && current.contentLayer >= 0 && it.content != null &&
                    current.contentLayer != it.contentLayer) {
                    // A close may keep its original content, but cannot claim a
                    // replacement window that appeared during a later launch.
                    stop(current, "owner-changed")
                    return
                }
                if (it.needsGroup && it.group == null) {
                    owners[current.request.eventNs] = Owner(it.task, it.uid, it.width, it.height)
                    current.activityToken = it.activityToken
                    current.taskId = it.taskId
                    current.task = it.task
                    current.uid = it.uid
                    // The first real VD buffer commit, not a polling timer,
                    // wakes this request to replace its retained preview.
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
                if (it.content != null) {
                    rememberLayers(current, it)
                    if (!current.request.closing) {
                        if (it.starting && it.previewIdentity != null) {
                            openingPreviews.compute(it.taskId) { _, old ->
                                if (old == null || old.openingNs <= it.previewIdentity.openingNs) it.previewIdentity else old
                            }
                            while (openingPreviews.size > MAX_PREVIEW_GRANTS)
                                openingPreviews.entries.minByOrNull { entry -> entry.value.openingNs }
                                    ?.let { entry -> openingPreviews.remove(entry.key, entry.value) }
                        } else openingPreviews.remove(it.taskId)
                    }
                }
                // A newer verified request for the same actual Task supersedes
                // discovery only; native preserves same-layer state and guards.
                sessions.values.filter { old -> old !== current && old.task === it.task &&
                    old.request.eventNs < current.request.eventNs }.toList().forEach { old ->
                    sessions.remove(old.parentLayer)
                    owners.remove(old.request.eventNs)
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
                rememberRegisteredPreview(preview, request)
                current.contentLayer = surfaceLayer.invoke(preview.surface) as Int
                current.nativeParent = surfaceLayer.invoke(preview.parent) as Int
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
        current.snapshotBlocked = true
        current.width = match.width
        current.height = match.height
    }

    /** Surface.copyFrom has just materialized a local BBQ, before its first buffer. */
    private fun bufferCreated(source: Any) {
        if (closed || !rendererReady || requests.isEmpty()) return
        val now = System.nanoTime()
        requests.values.filter { taskCurrent(it.request) && !it.request.closing && now - it.request.eventNs in 0..SESSION_NS }
            .sortedByDescending { it.request.eventNs }.forEach { entry ->
                val request = entry.request
                val width = request.geometry[6].toInt()
                val height = request.geometry[7].toInt()
                // Exact registered producer + planned identity domain are still
                // required. A matching builder size alone cannot authorize SELF.
                preStarting.opening(request.leash, width, height)?.use { preview ->
                    val resolved = ContentSurfaces.copy(source, "Genie.local.buffer", width, height)
                    try {
                        if (surfaceLayer.invoke(resolved) == surfaceLayer.invoke(preview.surface)) {
                            surfaceShowing(SurfaceEvents.Show(preview.surface, parent = preview.parent,
                                leash = request.leash))
                        }
                    } finally { release(resolved) }
                }
            }
    }

    private fun sourceRemoved(layer: Int) {
        subtrees.invalidate(layer)
        openingPreviews.entries.removeIf { it.value.producerLayer == layer || it.value.bufferLayer == layer }
        registeredOpeningPreviews.entries.removeIf {
            it.value.identity.producerLayer == layer || it.value.identity.bufferLayer == layer
        }
        groupTasks.entries.removeIf { it.value == layer }
        handler.post { restoreRetiredGroups() }
        surfacesChanged()
    }

    /** OEM local reversal can return to the App without another native SF request. */
    private fun taskReturned(returned: SurfaceEvents.TaskReturn) {
        if (closed || !rendererReady) return
        val request = taskRequests[returned.taskId] ?: return
        val opening = taskOpenEvents[returned.taskId] ?: return
        if (!request.closing || returned.enteredNs <= opening || returned.enteredNs >= request.eventNs ||
            returned.leftNs <= returned.enteredNs || returned.eventNs <= returned.leftNs ||
            returned.eventNs - request.eventNs !in 1..SESSION_NS ||
            request.containerLayer != returned.parentLayer) return
        val source = groupTasks[returned.taskId] ?: return
        val owner = owners[request.eventNs] ?: return
        if (owner.task !== returned.task || owner.uid != returned.uid || groupOwners[source] != request.eventNs) return
        // WMS owns the observer's thread. Its transaction and locks never cross
        // into this worker, which retains every existing publication barrier.
        handler.post {
            synchronized(groupPublicationLock) {
                val latest = taskRequests[returned.taskId]
                val latestOwner = owners[request.eventNs]
                if (closed || latest == null || latest.eventNs != request.eventNs || !latest.closing ||
                    latest.leash != request.leash || latest.containerLayer != returned.parentLayer || !taskCurrent(request) ||
                    taskOpenEvents[returned.taskId] != opening || groupTasks[returned.taskId] != source ||
                    groupOwners[source] != request.eventNs || latestOwner == null || latestOwner.task !== returned.task ||
                    latestOwner.uid != returned.uid) return@synchronized
                val current = sessions[request.leash] ?: return@synchronized
                if (!current.request.closing || current.request.eventNs != request.eventNs ||
                    current.appliedEventNs != request.eventNs || current.task !== returned.task ||
                    current.uid != returned.uid || current.activityToken !== returned.activityToken ||
                    current.nativeParent != returned.parentLayer) return@synchronized
                if (!subtrees.closingOutputMatches(source, returned.task, returned.parentLayer,
                        current.contentLayer, request.eventNs)) return@synchronized
                // This is an ownership-end watermark, never a synthetic open.
                // A late retry of the retired close cannot republish its hide.
                taskEvents.merge(returned.taskId, returned.eventNs) { old, next -> maxOf(old, next) }
                lastEvents[request.leash] = maxOf(lastEvents[request.leash] ?: 0L, returned.eventNs)
                taskRequests.remove(returned.taskId, latest)
                stop(current, "local-task-return")
                restoreRetiredGroups()
                report(Log.INFO, "local task return task=${returned.taskId} leash=${request.leash} " +
                    "epoch=${request.eventNs} event=${returned.eventNs}")
            }
        }
    }

    /** Prepare only a WM-verified identity window; arbitrary process surfaces are never accepted. */
    private fun prepareSubtree(show: SurfaceEvents.Show): Boolean {
        if (!contentEnabled) return false
        val window = show.window ?: return false
        val attrs = access.field(window, "mAttrs") as? WindowManager.LayoutParams ?: return false
        val outer = access.field(window, "mSurfaceControl")
        if (attrs.type != 1 || attrs.packageName == LAUNCHER ||
            attrs.flags and WindowManager.LayoutParams.FLAG_SECURE != 0) {
            subtrees.invalidate(surfaceLayer.invoke(show.surface) as Int)
            return false
        }
        val activity = access.field(window, "mActivityRecord") ?: return false
        val task = access.call(activity, "getTask") ?: return false
        val uid = (access.field(activity, "info") as? android.content.pm.ActivityInfo)?.applicationInfo?.uid
            ?: return false
        if (access.field(window, "mOwnerUid") != uid) return false
        val frame = access.call(window, "getFrame") as? Rect ?: return false
        if (!identityDomain(activity, task, frame, frame.width(), frame.height())) {
            subtrees.invalidate(surfaceLayer.invoke(show.surface) as Int)
            return false
        }
        val animator = access.field(window, "mWinAnimator") ?: return false
        val retained = access.field(animator, "mSurfaceControl") ?: return false
        if (!isLayer(show.surface, surfaceLayer.invoke(retained) as Int)) return false
        val hideTarget = outer ?: return false
        val parent = access.field(task, "mSurfaceControl") ?: return false
        if (!isValid(show.surface) || !isValid(hideTarget) || !isValid(parent) ||
            surfaceLayer.invoke(hideTarget) == surfaceLayer.invoke(show.surface)) return false
        val prepared = subtrees.prepare(show.surface, hideTarget, parent, frame.width(), frame.height(), task)
        if (prepared) groupTasks[access.field(task, "mTaskId") as Int] = surfaceLayer.invoke(show.surface) as Int
        return prepared
    }

    /** Runs at content creation/attachment or show, without waiting on bridge/WM work. */
    private fun surfaceShowing(show: SurfaceEvents.Show) {
        if (closed || !rendererReady) return
        val grouped = runCatching { prepareSubtree(show) }.getOrDefault(false)
        runCatching {
            val outer = show.window?.let { access.field(it, "mSurfaceControl") }
            val outerLayer = outer?.let { surfaceLayer.invoke(it) as Int } ?: -1
            if (outerLayer >= 0 && subtrees.activeForHideTarget(outerLayer)) {
                val owned = if (show.transaction == null) newTransaction.newInstance() else null
                try {
                    val publication = show.transaction ?: checkNotNull(owned)
                    val guarded = subtrees.guardHideTarget(publication, outerLayer)
                    if (owned != null) {
                        if (guarded) applyTransaction.invoke(owned)
                        else subtrees.discardPublication(owned)
                    }
                } finally { if (owned != null) closeTransaction.invoke(owned) }
            }
        }
        if (requests.isEmpty()) return
        runCatching {
            val now = System.nanoTime()
            val entries = requests.values.filter { taskCurrent(it.request) && now - it.request.eventNs in 0..SESSION_NS }
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
            if (request.closing) {
                // A late main window from this exact launch may feed its
                // offscreen group, but cannot appear unmasked behind the
                // closing preview or become the old request's shader target.
                val preview = closingPreviews[request.eventNs]
                val registered = registeredClosingPreviews[request.eventNs]
                val window = show.window ?: return
                val attrs = access.field(window, "mAttrs") as? WindowManager.LayoutParams ?: return
                val sameLaunch = if (registered != null) taskId == registered.taskId &&
                    registered.sameLaunch(activity, task, activity?.let(preStarting::launchToken), uid)
                else preview != null && taskId == preview.taskId &&
                    preview.sameLaunch(activity, task, activity?.let(preStarting::launchToken), uid)
                if (!grouped || attrs.type != 1 || !sameLaunch ||
                    !taskCurrent(request)) return
                val owned = if (show.transaction == null) newTransaction.newInstance() else null
                try {
                    val transaction = show.transaction ?: checkNotNull(owned)
                    val source = surfaceLayer.invoke(show.surface) as Int
                    synchronized(groupPublicationLock) {
                        if (subtrees.guard(transaction, source)) {
                            groupOwners.merge(source, request.eventNs) { old, next -> maxOf(old, next) }
                            if (owned != null) applyTransaction.invoke(owned)
                        } else if (owned != null) subtrees.discardPublication(owned)
                    }
                } finally { if (owned != null) closeTransaction.invoke(owned) }
                return
            }
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
                    // The manager hook proves exact registry membership and
                    // identity geometry; the Task anchor above independently
                    // binds ownership even before the bridge worker runs.
                    if (owner == null && request.taskId >= 0 && !show.verifiedManager) return
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
                    if (published) {
                        recordTaskRequest(bound)
                        handler.post { process(next.service, next.context, bound) }
                    }
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
                        val cleanup = Active(envelope.service, envelope.context, request)
                        cleanup.taskId = blockedTask
                        cleanup.task = task
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
            if (grouped) {
                // Keep the source's own show/buffer state intact so its mirror
                // can render. Only the outer physical container is held back;
                // the output's first-buffer commit wakes the normal lookup.
                val owned = if (show.transaction == null) newTransaction.newInstance() else null
                try {
                    val publication = show.transaction ?: checkNotNull(owned)
                    synchronized(groupPublicationLock) {
                        if (subtrees.guard(publication, surfaceLayer.invoke(show.surface) as Int)) {
                            groupOwners.merge(surfaceLayer.invoke(show.surface) as Int, request.eventNs) { old, next -> maxOf(old, next) }
                            if (owned != null) applyTransaction.invoke(owned)
                        } else if (owned != null) subtrees.discardPublication(owned)
                    }
                } finally { if (owned != null) closeTransaction.invoke(owned) }
                surfacesChanged()
                return
            }
            val bufferSurface = ContentSurfaces.copy(show.surface, "Genie.publication.buffer", width, height)
            try {
            val layer = surfaceLayer.invoke(bufferSurface) as Int
            // Layout may run repeatedly before mSurfaceShown flips. One early
            // publication per request/layer suffices; real show/attach
            // transactions must still receive their own atomic protection.
            if (show.transaction == null && earlyPublications[layer] == request.eventNs) return
            val surface = nativeObject.getLong(bufferSurface)
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
                val starting = show.window == null ||
                    (access.field(show.window, "mAttrs") as? WindowManager.LayoutParams)?.type == 3
                val attached = ContentAnimator.attach(transaction, surface, parentPointer, width, height,
                    request.eventNs, starting = starting) ||
                    ContentAnimator.prime(transaction, surface, parentPointer, width, height,
                        request.geometry, false, request.eventNs, starting = starting)
                if (attached) {
                    if (owned != null) {
                        applyTransaction.invoke(owned)
                        earlyPublications[layer] = request.eventNs
                        earlyPublications.entries.removeIf { now - it.value > SESSION_NS }
                    }
                    if (show.leash >= 0) preStarting.opening(request.leash, width, height)?.use { preview ->
                        if (surfaceLayer.invoke(preview.surface) == layer) rememberRegisteredPreview(preview, request)
                    }
                    report(Log.INFO, "${if (owned == null) "show" else "created"} primed " +
                        "leash=${request.leash} layer=$layer")
                    surfacesChanged()
                }
            } finally {
                if (owned != null) closeTransaction.invoke(owned)
            }
            } finally { release(bufferSurface) }
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

    /** WM lock held: prove the physical parent uses the same unrotated fullscreen coordinates. */
    private fun closingArea(task: Any, parent: Any, width: Int, height: Int): Any? {
        val taskSurface = access.field(task, "mSurfaceControl") ?: return null
        if (!isValid(taskSurface) || !isLayer(parent, surfaceLayer.invoke(taskSurface) as Int)) return null
        val display = access.call(task, "getDisplayContent") ?: return null
        val area = access.call(task, "getTaskDisplayArea") ?: return null
        val bounds = Rect(0, 0, width, height)
        if (access.call(area, "getDisplayContent") !== display ||
            access.call(task, "getWindowingMode") != 1 || access.call(task, "getBounds") != bounds ||
            access.call(area, "getBounds") != bounds || access.call(display, "getBounds") != bounds ||
            access.call(display, "getRotation") != 0) return null
        val surface = access.field(area, "mSurfaceControl") ?: return null
        if (!isValid(surface) || isLayer(surface, surfaceLayer.invoke(taskSurface) as Int)) return null
        return copySurface.newInstance(surface, "Genie.central.closing-area")
    }

    /** Revalidate the published OEM producer, while binding only the current fullscreen Activity. */
    private fun lookupRegisteredPreview(service: Any, request: Request, grant: RegisteredGrant,
                                        pinned: RegisteredCloseIdentity?): Snapshot? {
        val registered = grant.identity
        fun reject(reason: String): Snapshot? {
            report(Log.DEBUG, "registered close lookup rejected leash=${request.leash} task=${request.taskId} " +
                "parent=${request.containerLayer} token=${registered.token} producer=${registered.producerLayer} " +
                "buffer=${registered.bufferLayer} reason=$reason")
            return null
        }
        if (request.leash != registered.parentLayer || request.containerLayer < 0 ||
            !grant.allowsClose(request.eventNs, request.geometry[2], request.geometry[3]))
            return reject("request-domain-or-epoch")
        preStarting.prepare()
        val lock = access.field(service, "mGlobalLock") ?: return reject("missing-wm-lock")
        return synchronized(lock) {
            val windows = access.field(service, "mWindowMap") as? Map<*, *> ?: return@synchronized reject("missing-windows")
            val candidates = windows.values.mapNotNull { window ->
                window ?: return@mapNotNull null
                if (access.field(window, "mRemoved") == true || access.field(window, "mViewVisibility") != 0)
                    return@mapNotNull null
                val attrs = access.field(window, "mAttrs") as? WindowManager.LayoutParams ?: return@mapNotNull null
                if (attrs.type !in 1..3 || attrs.packageName == LAUNCHER ||
                    attrs.flags and WindowManager.LayoutParams.FLAG_SECURE != 0) return@mapNotNull null
                val activity = access.field(window, "mActivityRecord") ?: return@mapNotNull null
                val task = access.call(window, "getTask") ?: return@mapNotNull null
                if (access.call(activity, "getTask") !== task || access.field(task, "mTaskId") != request.taskId)
                    return@mapNotNull null
                val parent = access.field(task, "mSurfaceControl")?.takeIf {
                    isLayer(it, request.containerLayer) } ?: return@mapNotNull null
                val info = access.field(activity, "info") as? android.content.pm.ActivityInfo ?: return@mapNotNull null
                val uid = info.applicationInfo?.uid ?: return@mapNotNull null
                if (uid / 100000 != registered.userId ||
                    (attrs.type != 3 && access.field(window, "mOwnerUid") != uid)) return@mapNotNull null
                val token = access.field(activity, "token") ?: return@mapNotNull null
                val content = if (access.field(window, "mHasSurface") == true)
                    access.field(window, "mWinAnimator")?.let { access.field(it, "mSurfaceControl") }
                        ?.takeIf(::isValid) else null
                Candidate(window, parent, content, activity, task, token, request.taskId, uid, attrs.type)
            }
            val selected = candidates.firstOrNull { it.type == 1 } ?: candidates.firstOrNull()
            val activity = pinned?.activity ?: selected?.activity ?: return@synchronized reject("task-parent-or-window")
            val task = pinned?.task ?: checkNotNull(selected).task
            val activityToken = access.field(activity, "token") ?: return@synchronized reject("activity-token")
            val uid = (access.field(activity, "info") as? android.content.pm.ActivityInfo)?.applicationInfo?.uid
                ?: return@synchronized reject("activity-uid")
            if (access.call(activity, "getTask") !== task || access.field(task, "mTaskId") != request.taskId ||
                !isLayer(access.field(task, "mSurfaceControl"), request.containerLayer) || uid / 100000 != registered.userId)
                return@synchronized reject("task-parent-or-user")
            if (candidates.any { it.activity !== activity || it.activityToken !== activityToken ||
                    it.task !== task || it.uid != uid }) return@synchronized reject("ambiguous-activity")
            val frame = Rect(0, 0, registered.width, registered.height)
            // An already pinned OEM producer can outlive its starting WindowState.
            // Empty discovery cannot replace that capability with a later main window.
            if ((selected != null && access.call(selected.window, "getFrame") != frame) ||
                !identityDomain(activity, task, frame, registered.width, registered.height))
                return@synchronized reject("window-domain")
            if (preStarting.launchToken(activity) != registered.token)
                return@synchronized reject("launch-token current=${preStarting.launchToken(activity)}")
            if (pinned != null && (pinned.grant !== grant || pinned.activityToken !== activityToken ||
                    pinned.taskId != request.taskId || pinned.uid != uid))
                return@synchronized reject("pinned-owner-changed")
            if (!subtrees.previewReady(task)) return@synchronized reject("composed-output-pending")
            val starting = candidates.filter { it.type == 3 && it.content != null &&
                access.call(it.window, "getFrame") == frame &&
                contentSize(it) == (registered.width to registered.height) }
            val startingWindow = access.field(activity, "mStartingWindow")
            val companion = if (startingWindow != null) starting.singleOrNull { it.window === startingWindow }
                else starting.singleOrNull()
            if (companion == null && starting.isNotEmpty()) return@synchronized reject("ambiguous-starting-window")
            val preview = preStarting.closing(registered, activity, task)
                ?: return@synchronized reject("registered-producer-or-domain")
            var companionCopy: Any? = null
            var oemCopies: List<Any> = emptyList()
            var transferred = false
            try {
                companionCopy = companion?.content?.let {
                    ContentSurfaces.copy(it, "Genie.central.registered.starting", registered.width, registered.height)
                }
                val excluded = mutableSetOf(registered.producerLayer, registered.bufferLayer)
                companionCopy?.let { excluded += surfaceLayer.invoke(it) as Int }
                oemCopies = preStarting.copy(activity, task, frame,
                    registered.width, registered.height, request.leash, excluded)
                val companions = (listOfNotNull(companionCopy) + oemCopies).sortedBy { surfaceLayer.invoke(it) as Int }
                val identity = pinned ?: RegisteredCloseIdentity(grant, activity, activityToken, task, request.taskId, uid)
                Snapshot(preview.parent, preview.surface, companions, true, registered.width, registered.height,
                    activityToken, request.taskId, uid, task, true,
                    null, false, null, identity).also { transferred = true }
            } finally {
                if (!transferred) {
                    preview.close()
                    release(companionCopy)
                    oemCopies.forEach(::release)
                }
            }
        }
    }

    /** Copies only while WM's ownership is stable, never returns borrowed surfaces. */
    private fun lookup(service: Any, request: Request,
                       preview: PreviewCloseIdentity? = closingPreviews[request.eventNs]): Snapshot? {
        registeredClosingPreviews[request.eventNs]?.let {
            return lookupRegisteredPreview(service, request, it.grant, it)
        }
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
                if (preview != null && (request.containerLayer < 0 ||
                        !isLayer(access.field(task, "mSurfaceControl"), request.containerLayer))) return@mapNotNull null
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
            val startingContents = candidates.filter { !request.closing && it.type == 3 && it.content != null }
            val preferred = if (request.closing && preview != null) candidates.filter { candidate ->
                val producer = candidate.content
                val size = contentSize(candidate)
                candidate.type == 3 && producer != null && size != null && preview.matches(
                    candidate.window, candidate.activity, candidate.activityToken, candidate.task, producer,
                    surfaceLayer.invoke(producer) as Int, preStarting.launchToken(candidate.activity),
                    candidate.taskId, candidate.uid, size.first, size.second)
            } else normalContents.filter { it.type == 1 }.ifEmpty { normalContents }
                .ifEmpty { startingContents }
                .ifEmpty { normals.filter { it.type == 1 } }.ifEmpty { normals }
                .ifEmpty { candidates.filter { !request.closing && it.type == 3 } }
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
            if (selected.type == 1 && size != null &&
                !identityDomain(selected.activity, selected.task, frame, size.first, size.second)) {
                subtrees.invalidate(primaryLayer)
            }
            if (contentEnabled && selected.type == 1 && selected.content != null && size != null &&
                identityDomain(selected.activity, selected.task, frame, size.first, size.second)) {
                val outer = access.field(selected.window, "mSurfaceControl")
                if (isValid(outer) && subtrees.prepare(selected.content, checkNotNull(outer), selected.parent,
                        size.first, size.second, selected.task)) {
                    groupTasks[selected.taskId] = primaryLayer
                }
            }
            val companion = if (selected.type != 3 && size != null) startingContents.filter {
                it.activityToken === selected.activityToken && it.taskId == selected.taskId &&
                    it.uid == selected.uid && !isLayer(it.content, primaryLayer) &&
                    contentSize(it) == size && framesAligned(frame, access.call(it.window, "getFrame") as? Rect)
            }.singleOrNull() else null
            var contentCopy: Any? = null
            var group = if (selected.type == 1 && size != null) subtrees.ready(primaryLayer)?.let { lease ->
                if (lease.task === selected.task && lease.width == size.first && lease.height == size.second) lease
                else { lease.close(); null }
            } else null
            var companionCopy: Any? = null
            var oemCopies: List<Any> = emptyList()
            var parentCopy: Any? = null
            var closingArea: Any? = null
            try {
                if (request.closing && request.operation == PREPARE_GROUP && group != null)
                    closingArea = closingArea(selected.task, selected.parent, group.width, group.height)
                parentCopy = copySurface.newInstance(selected.parent, "Genie.central.parent")
                contentCopy = if (group != null) copySurface.newInstance(group.output, "Genie.central.group")
                else selected.content?.let {
                    if (size != null) ContentSurfaces.copy(it, "Genie.central.content", size.first, size.second)
                    else copySurface.newInstance(it, "Genie.central.content")
                }
                companionCopy = companion?.content?.let {
                    ContentSurfaces.copy(it, "Genie.central.starting", checkNotNull(size).first, size.second)
                }
                val excluded = mutableSetOf(primaryLayer)
                contentCopy?.let { excluded += surfaceLayer.invoke(it) as Int }
                companionCopy?.let { excluded += surfaceLayer.invoke(it) as Int }
                if (size != null) {
                    oemCopies = preStarting.copy(selected.activity, selected.task, frame,
                        size.first, size.second, request.leash, excluded)
                }
                val companions = (listOfNotNull(companionCopy) + oemCopies)
                    .sortedBy { surfaceLayer.invoke(it) as Int }
                val identity = if (selected.type == 3 && selected.content != null && size != null &&
                    identityDomain(selected.activity, selected.task, frame, size.first, size.second)) {
                    if (preview != null) preview.takeIf { contentCopy != null &&
                        surfaceLayer.invoke(contentCopy) == it.bufferLayer }
                    else preStarting.launchToken(selected.activity)?.let { launch ->
                        PreviewCloseIdentity(selected.window, selected.activity, selected.activityToken, selected.task,
                            selected.content, primaryLayer, contentCopy?.let { surfaceLayer.invoke(it) as Int } ?: -1,
                            launch, request.eventNs, selected.taskId, selected.uid, size.first, size.second)
                    }
                } else null
                if (preview != null && identity == null) return@synchronized null
                Snapshot(parentCopy, contentCopy, companions, selected.type == 3,
                    size?.first ?: 0, size?.second ?: 0, selected.activityToken,
                    selected.taskId, selected.uid, selected.task,
                    size != null && identityDomain(selected.activity, selected.task, frame, size.first, size.second),
                    group, selected.type == 1 && subtrees.prepared(primaryLayer), identity, closingArea = closingArea).also {
                    parentCopy = null; contentCopy = null; companionCopy = null; group = null; closingArea = null
                    oemCopies = emptyList()
                }
            } finally {
                oemCopies.forEach(::release)
                release(companionCopy)
                release(contentCopy)
                release(parentCopy)
                release(closingArea)
                group?.close()
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
        closingPreviews.remove(current.request.eventNs)
        registeredClosingPreviews.remove(current.request.eventNs)
        earlyPublications.entries.removeIf { it.value == current.request.eventNs }
        for (parent in setOf(current.nativeParent, current.request.leash).filter { it >= 0 }) {
            if (reason == "superseded-task" || reason == "owner-changed" || reason == "local-task-return")
                ContentAnimator.revokeRequest(parent, current.request.eventNs)
            else ContentAnimator.stopRequest(parent, current.request.eventNs)
        }
        releaseSnapshotAfterDetach(current)
        report(Log.INFO, "stop parent=${current.parentLayer} reason=$reason")
    }

    private fun releaseSnapshotAfterDetach(previous: Active) {
        handler.postDelayed({ restoreRetiredGroups() }, CHECK_MS)
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
            if (ContentAnimator.requestRunning(previous.request.eventNs) ||
                groupOwners.values.any { it == previous.request.eventNs } ||
                previous.task?.let { !subtrees.snapshotReady(it) } == true) handler.postDelayed({ check() }, CHECK_MS)
            else {
                snapshotReleases.remove(previous.taskId)
                snapshots.unblock(previous.taskId, previous.request.eventNs)
                if (!previous.request.closing) snapshots.refresh(previous.service, previous.taskId)
            }
        }
        handler.postDelayed({ check() }, CHECK_MS)
    }

    /** Cleanup only: no content lookup, and a newer request always keeps its group. */
    private fun restoreRetiredGroups() {
        if (closed || groupOwners.isEmpty()) return
        val retired = groupOwners.entries.map { it.key to it.value }.filter { (_, event) ->
            sessions.values.none { it.request.eventNs == event } &&
                requests.values.none { it.request.eventNs == event }
        }
        if (retired.isEmpty()) return
        val transaction = newTransaction.newInstance()
        val appended = ArrayList<Int>()
        try {
            synchronized(groupPublicationLock) {
                retired.forEach { (source, event) ->
                    if (groupOwners[source] == event && !ContentAnimator.requestRunning(event)) {
                        if (subtrees.restored(source)) groupOwners.remove(source, event)
                        else if (!subtrees.restoring(source) && subtrees.restore(source, transaction)) appended += source
                    }
                }
                if (appended.isNotEmpty()) applyTransaction.invoke(transaction)
                else subtrees.discardPublication(transaction)
            }
        } catch (error: Throwable) {
            appended.forEach(subtrees::restoreFailed)
            report(Log.WARN, "subtree restore failed; retaining snapshot protection", error)
        } finally {
            closeTransaction.invoke(transaction)
            if (retired.any { (source, event) -> groupOwners[source] == event })
                handler.postDelayed({ restoreRetiredGroups() }, CHECK_MS)
        }
    }

    override fun close() {
        if (closed) return
        closed = true
        requests.clear()
        taskOpenEvents.clear()
        owners.clear()
        earlyPublications.clear()
        openingPreviews.clear()
        closingPreviews.clear()
        registeredOpeningPreviews.clear()
        registeredClosingPreviews.clear()
        ContentAnimator.stop()
        runCatching { events.close() }
        groupOwners.clear()
        groupTasks.clear()
        taskEvents.clear()
        taskRequests.clear()
        subtrees.close()
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
        val group: LiveSubtrees.Entry?, val needsGroup: Boolean,
        val previewIdentity: PreviewCloseIdentity?,
        val registeredIdentity: RegisteredCloseIdentity? = null,
        val closingArea: Any? = null,
    ) : AutoCloseable {
        val parentLayer = surfaceLayer.invoke(parent) as Int
        val contentLayer = content?.let { surfaceLayer.invoke(it) as Int } ?: -1
        val companionLayers = companions.map { surfaceLayer.invoke(it) as Int }
        override fun close() {
            companions.forEach(::release); release(content); release(parent); release(closingArea); group?.close()
        }
    }

    private class Candidate(val window: Any, val parent: Any, val content: Any?,
        val activity: Any, val task: Any, val activityToken: Any, val taskId: Int, val uid: Int, val type: Int)

    private class Active(val service: Any, val context: Context,
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
        var snapshotBlocked = false
        var width = 0
        var height = 0
    }

    private class SnapshotRelease(val eventNs: Long)
    private data class RegisteredGrant(val identity: PreStartingSurfaces.RegisteredIdentity, val openingNs: Long) {
        fun allowsClose(eventNs: Long, width: Float, height: Float): Boolean = eventNs > openingNs &&
            width.isFinite() && height.isFinite() && width > 0f && height > 0f &&
            width <= identity.width + 2f && height <= identity.height + 2f &&
            kotlin.math.abs(height - width * identity.height / identity.width) < 2f
    }
    private class RegisteredCloseIdentity(val grant: RegisteredGrant, val activity: Any, val activityToken: Any,
                                         val task: Any, val taskId: Int, val uid: Int) {
        fun sameLaunch(activity: Any?, task: Any?, token: Long?, uid: Int?): Boolean =
            this.activity === activity && this.task === task && grant.identity.token == token && this.uid == uid
    }
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
                if ((operation != PREPARE_ICON && parcel.dataAvail() != 0) || operation !in TRANSITION..PREPARE_ICON ||
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

    private companion object {
        const val TAG = "Genie.Bridge"
        const val LAUNCHER = "com.miui.home"
        const val DESCRIPTOR = "android.view.IWindowManager"
        const val TRANSACTION = 0x484841
        const val MAGIC = 0x48484131
        const val VERSION = 5
        const val TRANSITION = 1
        const val CANCEL = 2
        const val PREPARE_PREVIEW = 3
        const val PREPARE_GROUP = 4
        const val PREPARE_ICON = 5
        const val MAX_EXTENT = 16384
        const val MAX_PREVIEW_GRANTS = 16
        const val FIRST_BUFFER_WAIT_MS = 64L
        const val CANCEL_GRACE_MS = 48L
        const val CHECK_MS = 50L // Detachment completion cleanup only.
        const val MAX_EVENT_AGE_NS = 1_500_000_000L
        const val FUTURE_NS = 100_000_000L
        const val SESSION_NS = 1_500_000_000L
        val SCALE_KEYS = arrayOf(Settings.Global.ANIMATOR_DURATION_SCALE,
            Settings.Global.WINDOW_ANIMATION_SCALE, Settings.Global.TRANSITION_ANIMATION_SCALE)
    }
}
