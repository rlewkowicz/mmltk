private const val UNIFFI_HANDLEMAP_INITIAL = 1.toLong()
private const val UNIFFI_HANDLEMAP_DELTA = 2.toLong()

internal class UniffiHandleMap<T: Any> {
    private val map = ConcurrentHashMap<Long, T>()
    private val counter = java.util.concurrent.atomic.AtomicLong(UNIFFI_HANDLEMAP_INITIAL)

    val size: Int
        get() = map.size

    fun insert(obj: T): Long {
        val handle = counter.getAndAdd(UNIFFI_HANDLEMAP_DELTA)
        map.put(handle, obj)
        return handle
    }

    fun clone(handle: Long): Long {
        val obj = map.get(handle) ?: throw InternalException("UniffiHandleMap.clone: Invalid handle")
        return insert(obj)
    }

    fun get(handle: Long): T {
        return map.get(handle) ?: throw InternalException("UniffiHandleMap.get: Invalid handle")
    }

    fun remove(handle: Long): T {
        return map.remove(handle) ?: throw InternalException("UniffiHandleMap: Invalid handle")
    }
}
