package ng.lst.genie

import android.annotation.SuppressLint
import android.graphics.PixelFormat
import android.graphics.Rect
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.util.Log
import android.view.Surface
import io.github.libxposed.api.XposedModule
import java.lang.reflect.Proxy
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.function.Consumer

/** System-server live subtree composition. Callers prove ownership and the identity domain. */
@SuppressLint("PrivateApi", "BlockedPrivateApi", "SoonBlockedPrivateApi")
internal class LiveSubtrees(
    private val module: XposedModule,
    loader: ClassLoader,
    private val onReady: () -> Unit = {},
) : AutoCloseable {
    private val sc = Class.forName("android.view.SurfaceControl")
    private val transactionType = Class.forName("android.view.SurfaceControl\$Transaction")
    private val builderType = Class.forName("android.view.SurfaceControl\$Builder")
    private val blastType = Class.forName("android.graphics.BLASTBufferQueue")
    private val displayType = Class.forName("com.android.server.display.DisplayControl", false, loader)
    private val listenerType = Class.forName("android.view.SurfaceControl\$TransactionCommittedListener")
    private val copySurface = sc.getConstructor(sc, String::class.java)
    private val surfaceLock = sc.getDeclaredField("mLock").apply { isAccessible = true }
    private val nativeObject = sc.getDeclaredField("mNativeObject").apply { isAccessible = true }
    private val transactionObject = transactionType.getDeclaredField("mNativeObject").apply { isAccessible = true }
    private val validSurface = sc.getMethod("isValid")
    private val surfaceLayer = sc.getMethod("getLayerId")
    private val releaseSurface = sc.getMethod("release")
    private val mirrorSurface = sc.getMethod("mirrorSurface", sc)
    private val build = builderType.getMethod("build")
    private val nameBuilder = builderType.getMethod("setName", String::class.java)
    private val parentBuilder = builderType.getMethod("setParent", sc)
    private val bufferSizeBuilder = builderType.getMethod("setBufferSize", Int::class.javaPrimitiveType,
        Int::class.javaPrimitiveType)
    private val containerBuilder = builderType.getMethod("setContainerLayer")
    private val blastBuilder = builderType.getMethod("setBLASTLayer")
    private val transactionConstructor = transactionType.getConstructor()
    private val applyTransaction = transactionType.getMethod("apply")
    private val closeTransaction = transactionType.getMethod("close")
    private val show = transactionType.getMethod("show", sc)
    private val hide = transactionType.getMethod("hide", sc)
    private val remove = transactionType.getMethod("remove", sc)
    private val reparent = transactionType.getMethod("reparent", sc, sc)
    private val setLayerStack = transactionType.getMethod("setLayerStack", sc, Int::class.javaPrimitiveType)
    private val setLayer = transactionType.getMethod("setLayer", sc, Int::class.javaPrimitiveType)
    private val setRelativeLayer = transactionType.getMethod("setRelativeLayer", sc, sc,
        Int::class.javaPrimitiveType)
    private val setTrustedOverlay = transactionType.getMethod("setTrustedOverlay", sc,
        Boolean::class.javaPrimitiveType)
    private val setDisplaySurface = transactionType.getMethod("setDisplaySurface", IBinder::class.java,
        Surface::class.java)
    private val setDisplayLayerStack = transactionType.getMethod("setDisplayLayerStack", IBinder::class.java,
        Int::class.javaPrimitiveType)
    private val setDisplayProjection = transactionType.getMethod("setDisplayProjection", IBinder::class.java,
        Int::class.javaPrimitiveType, Rect::class.java, Rect::class.java)
    private val setDisplaySize = transactionType.getMethod("setDisplaySize", IBinder::class.java,
        Int::class.javaPrimitiveType, Int::class.javaPrimitiveType)
    private val addCommitListener = transactionType.getMethod("addTransactionCommittedListener",
        Executor::class.java, listenerType)
    private val createDisplay = displayType.getDeclaredMethod("createVirtualDisplay", String::class.java,
        Boolean::class.javaPrimitiveType).apply { isAccessible = true }
    private val destroyDisplay = displayType.getDeclaredMethod("destroyVirtualDisplay",
        IBinder::class.java).apply { isAccessible = true }
    private val updateBlast = blastType.getMethod("update", sc, Int::class.javaPrimitiveType,
        Int::class.javaPrimitiveType, Int::class.javaPrimitiveType)
    private val syncBlast = blastType.getMethod("syncNextTransaction", Consumer::class.java)
    private val createBlastSurface = blastType.getMethod("createSurface")
    private val destroyBlast = blastType.getMethod("destroy")
    private val lastFrame = blastType.getMethod("getLastAcquiredFrameNum")
    private val lock = Any()
    private val records = LinkedHashMap<Int, Record>()
    private val retiring = LinkedHashSet<Record>()
    private val worker = HandlerThread("Genie.Subtrees").apply { start() }
    private val handler = Handler(worker.looper)
    private val direct = Executor { it.run() }
    private var nextGeneration = 0L
    private var closed = false

    /** A short-lived lease consisting of independent Java/native SC copies. Always use/close. */
    inner class Entry internal constructor(internal val record: Record) : AutoCloseable {
        val sourceLayer = record.sourceLayer
        val width = record.width
        val height = record.height
        val task = record.task
        private val held = ArrayList<Any>(3)
        private val released = AtomicBoolean()
        val source: Any
        val output: Any
        val parent: Any

        init {
            try {
                source = retain(record.source, "Genie.subtree.lease.source").also(held::add)
                output = retain(checkNotNull(record.output), "Genie.subtree.lease.output").also(held::add)
                parent = retain(record.parent, "Genie.subtree.lease.parent").also(held::add)
            } catch (error: Throwable) {
                held.forEach(::release)
                throw error
            }
        }

        internal fun valid(): Boolean = !released.get()
        override fun close() {
            if (released.compareAndSet(false, true)) held.forEach(::release)
        }
    }

    /** A presentation capability is separate from the Task used to authenticate content. */
    inner class ClosingHostLease internal constructor(internal val record: Record,
                                                       internal val host: ClosingHost) : AutoCloseable {
        val surface = retain(host.surface, "Genie.closing-host.lease")
        private val released = AtomicBoolean()
        internal fun valid(): Boolean = !released.get()
        override fun close() {
            if (!released.compareAndSet(false, true)) return
            release(surface)
            synchronized(lock) {
                if (!host.assigned && !host.removing) removeUnusedHost(record, host)
            }
        }
    }

    /** A previous borrowed reparent must reach SF before another epoch can move this output. */
    fun presentationReady(entry: Entry): Boolean = synchronized(lock) {
        entry.valid() && !closed && !entry.record.retired && records[entry.sourceLayer] === entry.record &&
            records.values.none { it.task === entry.task && !topologyReady(it) } &&
            retiring.none { it.task === entry.task }
    }

    /** Ownership remains identifiable while its borrowed publication is awaiting commit. */
    fun closingOutputMatches(sourceLayer: Int, task: Any, parentLayer: Int,
                             outputLayer: Int, eventNs: Long): Boolean = synchronized(lock) {
        val record = records[sourceLayer] ?: return@synchronized false
        val host = record.presentation ?: return@synchronized false
        !closed && !record.retired && record.active && record.bufferReady && record.task === task &&
            record.parentLayer == parentLayer && layer(record.output) == outputLayer &&
            host.eventNs == eventNs && host.assigned && !host.removing &&
            valid(record.source) && valid(record.parent) && valid(record.hideTarget) && valid(record.output)
    }

    /** Runs only on the authenticated prepare endpoint, outside WMS and SurfaceControl locks. */
    fun closingHost(entry: Entry, eventNs: Long, area: Any): ClosingHostLease? = synchronized(lock) {
        val record = entry.record
        val anchor = entry.parent
        if (!presentationReady(entry) || eventNs <= 0L || !valid(area) || !valid(anchor)) return@synchronized null
        val areaLayer = layer(area)
        val anchorLayer = layer(anchor)
        if (areaLayer < 0 || anchorLayer != record.parentLayer ||
            listOf(record.sourceLayer, record.hideLayer, record.parentLayer, layer(record.output))
                .any { it == areaLayer }) return@synchronized null
        record.presentation?.let { current ->
            if (current.eventNs > eventNs) return@synchronized null
            if (current.eventNs == eventNs) return@synchronized if (!current.removing &&
                current.areaLayer == areaLayer && current.anchorLayer == anchorLayer) ClosingHostLease(record, current) else null
        }
        val anchorCopy = retain(anchor, "Genie.closing-host.task")
        val surface = try { control("Genie.Closing.${record.sourceLayer}.$eventNs", area) }
        catch (error: Throwable) { release(anchorCopy); throw error }
        val host = ClosingHost(surface, anchorCopy, eventNs, areaLayer, anchorLayer)
        record.hosts.add(host)
        try { ClosingHostLease(record, host) }
        catch (error: Throwable) { removeUnusedHost(record, host); throw error }
    }

    private fun topologyReady(record: Record, reserved: ClosingHost? = null): Boolean =
        record.publications.values.none { it.topologyChange } &&
            record.hosts.none { it !== reserved && (!it.assigned || it.removing) }

    /** No VD, BLAST creation, transaction apply or worker wait occurs on this call's thread. */
    fun prepare(source: Any, hideTarget: Any, parent: Any, width: Int, height: Int,
                ownerTaskIdentity: Any): Boolean {
        if (width <= 0 || height <= width || width > MAX_EXTENT || height > MAX_EXTENT) return false
        val sourceLayer = layer(source)
        val hideLayer = layer(hideTarget)
        val parentLayer = layer(parent)
        if (sourceLayer < 0 || hideLayer < 0 || parentLayer < 0 ||
            sourceLayer == hideLayer || hideLayer == parentLayer || sourceLayer == parentLayer) return false
        synchronized(lock) {
            if (closed) return false
            records[sourceLayer]?.takeIf {
                it.matches(hideLayer, parentLayer, width, height, ownerTaskIdentity)
            }?.let { touch(it); return true }
        }
        val held = ArrayList<Any>(3)
        val sourceCopy: Any
        val hideCopy: Any
        val parentCopy: Any
        try {
            sourceCopy = retain(source, "Genie.subtree.source").also(held::add)
            hideCopy = retain(hideTarget, "Genie.subtree.window").also(held::add)
            parentCopy = retain(parent, "Genie.subtree.parent").also(held::add)
        } catch (error: Throwable) {
            held.forEach(::release)
            report("cannot retain subtree capabilities", error)
            return false
        }
        synchronized(lock) {
            if (closed) { held.forEach(::release); return false }
            val previous = records[sourceLayer]
            if (previous != null && previous.matches(hideLayer, parentLayer, width, height, ownerTaskIdentity)) {
                touch(previous)
                held.forEach(::release)
                return true
            }
            // Active/guarded streams are pinned until their owner restores or invalidates them.
            // A third concurrent stream must not uncover either still-visible predecessor.
            val evicted = if (previous == null && records.size >= MAX_ENTRIES)
                records.values.firstOrNull { !it.active && !it.guarded && it.restoreState == 0 &&
                    it.publications.isEmpty() } else null
            if (previous == null && records.size >= MAX_ENTRIES && evicted == null) {
                held.forEach(::release)
                return false
            }
            previous?.let { retireLocked(it) }
            evicted?.let { retireLocked(it) }
            val record = Record(++nextGeneration, sourceLayer, hideLayer, parentLayer,
                sourceCopy, hideCopy, parentCopy, width, height, ownerTaskIdentity)
            records[sourceLayer] = record
            if (!handler.post { create(record) }) {
                records.remove(sourceLayer, record)
                record.retired = true
                held.forEach(::release)
                return false
            }
        }
        return true
    }

    fun prepared(sourceLayer: Int): Boolean = synchronized(lock) {
        val record = records[sourceLayer]
        !closed && record != null && !record.retired
    }

    /** Binder capability endpoint only; never call while holding WMS or SC locks. */
    fun awaitReady(sourceLayer: Int, timeoutMillis: Long): Boolean {
        val record = synchronized(lock) {
            records[sourceLayer]?.takeIf { !closed && !it.retired } ?: return false
        }
        // The signal is emitted only after SF commits a real first buffer, or
        // retirement wakes the waiter. Waiting authorizes no empty publication.
        try {
            if (!record.readySignal.await(timeoutMillis, TimeUnit.MILLISECONDS)) return false
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
            return false
        }
        return synchronized(lock) {
            !closed && records[sourceLayer] === record && !record.retired && record.bufferReady &&
                publicationReady(record)
        }
    }

    fun ready(sourceLayer: Int): Entry? = synchronized(lock) {
        val record = records[sourceLayer] ?: return@synchronized null
        if (closed || record.retired || !record.bufferReady || !publicationReady(record)) return@synchronized null
        if (!valid(record.source) || !valid(record.parent) || !valid(record.hideTarget) ||
            !valid(record.output)) return@synchronized null
        Entry(record)
    }

    /** Hide only the external WindowState container, allowing the mirror to produce its first buffer. */
    fun guard(transaction: Any, sourceLayer: Int): Boolean = synchronized(lock) {
        val record = records[sourceLayer] ?: return@synchronized false
        if (closed || record.retired || !publicationReady(record)) return@synchronized false
        if (record.active && !record.activeCommitted) {
            // Activation belongs to a borrowed Launcher host transaction. A
            // concurrent WMS show may guard the source, but cannot reveal the
            // output before that host geometry reaches SF.
            return@synchronized publication(record, transaction, record.active, record.guarded, hideOnly = true)
        }
        publication(record, transaction, active = record.active, guarded = true)
    }

    fun activeForHideTarget(hideTargetLayer: Int): Boolean = synchronized(lock) {
        records.values.any { !it.retired && it.hideLayer == hideTargetLayer && (it.active || it.guarded) }
    }

    /** Reinforce a later WMS show without beginning a new guard for an idle source. */
    fun guardHideTarget(transaction: Any, hideTargetLayer: Int): Boolean = synchronized(lock) {
        val record = records.values.lastOrNull {
            !it.retired && it.hideLayer == hideTargetLayer && (it.active || it.guarded)
        } ?: return@synchronized false
        if (!publicationReady(record)) return@synchronized false
        if (record.active && !record.activeCommitted) {
            return@synchronized publication(record, transaction, record.active, record.guarded, hideOnly = true)
        }
        publication(record, transaction, record.active, record.guarded)
    }

    /** Call in the same transaction as the output's first native SELF publication. */
    fun activate(entry: Entry, transaction: Any): Boolean = activate(entry, transaction, 0L, null)

    fun activate(entry: Entry, transaction: Any, eventNs: Long, host: ClosingHostLease?): Boolean = synchronized(lock) {
        val record = entry.record
        if (!entry.valid() || closed || records[entry.sourceLayer] !== record ||
            record.retired || !record.bufferReady || !publicationReady(record)) return@synchronized false
        if (host != null && (!host.valid() || host.record !== record || host.host.eventNs != eventNs ||
                host.host.removing || !valid(host.surface))) return@synchronized false
        if (record.presentation?.eventNs?.let { it > eventNs } == true ||
            records.values.any { it.task === record.task && !topologyReady(it, host?.host) } ||
            retiring.any { it.task === record.task }) return@synchronized false
        // A recreated WindowState can produce a new source for the same Task
        // while the previous group is still animating. Retire its visibility
        // in this publication, before the new output is revealed.
        for (previous in records.values) {
            if (previous !== record && !previous.retired && previous.task === record.task &&
                (previous.active || previous.guarded) &&
                !publication(previous, transaction, active = false, guarded = false)) return@synchronized false
        }
        publication(record, transaction, active = true, guarded = false,
            presentation = host?.host, changePresentation = record.presentation !== host?.host)
    }

    fun restore(entry: Entry, transaction: Any): Boolean = synchronized(lock) {
        if (!entry.valid() || records[entry.sourceLayer] !== entry.record) return@synchronized false
        // Native priming may reject an activation before its transaction is
        // applied/exported. Undo that publication in the same atomic batch.
        val rollback = entry.record.publications.values.any {
            it.transaction === transaction && it.visibilityChange
        }
        restore(entry.record, transaction, rollback)
    }

    /** Also releases an opening guard whose first buffer has not arrived. */
    fun restore(sourceLayer: Int, transaction: Any): Boolean = synchronized(lock) {
        val record = records[sourceLayer] ?: return@synchronized false
        restore(record, transaction, rollback = false)
    }

    private fun restore(record: Record, transaction: Any, rollback: Boolean): Boolean {
        if (closed || record.retired || record.restoreState == 1) return false
        // A late borrowed hide would otherwise land after this source reveal.
        // Keep the current masked output until every older write has committed.
        if (!rollback && record.publications.isNotEmpty()) return false
        return publication(record, transaction, active = false, guarded = false)
    }

    /** An idle native writer does not prove that its visible output has been hidden. */
    fun restored(sourceLayer: Int): Boolean = synchronized(lock) {
        retiring.none { it.sourceLayer == sourceLayer } && records[sourceLayer].let {
            it == null || (!it.active && !it.guarded && it.restoreState == 0 && it.publications.isEmpty() && it.hosts.isEmpty())
        }
    }

    fun restoring(sourceLayer: Int): Boolean = synchronized(lock) {
        records[sourceLayer]?.let { it.restoreState == 1 ||
            (!it.active && !it.guarded && it.restoreState == 0 && it.publications.isNotEmpty()) } == true ||
            retiring.any { it.sourceLayer == sourceLayer }
    }

    fun snapshotReady(task: Any): Boolean = synchronized(lock) {
        records.values.none { it.task === task && (it.active || it.guarded || it.restoreState != 0 ||
            it.publications.isNotEmpty() || it.hosts.isNotEmpty()) } &&
            retiring.none { it.task === task }
    }

    /** A guarded source may feed a closing preview; a composed output must finish retiring first. */
    fun previewReady(task: Any): Boolean = synchronized(lock) {
        !closed && records.values.none { it.task === task && (it.active || it.restoreState != 0 ||
            it.hosts.isNotEmpty() || (!it.guarded && it.publications.isNotEmpty())) } && retiring.none { it.task === task }
    }

    /** A rejected apply has no commit to wait for; retain the barrier and retry its restore. */
    fun restoreFailed(sourceLayer: Int): Unit = synchronized(lock) {
        records[sourceLayer]?.takeIf { it.restoreState == 1 }?.let {
            it.restoreState = 2
            scheduleRetry(it)
        }
        Unit
    }

    private fun publicationReady(record: Record): Boolean = record.restoreState == 0 &&
        (record.active || record.guarded || record.publications.isEmpty()) &&
        records.values.none { it !== record && it.hideLayer == record.hideLayer &&
            (it.restoreState != 0 || (!it.active && !it.guarded && it.publications.isNotEmpty())) } &&
        retiring.none { it.hideLayer == record.hideLayer }

    /** Only for a transaction which was never applied or exported to another process. */
    fun discardPublication(transaction: Any) = synchronized(lock) {
        (records.values + retiring).forEach { record ->
            val discarded = record.publications.filterValues { it.transaction === transaction }
            if (discarded.isEmpty()) return@forEach
            discarded.keys.forEach(record.publications::remove)
            if (discarded.values.any { it.topologyChange }) record.hosts.forEach { host ->
                if (host.removalTransaction === transaction) {
                    host.removing = false
                    host.removalTransaction = null
                }
            }
            val changedVisibility = discarded.values.any { it.visibilityChange }
            if (changedVisibility) ++record.visibilitySerial
            if (record.retired) finishRetirement(record)
            else if (changedVisibility) {
                record.active = false
                record.guarded = false
                record.restoreState = 2
                scheduleRetry(record)
            }
        }
    }

    private fun scheduleRetry(record: Record) {
        if (record.retryScheduled) return
        record.retryScheduled = true
        val delay = record.retryDelay
        record.retryDelay = (delay * 2).coerceAtMost(MAX_RESTORE_RETRY_MS)
        handler.postDelayed({
            synchronized(lock) { record.retryScheduled = false }
            if (record.retired) destroy(record) else retryRestore(record)
        }, delay)
    }

    private fun retryRestore(record: Record): Unit = synchronized(lock) {
        if (closed || record.retired || records[record.sourceLayer] !== record || record.restoreState != 2)
            return@synchronized
        if (record.publications.isNotEmpty()) {
            scheduleRetry(record)
            return@synchronized
        }
        runCatching {
            ownedTransaction { transaction -> check(publication(record, transaction, active = false, guarded = false)) }
        }.onFailure {
            record.restoreState = 2
            report("subtree restore retry failed source=${record.sourceLayer}", it)
            scheduleRetry(record)
        }
        Unit
    }

    /** For actual source removal, rather than cancellation of a particular animation generation. */
    fun invalidate(sourceLayer: Int) = synchronized(lock) {
        records[sourceLayer]?.let(::retireLocked)
    }

    private fun touch(record: Record) {
        records.remove(record.sourceLayer)
        records[record.sourceLayer] = record
    }

    private fun create(record: Record) {
        val current = synchronized(lock) { !closed && !record.retired && records[record.sourceLayer] === record }
        if (!current) { destroy(record); return }
        try {
            val name = "Genie.Subtree.${record.sourceLayer}.${record.generation}"
            val output = control("$name.output", record.parent, record.width, record.height)
            synchronized(lock) { record.output = output }
            record.mirrorHost = control("$name.mirror-host", null)
            record.mirror = mirrorSurface.invoke(null, record.source)
            val queue = blastType.getConstructor(String::class.java, Boolean::class.javaPrimitiveType)
                .newInstance(name, true)
            record.queue = queue
            updateBlast.invoke(queue, output, record.width, record.height, PixelFormat.RGBA_8888)
            record.sink = createBlastSurface.invoke(queue) as Surface
            // This callback transfers ownership of the real buffer transaction to us.
            // Its receipt and commit, rather than display creation, establish readiness.
            val callback = Consumer<Any> { transaction ->
                if (!handler.post { firstBuffer(record, transaction) }) close(transaction)
            }
            check(syncBlast.invoke(queue, callback) != false) { "first buffer sync rejected" }
            record.display = createDisplay.invoke(null, name, false) as? IBinder
                ?: error("virtual display creation rejected")
            val stack = nextStack.incrementAndGet()
            ownedTransaction { transaction ->
                // Its source container is hidden during activation. Use the
                // Task-local output order without a visibility dependency on it.
                setLayer.invoke(transaction, output, OUTPUT_LAYER)
                setTrustedOverlay.invoke(transaction, output, true)
                hide.invoke(transaction, output)
                setDisplaySurface.invoke(transaction, record.display, record.sink)
                setDisplayLayerStack.invoke(transaction, record.display, stack)
                val bounds = Rect(0, 0, record.width, record.height)
                setDisplayProjection.invoke(transaction, record.display, 0, bounds, bounds)
                setDisplaySize.invoke(transaction, record.display, record.width, record.height)
                // A parentless mirror is an offscreen root on this ROM. A regular
                // Builder-created host is essential for entry into the VD hierarchy.
                setLayerStack.invoke(transaction, record.mirrorHost, stack)
                reparent.invoke(transaction, record.mirror, record.mirrorHost)
                show.invoke(transaction, record.mirrorHost)
                show.invoke(transaction, record.mirror)
            }
            // Cleanup deadline only: never use this timer to claim readiness or discover content.
            handler.postDelayed({
                synchronized(lock) {
                    if (!record.bufferReady && records[record.sourceLayer] === record) {
                        report("first buffer timed out source=${record.sourceLayer}")
                        retireLocked(record)
                    }
                }
            }, FIRST_BUFFER_TIMEOUT_MS)
        } catch (error: Throwable) {
            report("subtree preparation failed source=${record.sourceLayer}", error)
            synchronized(lock) { retireLocked(record) }
        }
    }

    private fun firstBuffer(record: Record, transaction: Any) {
        try {
            val current = synchronized(lock) { !closed && !record.retired && records[record.sourceLayer] === record }
            if (!current) return
            check((lastFrame.invoke(checkNotNull(record.queue)) as Long) > 0L) {
                "sync callback had no acquired frame"
            }
            committed(transaction) {
                handler.post {
                    val ready = synchronized(lock) {
                        if (closed || record.retired || records[record.sourceLayer] !== record) false
                        else { record.bufferReady = true; true }
                    }
                    if (ready) {
                        record.readySignal.countDown()
                        module.log(Log.INFO, TAG, "subtree ready source=${record.sourceLayer} " +
                            "output=${layer(record.output)} generation=${record.generation}")
                        notifyReady()
                    }
                }
            }
            applyTransaction.invoke(transaction)
        } catch (error: Throwable) {
            report("first buffer publication failed source=${record.sourceLayer}", error)
            synchronized(lock) { if (!record.retired) retireLocked(record) }
        } finally { close(transaction) }
    }

    /** Caller holds lock. No borrowed transaction escapes this method. */
    private fun publication(record: Record, transaction: Any, active: Boolean, guarded: Boolean,
                            hideOnly: Boolean = false, presentation: ClosingHost? = record.presentation,
                            changePresentation: Boolean = false): Boolean {
        if (!valid(record.hideTarget) || (active && !valid(record.output))) return false
        val topologyChange = changePresentation || (!active && !guarded &&
            (record.presentation != null || record.hosts.any { it.assigned && !it.removing }))
        if (topologyChange && record.publications.values.any {
                it.topologyChange && it.transaction !== transaction }) return false
        val targetHost = if (active) presentation else null
        if (topologyChange && targetHost != null &&
            (targetHost.removing || !valid(targetHost.surface) || !valid(targetHost.anchor))) return false
        // WMS may reuse a Java Transaction after apply while its prior commit
        // is pending. Track every append instead of deduplicating by wrapper.
        val hideCopy = retain(record.hideTarget, "Genie.subtree.publication.window")
        val outputCopy = record.output?.let { retain(it, "Genie.subtree.publication.output") }
        val publicationId = ++record.nextPublicationId
        val serial = if (hideOnly) record.visibilitySerial else ++record.visibilitySerial
        record.publications[publicationId] = Publication(transaction, !hideOnly, topologyChange)
        val removedHosts = ArrayList<ClosingHost>()
        if (!hideOnly) {
            record.active = active
            if (active) record.activeCommitted = false
            record.guarded = guarded
            if (!active && !guarded) record.restoreState = 1
        }
        record.touched = true
        try {
            committed(transaction) {
                val work = Runnable {
                    val stale = synchronized(lock) {
                            // A hide-only commit never publishes the output or
                            // completes its separate host activation. Repair it
                            // only when no current owner still needs this hide.
                            if (hideOnly) records.values.none {
                                !it.retired && it.hideLayer == record.hideLayer && (it.active || it.guarded)
                            }
                            else if (record.retired || records[record.sourceLayer] !== record ||
                                record.visibilitySerial != serial) true
                            else {
                                record.activeCommitted = active
                                if (!record.active && !record.guarded) {
                                    record.touched = false
                                    record.restoreState = 0
                                    record.retryDelay = RESTORE_RETRY_MS
                                }
                                false
                            }
                    }
                    val settled = {
                        synchronized(lock) {
                            record.publications.remove(publicationId)
                            removedHosts.forEach { releaseHost(record, it) }
                            finishRetirement(record)
                        }
                        release(hideCopy)
                        release(outputCopy)
                        notifyReady()
                    }
                    if (stale) repair(record, hideCopy, outputCopy, settled) else settled()
                }
                // A caller can commit a formerly borrowed transaction after close().
                // Keep its owned capability copies until that exact commit, then repair
                // on a cleanup worker even if the regular HandlerThread has exited.
                if (!handler.post(work)) Thread(work, "Genie.SubtreeRetire").start()
            }
        } catch (error: Throwable) {
            record.publications.remove(publicationId)
            release(hideCopy)
            release(outputCopy)
            if (!hideOnly) {
                record.active = false
                record.guarded = false
                record.restoreState = 2
                scheduleRetry(record)
            }
            report("cannot track subtree publication source=${record.sourceLayer}", error)
            return false
        }
        return try {
            if (topologyChange) {
                val output = checkNotNull(record.output)
                // Only module-owned output moves. The original Task/source
                // remain untouched, and an old host becomes an empty target.
                record.presentation = targetHost
                if (targetHost != null) {
                    setTrustedOverlay.invoke(transaction, targetHost.surface, true)
                    reparent.invoke(transaction, output, targetHost.surface)
                    setRelativeLayer.invoke(transaction, targetHost.surface, targetHost.anchor, 1)
                    show.invoke(transaction, targetHost.surface)
                    targetHost.assigned = true
                } else {
                    reparent.invoke(transaction, output, record.parent)
                    setLayer.invoke(transaction, output, OUTPUT_LAYER)
                }
                for (old in record.hosts) if (old !== targetHost && old.assigned && !old.removing) {
                    remove.invoke(transaction, old.surface)
                    old.removing = true
                    old.removalTransaction = transaction
                    removedHosts += old
                }
            }
            if (active || guarded) hide.invoke(transaction, record.hideTarget)
            else show.invoke(transaction, record.hideTarget)
            record.output?.takeUnless { hideOnly }?.let {
                if (active) {
                    // This visual copy must not block touches to the real Window,
                    // including while it is displayed outside the original Task.
                    setTrustedOverlay.invoke(transaction, it, true)
                    show.invoke(transaction, it)
                } else {
                    hide.invoke(transaction, it)
                    if (!guarded) synchronized(checkNotNull(surfaceLock.get(it))) {
                        ContentAnimator.clearRetiredGroup(transactionObject.getLong(transaction), nativeObject.getLong(it))
                    }
                }
            }
            true
        } catch (error: Throwable) {
            if (!hideOnly) {
                ++record.visibilitySerial // Any partially appended transaction now requires commit repair.
                record.active = false
                record.guarded = false
                record.restoreState = 2
                scheduleRetry(record)
            }
            report("subtree visibility append failed source=${record.sourceLayer}", error)
            false
        }
    }

    /** Correct only our own visibility changes; never reparent a source or clear another group's effect. */
    private fun repair(record: Record, hideTarget: Any, oldOutput: Any?, settled: () -> Unit): Unit = synchronized(lock) {
        val owner = records.values.lastOrNull {
            !it.retired && it.hideLayer == record.hideLayer && (it.active || it.guarded)
        }
        // Neither reveal nor hide over a borrowed host activation. Its commit
        // establishes visibility; a stale cleanup transaction cannot order it.
        if (owner === record && record.active && !record.activeCommitted) {
            settled()
            return@synchronized
        }
        val desired = (owner != null) to (owner === record && record.active && record.activeCommitted)
        runCatching {
            ownedTransaction { transaction ->
                committed(transaction, settled)
                if (valid(hideTarget)) {
                    if (desired.first) hide.invoke(transaction, hideTarget) else show.invoke(transaction, hideTarget)
                }
                if (valid(oldOutput)) {
                    if (desired.second) show.invoke(transaction, oldOutput) else hide.invoke(transaction, oldOutput)
                }
            }
        }.onFailure {
            report("late publication repair failed source=${record.sourceLayer}", it)
            handler.postDelayed({ repair(record, hideTarget, oldOutput, settled) }, MAX_RESTORE_RETRY_MS)
        }
        Unit
    }

    private fun finishRetirement(record: Record) {
        if (record.removalCommitted && record.publications.isEmpty() && record.hosts.isEmpty()) retiring.remove(record)
        if (closed && retiring.isEmpty()) worker.quitSafely()
    }

    private fun releaseHost(record: Record, host: ClosingHost) {
        if (!record.hosts.remove(host)) return
        release(host.anchor)
        release(host.surface)
    }

    /** A failed prepare never attached its output, but still owns this hidden container. */
    private fun removeUnusedHost(record: Record, host: ClosingHost) {
        if (host.removing || host !in record.hosts) return
        try {
            ownedTransaction { transaction ->
                committed(transaction) {
                    synchronized(lock) { releaseHost(record, host); finishRetirement(record) }
                    notifyReady()
                }
                remove.invoke(transaction, host.surface)
                host.removing = true
                host.removalTransaction = transaction
            }
        } catch (error: Throwable) {
            host.removing = false
            host.removalTransaction = null
            report("unused closing host removal failed source=${record.sourceLayer}", error)
            handler.postDelayed({ synchronized(lock) { removeUnusedHost(record, host) } }, MAX_RESTORE_RETRY_MS)
        }
    }

    private fun retireLocked(record: Record) {
        if (record.retired) return
        records.remove(record.sourceLayer, record)
        record.retired = true
        retiring.add(record)
        record.bufferReady = false
        record.readySignal.countDown()
        handler.post { destroy(record); notifyReady() }
    }

    /** Worker only. All visibility restoration/removals enter one owned transaction. */
    private fun destroy(record: Record) {
        if (record.destroyed) return
        synchronized(lock) {
            // A borrowed initial reparent could otherwise resurrect the removed
            // output and host after this independent cleanup transaction.
            if (record.publications.values.any { it.topologyChange }) {
                scheduleRetry(record)
                return
            }
        }
        record.destroyed = true
        val (hidden, hosts) = synchronized(lock) {
            records.values.any {
                !it.retired && it.hideLayer == record.hideLayer && (it.active || it.guarded)
            } to record.hosts.toList()
        }
        val removed = runCatching {
            ownedTransaction { transaction ->
                committed(transaction) {
                    synchronized(lock) {
                        hosts.forEach { releaseHost(record, it) }
                        record.presentation = null
                        record.removalCommitted = true
                        finishRetirement(record)
                    }
                    notifyReady()
                }
                if (record.touched && valid(record.hideTarget)) {
                    if (hidden) hide.invoke(transaction, record.hideTarget)
                    else show.invoke(transaction, record.hideTarget)
                }
                (listOfNotNull(record.output, record.mirror, record.mirrorHost) +
                    hosts.map { it.surface }).forEach {
                    if (valid(it)) remove.invoke(transaction, it)
                }
            }
        }.onFailure { report("subtree removal failed source=${record.sourceLayer}", it) }.isSuccess
        if (!removed) {
            record.destroyed = false
            synchronized(lock) { scheduleRetry(record) }
            return
        }
        record.display?.let { runCatching { destroyDisplay.invoke(null, it) }
            .onFailure { error -> report("virtual display removal failed source=${record.sourceLayer}", error) } }
        record.sink?.release()
        record.sink = null
        record.queue?.let { runCatching { destroyBlast.invoke(it) }
            .onFailure { error -> report("BLAST removal failed source=${record.sourceLayer}", error) } }
        record.queue = null
        listOf(record.mirror, record.mirrorHost, record.output, record.source, record.hideTarget, record.parent)
            .forEach(::release)
    }

    private fun control(name: String, parent: Any?, width: Int = 0, height: Int = 0): Any {
        val builder = builderType.getConstructor().newInstance()
        nameBuilder.invoke(builder, name)
        if (parent != null) parentBuilder.invoke(builder, parent)
        if (width > 0 && height > 0) {
            bufferSizeBuilder.invoke(builder, width, height)
            blastBuilder.invoke(builder)
        } else containerBuilder.invoke(builder)
        return build.invoke(builder)
    }

    private fun committed(transaction: Any, action: () -> Unit) {
        val once = AtomicBoolean()
        val listener = Proxy.newProxyInstance(listenerType.classLoader, arrayOf(listenerType)) { proxy, method, args ->
            when (method.name) {
                "onTransactionCommitted" -> { if (once.compareAndSet(false, true)) action(); null }
                "hashCode" -> System.identityHashCode(proxy)
                "equals" -> proxy === args?.firstOrNull()
                "toString" -> "Genie.SubtreeCommit"
                else -> null
            }
        }
        addCommitListener.invoke(transaction, direct, listener)
    }

    private fun ownedTransaction(append: (Any) -> Unit) {
        val transaction = transactionConstructor.newInstance()
        try {
            try { append(transaction) }
            catch (error: Throwable) { discardPublication(transaction); throw error }
            applyTransaction.invoke(transaction)
        }
        finally { close(transaction) }
    }

    private fun retain(surface: Any, callsite: String): Any = synchronized(checkNotNull(surfaceLock.get(surface))) {
        check(validSurface.invoke(surface) == true) { "released $callsite" }
        copySurface.newInstance(surface, callsite)
    }

    private fun valid(surface: Any?): Boolean = surface != null &&
        synchronized(checkNotNull(surfaceLock.get(surface))) { validSurface.invoke(surface) == true }

    private fun layer(surface: Any?): Int = if (surface == null) -1 else
        synchronized(checkNotNull(surfaceLock.get(surface))) {
            if (validSurface.invoke(surface) == true) surfaceLayer.invoke(surface) as Int else -1
        }

    private fun release(surface: Any?) { if (surface != null) runCatching { releaseSurface.invoke(surface) } }
    private fun close(transaction: Any) { runCatching { closeTransaction.invoke(transaction) } }
    private fun notifyReady() { runCatching(onReady).onFailure { report("subtree event callback failed", it) } }
    private fun report(message: String, error: Throwable? = null) {
        if (error == null) module.log(Log.WARN, TAG, message) else module.log(Log.WARN, TAG, message, error)
    }

    /** Runtime disable releases pipelines while retaining the reusable worker. */
    fun clear() = synchronized(lock) {
        records.values.toList().forEach(::retireLocked)
    }

    override fun close() {
        synchronized(lock) {
            if (closed) return
            closed = true
            val closing = records.values.toList()
            records.clear()
            retiring.addAll(closing)
            closing.forEach { it.retired = true; it.bufferReady = false; it.readySignal.countDown() }
            handler.post {
                closing.forEach(::destroy)
                synchronized(lock) { if (retiring.isEmpty()) worker.quitSafely() }
            }
        }
    }

    internal data class Publication(val transaction: Any, val visibilityChange: Boolean,
                                    val topologyChange: Boolean = false)

    internal class ClosingHost(val surface: Any, val anchor: Any, val eventNs: Long,
                               val areaLayer: Int, val anchorLayer: Int) {
        var assigned = false
        var removing = false
        var removalTransaction: Any? = null
    }

    internal class Record(
        val generation: Long, val sourceLayer: Int, val hideLayer: Int, val parentLayer: Int,
        val source: Any, val hideTarget: Any, val parent: Any, val width: Int, val height: Int,
        val task: Any,
    ) {
        var output: Any? = null
        var mirrorHost: Any? = null
        var mirror: Any? = null
        var queue: Any? = null
        var sink: Surface? = null
        var display: IBinder? = null
        var retired = false
        var destroyed = false
        var removalCommitted = false
        val readySignal = CountDownLatch(1)
        var bufferReady = false
        var active = false
        var activeCommitted = false
        var guarded = false
        var restoreState = 0 // 0: committed/unused, 1: awaiting commit, 2: retry required.
        val publications = LinkedHashMap<Long, Publication>()
        var nextPublicationId = 0L
        var retryScheduled = false
        var retryDelay = RESTORE_RETRY_MS
        var touched = false
        var visibilitySerial = 0L
        var presentation: ClosingHost? = null
        val hosts = LinkedHashSet<ClosingHost>()
        fun matches(hideLayer: Int, parentLayer: Int, width: Int, height: Int, task: Any): Boolean =
            !retired && this.hideLayer == hideLayer && this.parentLayer == parentLayer &&
                this.width == width && this.height == height && this.task === task
    }

    private companion object {
        const val TAG = "Genie.Subtrees"
        const val MAX_ENTRIES = 4
        const val MAX_EXTENT = 16384
        const val OUTPUT_LAYER = 1_000_000
        const val FIRST_BUFFER_TIMEOUT_MS = 2000L
        const val RESTORE_RETRY_MS = 50L
        const val MAX_RESTORE_RETRY_MS = 2000L
        val nextStack = AtomicInteger(0x48480000)
    }
}
