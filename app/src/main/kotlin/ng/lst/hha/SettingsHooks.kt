package ng.lst.hha

import android.animation.ValueAnimator
import android.annotation.SuppressLint
import android.app.Activity
import android.app.Instrumentation
import android.content.res.Configuration
import android.graphics.Point
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.View
import android.view.ViewTreeObserver
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedInterface.HookHandle
import io.github.libxposed.api.XposedModule
import java.io.File
import java.lang.ref.WeakReference
import java.lang.reflect.Field
import java.lang.reflect.Method

/** First integration target: the portrait, full-screen Settings root activity. */
// These verified ROM members are accessed inside LSPosed's hidden-API exemption.
@SuppressLint("PrivateApi", "BlockedPrivateApi", "SoonBlockedPrivateApi")
internal class SettingsHooks(private val module: XposedModule) {
    private val handler = Handler(Looper.getMainLooper())
    private val handles = mutableListOf<HookHandle>()
    private var session: Session? = null
    private var resumeLogs = 0
    @Volatile private var closed = false

    private val viewRootClass = Class.forName("android.view.ViewRootImpl")
    private val surfaceClass = Class.forName("android.view.SurfaceControl")
    private val getViewRoot = View::class.java.getDeclaredMethod("getViewRootImpl").accessible()
    private val getSurfaceControl = viewRootClass.getDeclaredMethod("getSurfaceControl").accessible()
    // This exact Point is passed to BLASTBufferQueue.update by the verified ROM.
    private val surfaceSize = viewRootClass.getDeclaredField("mSurfaceSize").accessible()
    private val surfaceLock = surfaceClass.getDeclaredField("mLock").accessible()
    private val nativeObject = surfaceClass.getDeclaredField("mNativeObject").accessible()

    fun install() {
        // The native ABI has only been verified on the Android 17 arm64 ROM.
        if (Build.VERSION.SDK_INT != 37 || !Build.SUPPORTED_ABIS.contains("arm64-v8a")) {
            report(Log.WARN, "unsupported SDK/ABI; keeping stock Settings animation")
            return
        }
        try {
            hook(Instrumentation::class.java.getDeclaredMethod("callActivityOnResume", Activity::class.java),
                "content-activity-resume") { chain ->
                val result = chain.proceed()
                val activity = chain.getArg(0) as? Activity
                if (activity != null) protect { onResume(activity) }
                result
            }
            for (method in listOf("callActivityOnPause", "callActivityOnDestroy")) {
                hook(Instrumentation::class.java.getDeclaredMethod(method, Activity::class.java),
                    "content-$method") { chain ->
                    val activity = chain.getArg(0) as? Activity
                    if (activity != null) protect { stopFor(activity) }
                    chain.proceed()
                }
            }
            hook(viewRootClass.getDeclaredMethod("destroySurface"), "content-surface-destroy") { chain ->
                protect {
                    if (session?.root === chain.thisObject) stopSession()
                }
                chain.proceed()
            }
            hook(viewRootClass.getDeclaredMethod("updateBlastSurfaceIfNeeded"), "content-surface-update") { chain ->
                val result = chain.proceed()
                protect { onSurfaceUpdated(chain.thisObject) }
                result
            }
            report(Log.INFO, "Settings lifecycle hooks installed; content_enabled defaults to 0")
        } catch (error: Throwable) {
            close()
            throw error
        }
    }

    /** Stop before old-generation hooks are dropped during a module reload. */
    fun close() {
        closed = true
        // Stop promptly even when the framework reload callback is not on main.
        ContentAnimator.stop()
        if (Looper.myLooper() == Looper.getMainLooper()) {
            stopSession()
        } else {
            handler.post { stopSession() }
        }
        handles.forEach { handle -> runCatching { handle.unhook() } }
        handles.clear()
    }

    private fun hook(method: Method, id: String, hooker: XposedInterface.Hooker) {
        handles += module.hook(method)
            .setPriority(XposedInterface.PRIORITY_DEFAULT)
            .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
            .setId(id)
            .intercept(hooker)
    }

