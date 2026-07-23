/**
 * The FfiConverter interface handles converter types to and from the FFI
 *
 * All implementing objects should be public to support external types.  When a
 * type is external we need to import it's FfiConverter.
 *
 * @suppress
 */
public interface FfiConverter<KotlinType, FfiType> {
    fun lift(value: FfiType): KotlinType

    fun lower(value: KotlinType): FfiType

    fun read(buf: ByteBuffer): KotlinType

    fun allocationSize(value: KotlinType): ULong

    fun write(value: KotlinType, buf: ByteBuffer)

    fun lowerIntoRustBuffer(value: KotlinType): RustBuffer.ByValue {
        val rbuf = RustBuffer.alloc(allocationSize(value))
        try {
            val bbuf = rbuf.data!!.getByteBuffer(0, rbuf.capacity).also {
                it.order(ByteOrder.BIG_ENDIAN)
            }
            write(value, bbuf)
            rbuf.writeField("len", bbuf.position().toLong())
            return rbuf
        } catch (e: Throwable) {
            RustBuffer.free(rbuf)
            throw e
        }
    }

    fun liftFromRustBuffer(rbuf: RustBuffer.ByValue): KotlinType {
        val byteBuf = rbuf.asByteBuffer()!!
        try {
           val item = read(byteBuf)
           if (byteBuf.hasRemaining()) {
               throw RuntimeException("junk remaining in buffer after lifting, something is very wrong!!")
           }
           return item
        } finally {
            RustBuffer.free(rbuf)
        }
    }
}

/**
 * FfiConverter that uses `RustBuffer` as the FfiType
 *
 * @suppress
 */
public interface FfiConverterRustBuffer<KotlinType>: FfiConverter<KotlinType, RustBuffer.ByValue> {
    override fun lift(value: RustBuffer.ByValue) = liftFromRustBuffer(value)
    override fun lower(value: KotlinType) = lowerIntoRustBuffer(value)
}
