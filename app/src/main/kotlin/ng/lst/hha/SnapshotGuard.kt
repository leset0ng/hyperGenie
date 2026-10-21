package ng.lst.hha

import android.annotation.SuppressLint
import android.util.Log
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedInterface.HookHandle
import io.github.libxposed.api.XposedModule
import java.lang.reflect.Field
import java.lang.reflect.Method
import java.util.concurrent.ConcurrentHashMap
import java.util.function.Supplier
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/**
 * Keeps task snapshots pristine while the central renderer deforms App content.
 * Only system_server is hooked; existing snapshots remain available to Shell.
 *
 * Call block before attaching any effect. Unblock only after native detachment
 * has completed, then call refresh on the bridge worker after the Task is idle.
 * The caller owns delayed-work generations: an old release must not unblock a
 * new animation of the same Task. This class creates no timer or render thread.
 */
@SuppressLint("PrivateApi", "BlockedPrivateApi", "SoonBlockedPrivateApi")
internal class SnapshotGuard(private val module: XposedModule, loader: ClassLoader) : AutoCloseable {
    private val taskClass = Class.forName("com.android.server.wm.Task", false, loader)
    private val baseClass = Class.forName("com.android.server.wm.AbsAppSnapshotController", false, loader)
    private val controllerClass = Class.forName("com.android.server.wm.TaskSnapshotController", false, loader)
    private val taskIdField = field(taskClass, "mTaskId")
    private val members = Members()
    private val hooks = ArrayList<HookHandle>(5)
    private val states = ConcurrentHashMap<Int, State>()
    @Volatile private var closed = false

    /**
     * State locks serialize block with an already-running capture of that Task.
     * Locks have process lifetime so removing an idle entry cannot create two
     * different locks for the same Task during an in-flight capture.
     */
    private class State { val lock = ReentrantLock(); var blocked = false; var ownerNs = 0L }

    fun install() {
        check(!closed && hooks.isEmpty())
        try {
            // Actual ROM: snapshot() checks this native screenshot result for
            // null before building a TaskSnapshot. captureSnapshot itself must
            // retain its valid empty SnapshotSupplier: recordSnapshotInner calls
            // supplier.setConsumer() without a null check.
            val capture = baseClass.declaredMethods.single { method ->
                method.name == "createSnapshot" && method.parameterCount == 5 &&
                    method.parameterTypes[1] == Float::class.javaPrimitiveType &&
                    method.parameterTypes[4] == Boolean::class.javaPrimitiveType
            }
            hooks += module.hook(capture)
                .setPriority(XposedInterface.PRIORITY_DEFAULT)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("pristine-task-native-snapshot")
                .intercept { chain ->
                    val source = chain.getArg(0)
                    if (closed || source == null || !taskClass.isInstance(source)) {
                        return@intercept chain.proceed()
                    }
                    val id = taskIdField.getInt(source)
                    val state = states.getOrPut(id) { State() }
                    state.lock.withLock {
                        if (!closed && state.blocked) {
                            report(Log.DEBUG, "skip capture task=$id while content is deformed")
                            null
                        } else chain.proceed()
                    }
                }

            // Avoid preparation and persistence scheduling for normal record
            // requests. Direct captures and inherited snapshot overloads are
            // still covered by the native createSnapshot hook above.
            val record = controllerClass.getDeclaredMethod("recordSnapshot", taskClass)
            hooks += module.hook(record)
                .setPriority(XposedInterface.PRIORITY_DEFAULT)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("pristine-task-record-snapshot")
                .intercept { chain ->
                    val source = chain.getArg(0)
                    if (closed || source == null) return@intercept chain.proceed()
                    val id = taskIdField.getInt(source)
                    val state = states.getOrPut(id) { State() }
                    val blocked = state.lock.withLock { !closed && state.blocked }
                    if (blocked) {
                        report(Log.DEBUG, "keep cached snapshot task=$id while content is deformed")
                        null
                    } else {
                        // Do not hold the state lock across record's consumer:
                        // the consumer acquires mGlobalLock. The lower capture
                        // hook rechecks and serializes the screenshot itself.
                        chain.proceed()
                    }
                }
            installOemCaptures()
            report(Log.INFO, "task snapshot guard installed")
        } catch (error: Throwable) {
            close()
            throw error
        }
    }

