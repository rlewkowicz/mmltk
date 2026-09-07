
internal const val UNIFFI_CALL_SUCCESS = 0.toByte()
internal const val UNIFFI_CALL_ERROR = 1.toByte()
internal const val UNIFFI_CALL_UNEXPECTED_ERROR = 2.toByte()

@Structure.FieldOrder("code", "error_buf")
internal open class UniffiRustCallStatus : Structure() {
    @JvmField var code: Byte = 0
    @JvmField var error_buf: RustBuffer.ByValue = RustBuffer.ByValue()

    class ByValue: UniffiRustCallStatus(), Structure.ByValue

    fun isSuccess(): Boolean {
        return code == UNIFFI_CALL_SUCCESS
    }

    fun isError(): Boolean {
        return code == UNIFFI_CALL_ERROR
    }

    fun isPanic(): Boolean {
        return code == UNIFFI_CALL_UNEXPECTED_ERROR
    }

    companion object {
        fun create(code: Byte, errorBuf: RustBuffer.ByValue): UniffiRustCallStatus.ByValue {
            val callStatus = UniffiRustCallStatus.ByValue()
            callStatus.code = code
            callStatus.error_buf = errorBuf
            return callStatus
        }
    }
}

class InternalException(message: String) : kotlin.Exception(message)

/**
 * Each top-level error class has a companion object that can lift the error from the call status's rust buffer
 *
 * @suppress
 */
interface UniffiRustCallStatusErrorHandler<E> {
    fun lift(error_buf: RustBuffer.ByValue): E;
}


private inline fun <U, E: kotlin.Exception> uniffiRustCallWithError(errorHandler: UniffiRustCallStatusErrorHandler<E>, callback: (UniffiRustCallStatus) -> U): U {
    var status = UniffiRustCallStatus()
    val return_value = callback(status)
    uniffiCheckCallStatus(errorHandler, status)
    return return_value
}

private fun<E: kotlin.Exception> uniffiCheckCallStatus(errorHandler: UniffiRustCallStatusErrorHandler<E>, status: UniffiRustCallStatus) {
    if (status.isSuccess()) {
        return
    } else if (status.isError()) {
        throw errorHandler.lift(status.error_buf)
    } else if (status.isPanic()) {
        if (status.error_buf.len > 0) {
            throw InternalException({{ Type::String.borrow()|lift_fn }}(status.error_buf))
        } else {
            throw InternalException("Rust panic")
        }
    } else {
        throw InternalException("Unknown rust call status: $status.code")
    }
}

/**
 * UniffiRustCallStatusErrorHandler implementation for times when we don't expect a CALL_ERROR
 *
 * @suppress
 */
object UniffiNullRustCallStatusErrorHandler: UniffiRustCallStatusErrorHandler<InternalException> {
    override fun lift(error_buf: RustBuffer.ByValue): InternalException {
        RustBuffer.free(error_buf)
        return InternalException("Unexpected CALL_ERROR")
    }
}

private inline fun <U> uniffiRustCall(callback: (UniffiRustCallStatus) -> U): U {
    return uniffiRustCallWithError(UniffiNullRustCallStatusErrorHandler, callback)
}

internal inline fun<T> uniffiTraitInterfaceCall(
    callStatus: UniffiRustCallStatus,
    makeCall: () -> T,
    writeReturn: (T) -> Unit,
) {
    try {
        writeReturn(makeCall())
    } catch(e: kotlin.Exception) {
        val err = try { e.stackTraceToString() } catch(_: Throwable) { "" }
        callStatus.code = UNIFFI_CALL_UNEXPECTED_ERROR
        callStatus.error_buf = {{ Type::String.borrow()|lower_fn }}(err)
    }
}

internal inline fun<T, reified E: Throwable> uniffiTraitInterfaceCallWithError(
    callStatus: UniffiRustCallStatus,
    makeCall: () -> T,
    writeReturn: (T) -> Unit,
    lowerError: (E) -> RustBuffer.ByValue
) {
    try {
        writeReturn(makeCall())
    } catch(e: kotlin.Exception) {
        if (e is E) {
            callStatus.code = UNIFFI_CALL_ERROR
            callStatus.error_buf = lowerError(e)
        } else {
            val err = try { e.stackTraceToString() } catch(_: Throwable) { "" }
            callStatus.code = UNIFFI_CALL_UNEXPECTED_ERROR
            callStatus.error_buf = {{ Type::String.borrow()|lower_fn }}(err)
        }
    }
}
