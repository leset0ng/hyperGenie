package ng.lst.genie

import android.annotation.SuppressLint

/** Resolve local legacy Surface(sc) BBQ buffers before copying away their owner. */
@SuppressLint("PrivateApi", "BlockedPrivateApi", "SoonBlockedPrivateApi")
internal object ContentSurfaces {
    private val type = Class.forName("android.view.SurfaceControl")
    private val empty = type.getConstructor()
    private val copy = type.getConstructor(type, String::class.java)
    private val lock = type.getDeclaredField("mLock").apply { isAccessible = true }
    private val pointer = type.getDeclaredField("mNativeObject").apply { isAccessible = true }
    private val assign = type.getDeclaredMethod("assignNativeObject", Long::class.javaPrimitiveType,
        String::class.java).apply { isAccessible = true }
    private val widthField = type.getDeclaredField("mWidth").apply { isAccessible = true }
    private val heightField = type.getDeclaredField("mHeight").apply { isAccessible = true }
    private val release = type.getMethod("release")

    /** Width/height must already have been proved in the source's local domain. */
    fun copy(source: Any, callsite: String, width: Int, height: Int): Any =
        synchronized(checkNotNull(lock.get(source))) {
            // The native result is owned exactly like nativeCopyFromSurfaceControl.
            // The empty Java object adopts it through the platform allocation registry.
            val result = empty.newInstance()
            val buffer = ContentAnimator.bufferSurfaceCopy(pointer.getLong(source))
            if (buffer == 0L) return@synchronized copy.newInstance(source, callsite)
            try {
                assign.invoke(result, buffer, callsite)
                widthField.setInt(result, width)
                heightField.setInt(result, height)
                result
            } catch (error: Throwable) {
                // assignNativeObject normally cannot fail for a valid owned copy.
                // Release any adopted native allocation before propagating failure.
                runCatching { release.invoke(result) }
                throw error
            }
        }
}
