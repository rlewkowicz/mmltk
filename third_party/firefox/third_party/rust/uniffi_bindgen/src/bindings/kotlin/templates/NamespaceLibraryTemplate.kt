@Synchronized
private fun findLibraryName(componentName: String): String {
    val libOverride = System.getProperty("uniffi.component.$componentName.libraryOverride")
    if (libOverride != null) {
        return libOverride
    }
    return "{{ config.cdylib_name() }}"
}

{%- for def in ci.ffi_definitions() %}
{%- match def %}
{%- when FfiDefinition::CallbackFunction(callback) %}
internal interface {{ callback.name()|ffi_callback_name }} : com.sun.jna.Callback {
    fun callback(
        {%- for arg in callback.arguments() -%}
        {{ arg.name().borrow()|var_name }}: {{ arg.type_().borrow()|ffi_type_name_by_value(ci) }},
        {%- endfor -%}
        {%- if callback.has_rust_call_status_arg() -%}
        uniffiCallStatus: UniffiRustCallStatus,
        {%- endif -%}
    )
    {%- if let Some(return_type) = callback.return_type() %}
    : {{ return_type|ffi_type_name_by_value(ci) }}
    {%- endif %}
}
{%- when FfiDefinition::Struct(ffi_struct) %}
@Structure.FieldOrder({% for field in ffi_struct.fields() %}"{{ field.name()|var_name_raw }}"{% if !loop.last %}, {% endif %}{% endfor %})
internal open class {{ ffi_struct.name()|ffi_struct_name }}(
    {%- for field in ffi_struct.fields() %}
    @JvmField internal var {{ field.name()|var_name }}: {{ field.type_().borrow()|ffi_type_name_for_ffi_struct(ci) }} = {{ field.type_()|ffi_default_value }},
    {%- endfor %}
) : Structure() {
    class UniffiByValue(
        {%- for field in ffi_struct.fields() %}
        {{ field.name()|var_name }}: {{ field.type_().borrow()|ffi_type_name_for_ffi_struct(ci) }} = {{ field.type_()|ffi_default_value }},
        {%- endfor %}
    ): {{ ffi_struct.name()|ffi_struct_name }}({%- for field in ffi_struct.fields() %}{{ field.name()|var_name }}, {%- endfor %}), Structure.ByValue

   internal fun uniffiSetValue(other: {{ ffi_struct.name()|ffi_struct_name }}) {
        {%- for field in ffi_struct.fields() %}
        {{ field.name()|var_name }} = other.{{ field.name()|var_name }}
        {%- endfor %}
    }

}
{%- when FfiDefinition::Function(_) %}
{#- functions are handled below #}
{%- endmatch %}
{%- endfor %}

{%- macro decl_kotlin_functions(func_list) -%}
{% for func in func_list -%}
external fun {{ func.name() }}(
    {%- call kt::arg_list_ffi_decl(func) %}
): {% match func.return_type() %}{% when Some with (return_type) %}{{ return_type.borrow()|ffi_type_name_by_value(ci) }}{% when None %}Unit{% endmatch %}
{% endfor %}
{%- endmacro %}


internal object IntegrityCheckingUniffiLib {
    init {
        Native.register(IntegrityCheckingUniffiLib::class.java, findLibraryName(componentName = "{{ ci.namespace() }}"))
        uniffiCheckContractApiVersion(this)
{%- if !config.omit_checksums %}
        uniffiCheckApiChecksums(this)
{%- endif %}
    }
    {% filter indent(4) %}
    {%- call decl_kotlin_functions(ci.iter_ffi_function_integrity_checks()) %}
    {% endfilter %}
}

internal object UniffiLib {
    {% if ci.contains_object_types() %}
    internal val CLEANER: UniffiCleaner by lazy {
        UniffiCleaner.create()
    }
    {% endif %}

    init {
        Native.register(UniffiLib::class.java, findLibraryName(componentName = "{{ ci.namespace() }}"))
        {% for fn_item in self.initialization_fns(ci) -%}
        {{ fn_item }}
        {% endfor %}
    }
    {#- XXX - this `filter indent` doesn't seem to work, even though the one above does? #}
    {% filter indent(4) %}
    {%- call decl_kotlin_functions(ci.iter_ffi_function_definitions_excluding_integrity_checks()) %}
    {% endfilter %}
}

private fun uniffiCheckContractApiVersion(lib: IntegrityCheckingUniffiLib) {
    val bindings_contract_version = {{ ci.uniffi_contract_version() }}
    val scaffolding_contract_version = lib.{{ ci.ffi_uniffi_contract_version().name() }}()
    if (bindings_contract_version != scaffolding_contract_version) {
        throw RuntimeException("UniFFI contract version mismatch: try cleaning and rebuilding your project")
    }
}

{%- if !config.omit_checksums %}
@Suppress("UNUSED_PARAMETER")
private fun uniffiCheckApiChecksums(lib: IntegrityCheckingUniffiLib) {
    {%- for (name, expected_checksum) in ci.iter_checksums() %}
    if (lib.{{ name }}() != {{ expected_checksum }}.toShort()) {
        throw RuntimeException("UniFFI API checksum mismatch: try cleaning and rebuilding your project")
    }
    {%- endfor %}
}
{%- endif %}

/**
 * @suppress
 */
public fun uniffiEnsureInitialized() {
    IntegrityCheckingUniffiLib
    UniffiLib
}