    /** OEM startup caches capture the Task directly, bypassing TaskSnapshotController. */
    private fun installOemCaptures() {
        val manager = Class.forName("com.android.server.wm.PreStartingManager", false, taskClass.classLoader)
        val activity = Class.forName("com.android.server.wm.ActivityRecord", false, taskClass.classLoader)
        val display = Class.forName("com.android.server.wm.DisplayContent", false, taskClass.classLoader)
        val signatures = listOf(
            manager.getDeclaredMethod("captureSplashLayer", activity, display, taskClass) to 2,
            manager.getDeclaredMethod("getBuffer", display, activity, taskClass, ArrayList::class.java) to 2,
            manager.getDeclaredMethod("captureActivity", activity, display, taskClass,
                Boolean::class.javaPrimitiveType) to 2,
        )
        for ((capture, taskArgument) in signatures) {
            hooks += module.hook(capture)
                .setPriority(XposedInterface.PRIORITY_DEFAULT)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("pristine-oem-${capture.name}")
                .intercept { chain ->
                    val task = chain.getArg(taskArgument)
                    if (closed || task == null) return@intercept chain.proceed()
                    val id = taskIdField.getInt(task)
                    val state = states.getOrPut(id) { State() }
                    // Serialize the actual screenshot with block(), including
                    // captures queued before the animation began. Returning null
                    // is the ROM's existing no-buffer/void path, keeping its cache.
                    state.lock.withLock {
                        if (!closed && state.blocked) {
                            report(Log.DEBUG, "skip OEM ${capture.name} task=$id while content is deformed")
                            null
                        } else chain.proceed()
                    }
                }
        }
        report(Log.INFO, "OEM startup capture guard installed (${signatures.size} paths)")
    }

    /** May wait for an existing pristine capture; returns before effects attach. */
    fun block(taskId: Int, ownerNs: Long): Boolean {
        if (taskId < 0 || ownerNs <= 0 || closed) return false
        val state = states.getOrPut(taskId) { State() }
        return state.lock.withLock {
            if (closed || ownerNs < state.ownerNs) false else {
                state.ownerNs = ownerNs
                state.blocked = true
                true
            }
        }
    }

    /** First-show hooks must not wait for an in-flight screenshot under WMS. */
    fun tryBlock(taskId: Int, ownerNs: Long): Boolean {
        if (taskId < 0 || ownerNs <= 0 || closed) return false
        val state = states.getOrPut(taskId) { State() }
        if (!state.lock.tryLock()) return false
        return try {
            if (closed || ownerNs < state.ownerNs) false else {
                state.ownerNs = ownerNs
                state.blocked = true
                true
            }
        } finally { state.lock.unlock() }
    }

    /** An old release cannot unblock a later first-show publication. */
    fun unblock(taskId: Int, ownerNs: Long) {
        val state = states[taskId] ?: return
        state.lock.withLock { if (state.ownerNs == ownerNs) state.blocked = false }
    }

