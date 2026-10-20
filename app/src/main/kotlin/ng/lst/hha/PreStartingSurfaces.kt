package ng.lst.hha

import android.annotation.SuppressLint
import android.graphics.Rect
import android.util.Log
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedInterface.HookHandle
import io.github.libxposed.api.XposedModule
import java.lang.reflect.Field
import java.lang.reflect.Method
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.locks.ReentrantLock

/** Copies only OEM previews whose local domain matches the selected fullscreen window. */
@SuppressLint("PrivateApi", "BlockedPrivateApi", "SoonBlockedPrivateApi")
internal class PreStartingSurfaces(private val module: XposedModule, private val loader: ClassLoader) : AutoCloseable {
    /** The caller must retry without retiring its currently attached content. */
    class Busy : RuntimeException()

    private val members = Members()
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
    private var manager: Any? = null
    private var controller: Any? = null
    private var drawHook: HookHandle? = null
    private val drawDomains = LinkedHashMap<Int, Rect>()

    fun install() {
        // SC builder dimensions alone do not prove the opening snapshot's
        // domain: this OEM draw can subsequently rotate and scale its buffer.
        runCatching {
            val type = Class.forName("com.android.server.wm.PreStartupSnapshotHandler", false, loader)
            val draw = type.declaredMethods.single {
                it.name == "drawSnapshotOnSurface" && it.parameterCount == 4
            }
            drawHook = module.hook(draw)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("prestarting-snapshot-domain")
                .intercept { chain ->
                    val surface = chain.getArg(0)
                    val snapshot = chain.getArg(1)
                    val display = chain.getArg(2)
                    val bounds = (chain.getArg(3) as? Rect)?.let(::Rect)
                    val aligned = runCatching {
                        surface != null && snapshot != null && display != null && bounds != null &&
                            bounds.left == 0 && bounds.top == 0 && bounds.width() < bounds.height() &&
                            members.call(display, "getRotation") == 0 &&
                            members.call(snapshot, "getRotation") == 0 &&
                            members.call(snapshot, "isBufferValid") == true &&
                            members.call(snapshot, "getHardwareBufferWidth") == bounds.width() &&
                            members.call(snapshot, "getHardwareBufferHeight") == bounds.height()
                    }.getOrDefault(false)
                    val layer = surface?.let { runCatching { layer(it) }.getOrNull() }
                    if (layer != null) synchronized(drawDomains) { drawDomains.remove(layer) }
                    val result = chain.proceed()
                    if (aligned && surface != null && bounds != null && layer != null) {
                        synchronized(surfaceLock.get(surface)) {
                            if (surfaceValid.invoke(surface) == true) synchronized(drawDomains) {
                                drawDomains[layer] = bounds
                                while (drawDomains.size > MAX_DRAW_DOMAINS) {
                                    drawDomains.remove(drawDomains.keys.first())
                                }
                            }
                        }
                    }
                    result
                }
        }.onFailure {
            module.log(Log.WARN, TAG, "opening snapshot domain observer unavailable; excluding unverified previews", it)
        }
    }

    /** Resolve OEM singleton locks before the caller enters mGlobalLock. */
    fun prepare() {
        manager = managerFactory?.invoke(null)
        controller = controllerFactory?.invoke(null)
    }

