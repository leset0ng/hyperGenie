package ng.lst.hha

import android.util.Log

/** JNI boundary for the App-owned VRI animation. Never initialized in Zygote. */
internal object ContentAnimator {
    private var loaded = false
    private var initialized = false

    @Synchronized
    fun initialize(): Boolean {
        if (initialized) return true
        return runCatching {
            if (!loaded) {
                System.loadLibrary("hha_content")
                loaded = true
            }
            nativeInitialize().also { initialized = it }
        }.onFailure {
            Log.e(TAG, "content: native initialization failed; keeping stock animation", it)
        }.getOrDefault(false)
    }

    /** Called while SurfaceControl.mLock is held; nativeStart retains its own ref. */
    @Synchronized
    fun start(surface: Long, width: Int, height: Int, genieAmount: Float, iconScale: Float): Boolean {
        if (!initialized) return false
        return runCatching {
            nativeStart(surface, width, height, genieAmount, iconScale)
        }.onFailure {
            Log.e(TAG, "content: start failed", it)
        }.getOrDefault(false)
    }

    @Synchronized
    fun stop() {
        if (!initialized) return
        runCatching { nativeStop() }.onFailure {
            Log.e(TAG, "content: stop failed", it)
        }
    }

    @JvmStatic external fun nativeInitialize(): Boolean
    @JvmStatic external fun nativeStart(
        surface: Long,
        width: Int,
        height: Int,
        genieAmount: Float,
        iconScale: Float,
    ): Boolean
    @JvmStatic external fun nativeStop()

    private const val TAG = "HHA"
}
