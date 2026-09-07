
{{ self.add_import("java.util.concurrent.atomic.AtomicBoolean") }}

{%- let obj = ci.get_object_definition(name).unwrap() %}
{%- let interface_name = self::object_interface_name(ci, obj) %}
{%- let impl_class_name = self::object_impl_name(ci, obj) %}
{%- let methods = obj.methods() %}
{%- let uniffi_trait_methods = obj.uniffi_trait_methods() %}
{%- let interface_docstring = obj.docstring() %}
{%- let is_error = ci.is_name_used_as_error(name) %}
{%- let ffi_converter_name = obj|ffi_converter_name %}

{%- include "Interface.kt" %}

{%- call kt::docstring(obj, 0) %}
{% if (is_error) %}
open class {{ impl_class_name }} : kotlin.Exception, Disposable, AutoCloseable, {{ interface_name }} {
{% else -%}
open class {{ impl_class_name }}: Disposable, AutoCloseable, {{ interface_name }}
{%- for t in obj.trait_impls() %}
, {{ self::trait_interface_name(ci, t.trait_ty)? }}
{% endfor %}
{%- if uniffi_trait_methods.ord_cmp.is_some() %}
, Comparable<{{ impl_class_name }}>
{%- endif %}
{
{%- endif %}

    @Suppress("UNUSED_PARAMETER")
    /**
     * @suppress
     */
    constructor(withHandle: UniffiWithHandle, handle: Long) {
        this.handle = handle
        this.cleanable = UniffiLib.CLEANER.register(this, UniffiCleanAction(handle))
    }

    /**
     * @suppress
     *
     * This constructor can be used to instantiate a fake object. Only used for tests. Any
     * attempt to actually use an object constructed this way will fail as there is no
     * connected Rust object.
     */
    @Suppress("UNUSED_PARAMETER")
    constructor(noHandle: NoHandle) {
        this.handle = 0
        this.cleanable = null
    }

    {%- match obj.primary_constructor() %}
    {%- when Some(cons) %}
    {%-     if cons.is_async() %}
    {%-     else %}
    {%- call kt::docstring(cons, 4) %}
    constructor({% call kt::arg_list(cons, true) -%}) :
        this(UniffiWithHandle, {% call kt::to_ffi_call(cons) %})
    {%-     endif %}
    {%- when None %}
    {%- endmatch %}

    protected val handle: Long
    protected val cleanable: UniffiCleaner.Cleanable?

    private val wasDestroyed = AtomicBoolean(false)
    private val callCounter = AtomicLong(1)

    override fun destroy() {
        if (this.wasDestroyed.compareAndSet(false, true)) {
            if (this.callCounter.decrementAndGet() == 0L) {
                cleanable?.clean()
            }
        }
    }

    @Synchronized
    override fun close() {
        this.destroy()
    }

    internal inline fun <R> callWithHandle(block: (handle: Long) -> R): R {
        do {
            val c = this.callCounter.get()
            if (c == 0L) {
                throw IllegalStateException("${this.javaClass.simpleName} object has already been destroyed")
            }
            if (c == Long.MAX_VALUE) {
                throw IllegalStateException("${this.javaClass.simpleName} call counter would overflow")
            }
        } while (! this.callCounter.compareAndSet(c, c + 1L))
        try {
            return block(this.uniffiCloneHandle())
        } finally {
            if (this.callCounter.decrementAndGet() == 0L) {
                cleanable?.clean()
            }
        }
    }

    private class UniffiCleanAction(private val handle: Long) : Runnable {
        override fun run() {
            if (handle == 0.toLong()) {
                return;
            }
            uniffiRustCall { status ->
                UniffiLib.{{ obj.ffi_object_free().name() }}(handle, status)
            }
        }
    }

    /**
     * @suppress
     */
    fun uniffiCloneHandle(): Long {
        if (handle == 0.toLong()) {
            throw InternalException("uniffiCloneHandle() called on NoHandle object");
        }
        return uniffiRustCall() { status ->
            UniffiLib.{{ obj.ffi_object_clone().name() }}(handle, status)
        }
    }

    {% for meth in methods -%}
    {%- call kt::func_decl("override", meth, 4) %}
    {% endfor %}

    {% call kt::uniffi_trait_impls(uniffi_trait_methods) %}

    {# XXX - "companion object" confusion? How to have alternate constructors *and* be an error? #}
    {% if !obj.alternate_constructors().is_empty() -%}
    companion object {
        {% for cons in obj.alternate_constructors() -%}
        {% call kt::func_decl("", cons, 4) %}
        {% endfor %}
    }
    {% else if is_error %}
    companion object ErrorHandler : UniffiRustCallStatusErrorHandler<{{ impl_class_name }}> {
        override fun lift(error_buf: RustBuffer.ByValue): {{ impl_class_name }} {
            val bb = error_buf.asByteBuffer()
            if (bb == null) {
                throw InternalException("?")
            }
            return {{ ffi_converter_name }}.read(bb)
        }
    }
    {% else %}
    /**
     * @suppress
     */
    companion object
    {% endif %}
}

{%- if !obj.has_callback_interface() %}
{# Simple case: the interface can only be implemented in Rust #}

/**
 * @suppress
 */
public object {{ ffi_converter_name }}: FfiConverter<{{ type_name }}, Long> {
    override fun lower(value: {{ type_name }}): Long {
        return value.uniffiCloneHandle()
    }

    override fun lift(value: Long): {{ type_name }} {
        return {{ impl_class_name }}(UniffiWithHandle, value)
    }

    override fun read(buf: ByteBuffer): {{ type_name }} {
        return lift(buf.getLong())
    }

    override fun allocationSize(value: {{ type_name }}) = 8UL

    override fun write(value: {{ type_name }}, buf: ByteBuffer) {
        buf.putLong(lower(value))
    }
}
{%- else %}
{# 
 # The interface can be implemented in Rust or Kotlin
 # * Generate a callback interface implementation to handle the Kotlin side
 # * In the FfiConverter, check which side a handle came from to know how to handle correctly.
#}
{%- let vtable = obj.vtable().expect("trait interface should have a vtable") %}
{%- let vtable_methods = obj.vtable_methods() %}
{%- let ffi_init_callback = obj.ffi_init_callback() %}
{% include "CallbackInterfaceImpl.kt" %}

/**
 * @suppress
 */
public object {{ ffi_converter_name }}: FfiConverter<{{ type_name }}, Long> {
    internal val handleMap = UniffiHandleMap<{{ type_name }}>()

    override fun lower(value: {{ type_name }}): Long {
        if (value is {{ impl_class_name }}) {
            return value.uniffiCloneHandle()
         } else {
            return handleMap.insert(value)
         }
    }

    override fun lift(value: Long): {{ type_name }} {
        if ((value and 1.toLong()) == 0.toLong()) {
            return {{ impl_class_name }}(UniffiWithHandle, value)
        } else {
            return handleMap.remove(value)
        }
    }

    override fun read(buf: ByteBuffer): {{ type_name }} {
        return lift(buf.getLong())
    }

    override fun allocationSize(value: {{ type_name }}) = 8UL

    override fun write(value: {{ type_name }}, buf: ByteBuffer) {
        buf.putLong(lower(value))
    }
}
{%- endif %}