    private fun onResume(activity: Activity) {
        if (resumeLogs++ < 8) {
            report(Log.INFO, "resume activity=${activity.javaClass.name} config=${readConfig().enabled} supported=${supportedWindow(activity)}")
        }
        if (closed || activity.packageName != PACKAGE || activity.javaClass.name !in ROOT_ACTIVITIES) return
        stopSession()
        val config = readConfig()
        if (!config.enabled || !supportedWindow(activity)) return
        if (!ContentAnimator.initialize()) {
            report(Log.WARN, "native initialization rejected; keeping stock Settings animation")
            return
        }
        val decor = activity.window?.decorView ?: return
        val next = Session(WeakReference(activity), decor, config, SystemClock.uptimeMillis())
        session = next
        next.preDraw = ViewTreeObserver.OnPreDrawListener {
            if (session === next && !closed) protect { tryStart(next) }
            true
        }
        next.attachListener = object : View.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(view: View) = Unit
            override fun onViewDetachedFromWindow(view: View) {
                if (session === next) protect { stopSession() }
            }
        }
        decor.viewTreeObserver.addOnPreDrawListener(next.preDraw)
        decor.addOnAttachStateChangeListener(next.attachListener)
        next.check = Runnable { protect { checkSession(next) } }
        handler.postDelayed(next.check, CHECK_INTERVAL_MS)
        decor.invalidate()
    }

    private fun tryStart(current: Session) {
        val activity = current.activity.get()
        if (activity == null || !supportedWindow(activity) || !readConfig().enabled) {
            stopSession()
            return
        }
        current.attempts++
        if (current.attempts > MAX_PREDRAW_ATTEMPTS ||
            SystemClock.uptimeMillis() - current.createdAt > SURFACE_WAIT_MS) {
            report(Log.WARN, "no usable VRI before first-draw deadline; keeping stock animation")
            stopSession()
            return
        }
        val root = getViewRoot.invoke(current.decor) ?: return
        current.root = root
        val sc = getSurfaceControl.invoke(root) ?: return
        val size = surfaceSize.get(root) as? Point ?: return
        if (size.x <= 0 || size.y <= size.x) return
        val width = size.x
        val height = size.y
        val lock = surfaceLock.get(sc) ?: return
        val started = synchronized(lock) {
            val pointer = nativeObject.getLong(sc)
            if (pointer == 0L) return@synchronized false
            ContentAnimator.start(pointer, width, height,
                current.config.genieAmount, current.config.iconScale).also {
                if (it) current.surfacePointer = pointer
            }
        }
        if (!started) {
            report(Log.WARN, "VRI start rejected; keeping stock animation")
            stopSession()
            return
        }
        current.width = width
        current.height = height
        current.started = true
        removePreDraw(current)
        report(Log.INFO, "started Settings VRI ${width}x$height")
    }

    private fun onSurfaceUpdated(root: Any?) {
        val current = session ?: return
        if (!current.started || current.root !== root) return
        val sc = getSurfaceControl.invoke(root)
        val size = surfaceSize.get(root) as? Point
        if (sc == null || size == null || size.x != current.width || size.y != current.height) {
            stopSession()
            return
        }
        val lock = surfaceLock.get(sc) ?: run { stopSession(); return }
        val pointer = synchronized(lock) { nativeObject.getLong(sc) }
        if (pointer != current.surfacePointer) stopSession()
    }

    private fun checkSession(current: Session) {
        if (session !== current || closed) return
        val age = SystemClock.uptimeMillis() - current.createdAt
        val activity = current.activity.get()
        if (activity == null || !supportedWindow(activity) || !readConfig().enabled ||
            age > (if (current.started) SESSION_LIMIT_MS else SURFACE_WAIT_MS)) {
            stopSession()
            return
        }
        // Bounded watchdog: catches disabled animations/config changes even if
        // the App stops drawing; it does not drive native animation frames.
        handler.postDelayed(current.check, CHECK_INTERVAL_MS)
    }

    private fun stopFor(activity: Activity) {
        if (session?.activity?.get() === activity) stopSession()
    }

    private fun stopSession() {
        val current = session ?: return
        session = null
        handler.removeCallbacks(current.check)
        removePreDraw(current)
        current.decor.removeOnAttachStateChangeListener(current.attachListener)
        ContentAnimator.stop()
        report(Log.INFO, "Settings VRI session stopped")
    }

    private fun removePreDraw(current: Session) {
        val observer = current.decor.viewTreeObserver
        if (observer.isAlive) observer.removeOnPreDrawListener(current.preDraw)
    }

    private fun supportedWindow(activity: Activity): Boolean =
        !activity.isFinishing && !activity.isDestroyed &&
            activity.resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT &&
            !activity.isInMultiWindowMode && !activity.isInPictureInPictureMode &&
            ValueAnimator.areAnimatorsEnabled()

    private fun readConfig(): Config = runCatching {
        var master = false
        var content = false
        var amount = -0.8f
        var scale = 0.4f
        val file = listOf(File(CONFIG_PATH), File(CONFIG_FALLBACK))
            .firstOrNull { it.isFile && it.canRead() } ?: return@runCatching Config()
        file.useLines { lines ->
            lines.forEach { line ->
                val parts = line.substringBefore('#').split('=', limit = 2)
                if (parts.size != 2) return@forEach
                val value = parts[1].trim()
                when (parts[0].trim()) {
                    "enabled" -> master = parseBoolean(value, master)
                    "content_enabled" -> content = parseBoolean(value, content)
                    "content_genie_amount" -> value.toFloatOrNull()?.takeIf { it.isFinite() }?.let {
                        amount = it.coerceIn(-1f, 1f)
                    }
                    "content_icon_scale" -> value.toFloatOrNull()?.takeIf { it.isFinite() }?.let {
                        scale = it.coerceIn(0.05f, 0.95f)
                    }
                }
            }
        }
        Config(master && content, amount, scale)
    }.getOrDefault(Config())

    private fun parseBoolean(value: String, fallback: Boolean): Boolean = when (value) {
        "1", "true" -> true
        "0", "false" -> false
        else -> fallback
    }

    private inline fun protect(block: () -> Unit) {
        try {
            block()
        } catch (error: Throwable) {
            report(Log.ERROR, "lifecycle hook failed; restoring stock animation", error)
            stopSession()
        }
    }

    private fun report(priority: Int, message: String, error: Throwable? = null) {
        if (error == null) module.log(priority, TAG, "content: $message")
        else module.log(priority, TAG, "content: $message", error)
    }

    private data class Config(
        val enabled: Boolean = false,
        val genieAmount: Float = -0.8f,
        val iconScale: Float = 0.4f,
    )

    private class Session(
        val activity: WeakReference<Activity>,
        val decor: View,
        val config: Config,
        val createdAt: Long,
    ) {
        lateinit var preDraw: ViewTreeObserver.OnPreDrawListener
        lateinit var attachListener: View.OnAttachStateChangeListener
        lateinit var check: Runnable
        var root: Any? = null
        var surfacePointer = 0L
        var width = 0
        var height = 0
        var attempts = 0
        var started = false
    }

    private fun Method.accessible() = apply { isAccessible = true }
    private fun Field.accessible() = apply { isAccessible = true }

    private companion object {
        const val TAG = "HHA"
        const val PACKAGE = "com.android.settings"
        const val CONFIG_PATH = "/data/local/tmp/hha.conf"
        const val CONFIG_FALLBACK = "/data/user/0/com.android.settings/files/hha-content.conf"
        const val CHECK_INTERVAL_MS = 250L
        const val SURFACE_WAIT_MS = 1000L
        const val SESSION_LIMIT_MS = 2500L
        const val MAX_PREDRAW_ATTEMPTS = 60
        val ROOT_ACTIVITIES = setOf(
            "com.android.settings.MiuiSettings",
            "com.android.settings.MainSettings",
            "com.android.settings.Settings",
        )
    }
}
