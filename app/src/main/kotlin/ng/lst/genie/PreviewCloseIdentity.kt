package ng.lst.genie

/** A live WindowState capability, never a permission to discover another starting window. */
internal class PreviewCloseIdentity(
    val window: Any, val activity: Any, val token: Any, val task: Any,
    val producer: Any, val producerLayer: Int, val bufferLayer: Int,
    val launchToken: Long, val openingNs: Long, val taskId: Int, val uid: Int,
    val width: Int, val height: Int,
) {
    fun matches(window: Any, activity: Any, token: Any, task: Any, producer: Any?,
                producerLayer: Int, launchToken: Long?, taskId: Int, uid: Int,
                width: Int, height: Int): Boolean =
        this.window === window && this.activity === activity && this.token === token &&
            this.task === task && this.producer === producer && this.producerLayer == producerLayer &&
            this.launchToken > 0L && this.launchToken == launchToken && this.taskId == taskId &&
            this.uid == uid && this.width == width && this.height == height

    fun allowsClose(taskId: Int, eventNs: Long, width: Float, height: Float): Boolean =
        this.taskId == taskId && eventNs > openingNs &&
            width.isFinite() && height.isFinite() && width > 0f && height > 0f &&
            width <= this.width + 2f && height <= this.height + 2f &&
            kotlin.math.abs(height - width * this.height / this.width) < 2f

    /** New content can be guarded, but cannot inherit the preview's closing writer. */
    fun sameLaunch(activity: Any?, task: Any?, launchToken: Long?, uid: Int?): Boolean =
        this.activity === activity && this.task === task && this.launchToken == launchToken && this.uid == uid
}
