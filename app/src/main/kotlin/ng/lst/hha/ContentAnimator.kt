package ng.lst.hha

import android.util.Log

/** JNI boundary for system_server-owned content animation. Never initialized in Zygote. */
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

    /** All copied SurfaceControl locks stay held until native retains its own refs. */
    @Synchronized
    fun transition(
        surface: Long,
        parent: Long,
        width: Int,
        height: Int,
        geometry: FloatArray,
        closing: Boolean,
        eventTimeNanos: Long,
        companions: LongArray = longArrayOf(),
        starting: Boolean = false,
    ): Boolean {
        if (!initialized || geometry.size != 12 || companions.size > 8 ||
            companions.any { it == 0L } || companions.distinct().size != companions.size) return false
        return runCatching {
            nativeTransition(surface, parent, width, height, geometry, closing, eventTimeNanos,
                companions, starting)
        }.onFailure {
            Log.e(TAG, "content: transition failed", it)
        }.getOrDefault(false)
    }

    @Synchronized
    fun running(): Boolean = initialized && nativeRunning()

    @Synchronized
    fun stop() {
        if (!initialized) return
        runCatching { nativeStop() }.onFailure {
            Log.e(TAG, "content: stop failed", it)
        }
    }

    @JvmStatic external fun nativeInitialize(): Boolean
    @JvmStatic external fun nativeTransition(
        surface: Long,
        parent: Long,
        width: Int,
        height: Int,
        geometry: FloatArray,
        closing: Boolean,
        eventTimeNanos: Long,
        companions: LongArray,
        starting: Boolean,
    ): Boolean
    @JvmStatic external fun nativeRunning(): Boolean
    @JvmStatic external fun nativeStop()

    private const val TAG = "HHA"
}
