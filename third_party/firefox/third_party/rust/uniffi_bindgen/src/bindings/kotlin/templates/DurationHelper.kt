/**
 * @suppress
 */
public object FfiConverterDuration: FfiConverterRustBuffer<java.time.Duration> {
    override fun read(buf: ByteBuffer): java.time.Duration {
        val seconds = buf.getLong()
        val nanoseconds = buf.getInt().toLong()
        if (seconds < 0) {
            throw java.time.DateTimeException("Duration exceeds minimum or maximum value supported by uniffi")
        }
        if (nanoseconds < 0) {
            throw java.time.DateTimeException("Duration nanoseconds exceed minimum or maximum supported by uniffi")
        }
        return java.time.Duration.ofSeconds(seconds, nanoseconds)
    }

    override fun allocationSize(value: java.time.Duration) = 12UL

    override fun write(value: java.time.Duration, buf: ByteBuffer) {
        if (value.seconds < 0) {
            throw IllegalArgumentException("Invalid duration, must be non-negative")
        }

        if (value.nano < 0) {
            throw IllegalArgumentException("Invalid duration, nano value must be non-negative")
        }

        buf.putLong(value.seconds)
        buf.putInt(value.nano)
    }
}
