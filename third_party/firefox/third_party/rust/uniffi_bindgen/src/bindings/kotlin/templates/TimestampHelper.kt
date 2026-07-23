/**
 * @suppress
 */
public object FfiConverterTimestamp: FfiConverterRustBuffer<java.time.Instant> {
    override fun read(buf: ByteBuffer): java.time.Instant {
        val seconds = buf.getLong()
        val nanoseconds = buf.getInt().toLong()
        if (nanoseconds < 0) {
            throw java.time.DateTimeException("Instant nanoseconds exceed minimum or maximum supported by uniffi")
        }
        if (seconds >= 0) {
            return java.time.Instant.EPOCH.plus(java.time.Duration.ofSeconds(seconds, nanoseconds))
        } else {
            return java.time.Instant.EPOCH.minus(java.time.Duration.ofSeconds(-seconds, nanoseconds))
        }
    }

    override fun allocationSize(value: java.time.Instant) = 12UL

    override fun write(value: java.time.Instant, buf: ByteBuffer) {
        var epochOffset = java.time.Duration.between(java.time.Instant.EPOCH, value)

        var sign = 1
        if (epochOffset.isNegative()) {
            sign = -1
            epochOffset = epochOffset.negated()
        }

        if (epochOffset.nano < 0) {
            throw IllegalArgumentException("Invalid timestamp, nano value must be non-negative")
        }

        buf.putLong(sign * epochOffset.seconds)
        buf.putInt(epochOffset.nano)
    }
}