    /** Caller holds WMS.mGlobalLock. Returned surfaces are exclusively owned copies. */
    fun copy(activity: Any, task: Any, frame: Rect?, width: Int, height: Int,
             leash: Int, excludedLayers: Set<Int>): List<Any> {
        val display = identityDisplay(activity, task, frame, width, height) ?: return emptyList()
        val owned = ArrayList<Any>(3)
        val seen = excludedLayers.toMutableSet()
        try {
            val startingData = members.field(activity, "mStartingData")
            val currentManager = manager
            if (startingData != null && currentManager != null) {
                val lock = members.field(currentManager, "mSurfaceLock") as? ReentrantLock
                    ?: return emptyList()
                if (!lock.tryLock()) throw Busy()
                try {
                    val snapshots = members.field(currentManager, "mPreSurfaceControlMap") as? Map<*, *>
                    val colors = members.field(currentManager, "mColorSurfaceControlMap") as? Map<*, *>
                    val positions = members.field(currentManager, "mPreSurfaceInitPosMap") as? Map<*, *>
                    val scales = members.field(currentManager, "mPreSurfaceScaleMap") as? Map<*, *>
                    if (positions != null && scales != null &&
                        identityPair(positions[startingData], 0f) && identityPair(scales[startingData], 1f)) {
                        val snapshot = members.call(startingData, "getSnapshot")
                        if (snapshot != null && members.call(snapshot, "getRotation") == 0 &&
                            members.call(snapshot, "getHardwareBufferWidth") == width &&
                            members.call(snapshot, "getHardwareBufferHeight") == height) {
                            addCopy(snapshots?.get(startingData), width, height, false, seen, owned)
                        }
                    }
                    // The non-letterboxed color branch is created at (0,0),
                    // scale 1, with its domain inherited from the trusted parent.
                    addCopy(colors?.get(startingData), width, height, true, seen, owned)
                } finally {
                    lock.unlock()
                }
            }
            val animations = controller?.let { members.call(it, "getAnimations") } as? Map<*, *>
            val matches = animations?.values?.filterNotNull()?.filter { animation ->
                val opening = members.field(animation, "mOpeningLeash")
                opening != null && runCatching { layer(opening) == leash }.getOrDefault(false)
            }.orEmpty()
            val animation = matches.singleOrNull()
            if (animation != null && members.field(animation, "mDisplayContent") === display &&
                members.call(animation, "isParallel") == false) {
                val info = members.field(animation, "mTransitionInfo")
                val changes = info?.let { members.call(it, "getChanges") } as? List<*>
                val opening = changes?.filterNotNull()?.singleOrNull { change ->
                    val control = members.call(change, "getLeash")
                    control != null && runCatching { layer(control) == leash }.getOrDefault(false)
                }
                if (opening != null && members.call(opening, "getStartRotation") == 0 &&
                    members.call(opening, "getEndRotation") == 0 &&
                    members.call(opening, "getStartAbsBounds") == frame &&
                    members.call(opening, "getEndAbsBounds") == frame) {
                    val surface = members.call(animation, "getPreStartingSurface")
                    val type = members.call(animation, "getPreStartingType")
                    val observed = surface?.let { source ->
                        runCatching {
                            val sourceLayer = layer(source)
                            synchronized(drawDomains) { drawDomains[sourceLayer]?.let(::Rect) }
                        }.getOrNull()
                    }
                    if (type == 1 || (type == 2 && observed == frame)) {
                        addCopy(surface, width, height, false, seen, owned)
                    }
                }
            }
            return owned
        } catch (error: Throwable) {
            owned.forEach(::release)
            throw error
        }
    }

    private fun identityDisplay(activity: Any, task: Any, frame: Rect?, width: Int, height: Int): Any? {
        if (width <= 0 || height <= width || frame != Rect(0, 0, width, height) ||
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
                        seen: MutableSet<Int>, owned: MutableList<Any>) {
        if (source == null) return
        synchronized(surfaceLock.get(source)) {
            if (surfaceValid.invoke(source) != true) return
            val layer = surfaceLayer.invoke(source) as Int
            if (layer < 0 || layer in seen) return
            val actualWidth = surfaceWidth.invoke(source) as Int
            val actualHeight = surfaceHeight.invoke(source) as Int
            if ((actualWidth != width || actualHeight != height) &&
                !(color && actualWidth <= 0 && actualHeight <= 0)) return
            val copy = copySurface.newInstance(source, "HHA.central.prestarting")
            try {
                owned += copy
            } catch (error: Throwable) {
                release(copy)
                throw error
            }
            seen += layer
        }
    }

    private fun layer(surface: Any): Int = synchronized(surfaceLock.get(surface)) {
        if (surfaceValid.invoke(surface) == true) surfaceLayer.invoke(surface) as Int else -1
    }

    private fun release(surface: Any) { runCatching { surfaceRelease.invoke(surface) } }

    override fun close() {
        runCatching { drawHook?.unhook() }
        drawHook = null
        synchronized(drawDomains) { drawDomains.clear() }
    }

    private fun factory(name: String): Method? = runCatching {
        Class.forName(name, false, loader).getDeclaredMethod("getInstance").apply { isAccessible = true }
    }.getOrNull()

    private class Members {
        private data class Key(val type: Class<*>, val name: String)
        private data class Optional<T>(val value: T?)
        private val fields = ConcurrentHashMap<Key, Optional<Field>>()
        private val methods = ConcurrentHashMap<Key, Optional<Method>>()
        fun field(target: Any, name: String): Any? = fields.getOrPut(Key(target.javaClass, name)) {
            Optional(find(target.javaClass) { it.getDeclaredField(name).apply { isAccessible = true } })
        }.value?.get(target)
        fun call(target: Any, name: String): Any? = methods.getOrPut(Key(target.javaClass, name)) {
            Optional(find(target.javaClass) { it.getDeclaredMethod(name).apply { isAccessible = true } })
        }.value?.invoke(target)
        private fun <T> find(type: Class<*>, lookup: (Class<*>) -> T): T? {
            var current: Class<*>? = type
            while (current != null) {
                try { return lookup(current) }
                catch (_: NoSuchFieldException) { current = current.superclass }
                catch (_: NoSuchMethodException) { current = current.superclass }
            }
            return null
        }
    }

    private companion object {
        const val TAG = "HHA.PreStarting"
        const val MAX_DRAW_DOMAINS = 64
    }
}
