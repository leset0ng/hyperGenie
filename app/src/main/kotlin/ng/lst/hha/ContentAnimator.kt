package ng.lst.hha

import android.util.Log

/** JNI boundary for system_server-owned content animation. Never initialized in Zygote. */
internal object ContentAnimator {
    private var loaded = false
    @Volatile private var initialized = false

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

    /** Append to a caller-owned show transaction; never apply or close it here. */
    fun prime(transaction: Long, surface: Long, parent: Long, width: Int, height: Int,
              geometry: FloatArray, closing: Boolean, eventTimeNanos: Long,
              companions: LongArray = longArrayOf(), starting: Boolean = false,
              unwarpedSource: Long = 0L): Boolean =
        initialized && nativePrime(transaction, surface, parent, width, height, geometry,
            closing, eventTimeNanos, companions, starting, unwarpedSource)

    fun attach(transaction: Long, surface: Long, parent: Long, width: Int, height: Int,
               eventTimeNanos: Long): Boolean = initialized &&
        nativeAttach(transaction, surface, parent, width, height, eventTimeNanos)

    fun stopRequest(parent: Int, eventTimeNanos: Long) {
        if (initialized && parent >= 0) nativeStopRequest(parent.toLong(), eventTimeNanos)
    }

    fun running(): Boolean = initialized && nativeRunning()

    fun stop() {
        if (!initialized) return
        runCatching { nativeStop() }.onFailure {
            Log.e(TAG, "content: stop failed", it)
        }
    }

    /** Resolve only a buffer child owned by this process's original control. */
    fun bufferSurfaceCopy(surface: Long): Long =
        if (initialized && surface != 0L) nativeBufferSurfaceCopy(surface) else 0L

    @JvmStatic external fun nativeBufferSurfaceCopy(surface: Long): Long
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
    @JvmStatic external fun nativePrime(
        transaction: Long, surface: Long, parent: Long, width: Int, height: Int,
        geometry: FloatArray, closing: Boolean, eventNs: Long,
        companions: LongArray, starting: Boolean, unwarpedSource: Long,
    ): Boolean
    @JvmStatic external fun nativeAttach(
        transaction: Long, surface: Long, parent: Long, width: Int, height: Int, eventNs: Long,
    ): Boolean
    @JvmStatic external fun nativeStopRequest(parent: Long, eventNs: Long)
    @JvmStatic external fun nativeRunning(): Boolean
    @JvmStatic external fun nativeStop()

    private const val TAG = "HHA"
}
