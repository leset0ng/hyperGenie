package ng.lst.hha

import java.lang.reflect.Field
import java.lang.reflect.Method
import java.util.concurrent.ConcurrentHashMap

/** Per-component cache of optional fields and no-argument methods, including absent members. */
internal class OptionalMembers {
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
