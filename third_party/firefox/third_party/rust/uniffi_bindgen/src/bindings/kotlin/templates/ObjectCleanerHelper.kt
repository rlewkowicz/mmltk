
/**
 * The cleaner interface for Object finalization code to run.
 * This is the entry point to any implementation that we're using.
 *
 * The cleaner registers objects and returns cleanables, so now we are
 * defining a `UniffiCleaner` with a `UniffiClenaer.Cleanable` to abstract the
 * different implmentations available at compile time.
 *
 * @suppress
 */
interface UniffiCleaner {
    interface Cleanable {
        fun clean()
    }

    fun register(value: Any, cleanUpTask: Runnable): UniffiCleaner.Cleanable

    companion object
}

private class UniffiJnaCleaner : UniffiCleaner {
    private val cleaner = com.sun.jna.internal.Cleaner.getCleaner()

    override fun register(value: Any, cleanUpTask: Runnable): UniffiCleaner.Cleanable =
        UniffiJnaCleanable(cleaner.register(value, cleanUpTask))
}

private class UniffiJnaCleanable(
    private val cleanable: com.sun.jna.internal.Cleaner.Cleanable,
) : UniffiCleaner.Cleanable {
    override fun clean() = cleanable.clean()
}

{% if config.disable_java_cleaner() %}
private fun UniffiCleaner.Companion.create(): UniffiCleaner = UniffiJnaCleaner()
{% else %}
{%-   include "ObjectCleanerHelperJvm.kt" %}
{%- endif %}
