private fun UniffiCleaner.Companion.create(): UniffiCleaner =
    try {
        java.lang.Class.forName("java.lang.ref.Cleaner")
        JavaLangRefCleaner()
    } catch (e: ClassNotFoundException) {
        UniffiJnaCleaner()
    }

private class JavaLangRefCleaner : UniffiCleaner {
    val cleaner = java.lang.ref.Cleaner.create()

    override fun register(value: Any, cleanUpTask: Runnable): UniffiCleaner.Cleanable =
        JavaLangRefCleanable(cleaner.register(value, cleanUpTask))
}

private class JavaLangRefCleanable(
    val cleanable: java.lang.ref.Cleaner.Cleanable
) : UniffiCleaner.Cleanable {
    override fun clean() = cleanable.clean()
}