    /**
     * Refreshes only this attached, visible fullscreen Task through the ROM's
     * normal record/cache/persist consumer. Invoke after detachment on a worker;
     * never from the renderer, a Binder animation callback, or under SC locks.
     *
     * Snapshot preparation/capture follows WMS's own global-lock convention.
     * Supplier consumption (which reacquires WMS for the cache consumer) runs
     * after releasing that lock. A newer block prevents the capture rather than
     * toggling live effects. False leaves the previous snapshot untouched.
     */
    fun refresh(service: Any, taskId: Int): Boolean {
        if (closed || taskId < 0) return false
        return runCatching {
            val globalLock = members.value(service, "mGlobalLock") ?: return false
            val supplier: Supplier<*> = synchronized(globalLock) {
                val state = states.getOrPut(taskId) { State() }
                state.lock.withLock {
                    if (closed || state.blocked) return false
                    val root = members.value(service, "mRoot") ?: return false
                    val task = members.method(root, "anyTaskForId", Int::class.javaPrimitiveType!!,
                        Int::class.javaPrimitiveType!!).invoke(root, taskId, 1) ?: return false
                    if (!taskClass.isInstance(task) ||
                        members.invoke(task, "isAttached") != true ||
                        members.invoke(task, "isVisible") != true ||
                        members.invoke(task, "isVisibleRequested") != true ||
                        members.invoke(task, "isActivityTypeHome") == true ||
                        members.invoke(task, "getWindowingMode") != 1) return false
                    val activity = members.invoke(task, "getTopVisibleActivity") ?: return false
                    val window = members.invoke(activity, "findMainWindow") ?: return false
                    val animator = members.value(window, "mWinAnimator") ?: return false
                    val drawState = members.value(animator, "mDrawState") as? Int ?: return false
                    if (drawState < 4) return false
                    val controller = members.value(service, "mTaskSnapshotController") ?: return false
                    // Actual ROM recordSnapshot(Task) uses this supplier and its
                    // built-in consumer to cache, persist and notify the Task.
                    val prepare = members.method(controller, "getRecordSnapshotSupplier", taskClass,
                        Int::class.javaPrimitiveType!!)
                    prepare.invoke(controller, task, 0) as? Supplier<*> ?: return false
                }
            }
            val captured = supplier.get() != null
            if (captured) report(Log.DEBUG, "refreshed pristine snapshot task=$taskId")
            captured
        }.onFailure {
            report(Log.WARN, "snapshot refresh skipped task=$taskId", it)
        }.getOrDefault(false)
    }

    override fun close() {
        if (closed) return
        closed = true
        hooks.asReversed().forEach { handle -> runCatching { handle.unhook() } }
        hooks.clear()
        states.values.forEach { state -> state.lock.withLock { state.blocked = false } }
    }

    private fun report(priority: Int, message: String, error: Throwable? = null) {
        if (error == null) module.log(priority, TAG, message)
        else module.log(priority, TAG, message, error)
    }

    private class Members {
        private data class MethodKey(val type: Class<*>, val name: String, val parameters: List<Class<*>>)
        private data class FieldKey(val type: Class<*>, val name: String)
        private val methods = ConcurrentHashMap<MethodKey, Method>()
        private val fields = ConcurrentHashMap<FieldKey, Field>()
        fun value(target: Any, name: String): Any? = fields.getOrPut(FieldKey(target.javaClass, name)) {
            field(target.javaClass, name)
        }.get(target)
        fun invoke(target: Any, name: String): Any? = method(target, name).invoke(target)
        fun method(target: Any, name: String, vararg parameters: Class<*>): Method =
            methods.getOrPut(MethodKey(target.javaClass, name, parameters.toList())) {
                find(target.javaClass) { it.getDeclaredMethod(name, *parameters).apply { isAccessible = true } }
            }
    }

    private companion object {
        const val TAG = "HHA.Snapshot"
        fun field(type: Class<*>, name: String): Field =
            find(type) { it.getDeclaredField(name).apply { isAccessible = true } }
        fun <T> find(type: Class<*>, lookup: (Class<*>) -> T): T {
            var current: Class<*>? = type
            while (current != null) {
                try { return lookup(current) }
                catch (_: NoSuchFieldException) { current = current.superclass }
                catch (_: NoSuchMethodException) { current = current.superclass }
            }
            throw NoSuchMethodException("${type.name}: required snapshot member is unavailable")
        }
    }
}
