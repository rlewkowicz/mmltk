/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(mozilla_Attributes_h)
#define mozilla_Attributes_h

#if defined(__cplusplus)
#  include <version>  // IWYU pragma: keep(__GLIBCXX__ lookup)
#endif

#if defined(_MSC_VER)
#  define MOZ_ALWAYS_INLINE_EVEN_DEBUG __forceinline
#elif defined(__GNUC__)
#  define MOZ_ALWAYS_INLINE_EVEN_DEBUG __attribute__((always_inline)) inline
#else
#  define MOZ_ALWAYS_INLINE_EVEN_DEBUG inline
#endif

#if defined(_MSC_VER)
#  define MOZ_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#  define MOZ_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

#if !defined(DEBUG)
#  define MOZ_ALWAYS_INLINE MOZ_ALWAYS_INLINE_EVEN_DEBUG
#elif defined(_MSC_VER) && !defined(__cplusplus)
#  define MOZ_ALWAYS_INLINE __inline
#else
#  define MOZ_ALWAYS_INLINE inline
#endif

#if defined(_MSC_VER)
#  define MOZ_HAVE_NEVER_INLINE __declspec(noinline)
#elif defined(__clang__)
#if !defined(__has_extension)
#    define __has_extension \
      __has_feature 
#endif
#if __has_attribute(noinline)
#    define MOZ_HAVE_NEVER_INLINE __attribute__((noinline))
#endif
#elif defined(__GNUC__)
#  define MOZ_HAVE_NEVER_INLINE __attribute__((noinline))
#  define MOZ_HAVE_NORETURN_PTR __attribute__((noreturn))
#endif

#if defined(__clang__)
#if __has_attribute(no_stack_protector)
#    define MOZ_HAVE_NO_STACK_PROTECTOR __attribute__((no_stack_protector))
#endif
#elif defined(__GNUC__)
#  define MOZ_HAVE_NO_STACK_PROTECTOR __attribute__((no_stack_protector))
#endif

#if defined(__clang__)
#  define MOZ_HAS_CLANG_ATTRIBUTE(attr) __has_attribute(attr)
#else
#  define MOZ_HAS_CLANG_ATTRIBUTE(attr) 0
#endif

#if MOZ_HAS_CLANG_ATTRIBUTE(__trivial_abi__)
#  define MOZ_TRIVIAL_ABI __attribute__((__trivial_abi__))
#else
#  define MOZ_TRIVIAL_ABI
#endif

#if defined(__clang_analyzer__)
#if __has_extension(attribute_analyzer_noreturn)
#    define MOZ_HAVE_ANALYZER_NORETURN __attribute__((analyzer_noreturn))
#endif
#endif

#if defined(__GNUC__) || MOZ_HAS_CLANG_ATTRIBUTE(no_profile_instrument_function)
#  define MOZ_NOPROFILE __attribute__((no_profile_instrument_function))
#else
#  define MOZ_NOPROFILE
#endif

#if defined(__GNUC__) || (MOZ_HAS_CLANG_ATTRIBUTE(no_instrument_function))
#  define MOZ_NOINSTRUMENT __attribute__((no_instrument_function))
#else
#  define MOZ_NOINSTRUMENT
#endif

#define MOZ_NAKED __attribute__((naked)) MOZ_NOPROFILE MOZ_NOINSTRUMENT

#if MOZ_HAS_CLANG_ATTRIBUTE(nomerge)
#  define MOZ_NOMERGE __attribute__((nomerge))
#else
#  define MOZ_NOMERGE
#endif

#if defined(MOZ_HAVE_NEVER_INLINE)
#  define MOZ_NEVER_INLINE MOZ_HAVE_NEVER_INLINE
#else
#  define MOZ_NEVER_INLINE /* no support */
#endif

#if defined(DEBUG)
#  define MOZ_NEVER_INLINE_DEBUG MOZ_NEVER_INLINE
#else
#  define MOZ_NEVER_INLINE_DEBUG /* don't inline in opt builds */
#endif
#if defined(MOZ_HAVE_NORETURN_PTR)
#  define MOZ_NORETURN_PTR MOZ_HAVE_NORETURN_PTR
#else
#  define MOZ_NORETURN_PTR /* no support */
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define MOZ_COLD __attribute__((cold))
#else
#  define MOZ_COLD
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define MOZ_NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))
#else
#  define MOZ_NONNULL(...)
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define MOZ_NONNULL_RETURN __attribute__((returns_nonnull))
#else
#  define MOZ_NONNULL_RETURN
#endif

#if defined(MOZ_HAVE_ANALYZER_NORETURN)
#  define MOZ_PRETEND_NORETURN_FOR_STATIC_ANALYSIS MOZ_HAVE_ANALYZER_NORETURN
#else
#  define MOZ_PRETEND_NORETURN_FOR_STATIC_ANALYSIS /* no support */
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#    define MOZ_HAVE_ASAN_IGNORE
#endif
#elif defined(__GNUC__)
#if defined(__SANITIZE_ADDRESS__)
#    define MOZ_HAVE_ASAN_IGNORE
#endif
#endif

#if defined(MOZ_HAVE_ASAN_IGNORE)
#  define MOZ_ASAN_IGNORE MOZ_NEVER_INLINE __attribute__((no_sanitize_address))
#else
#  define MOZ_ASAN_IGNORE /* nothing */
#endif

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#    define MOZ_TSAN_IGNORE MOZ_NEVER_INLINE __attribute__((no_sanitize_thread))
#else
#    define MOZ_TSAN_IGNORE /* nothing */
#endif
#else
#  define MOZ_TSAN_IGNORE /* nothing */
#endif

#if defined(__has_attribute)
#if __has_attribute(no_sanitize)
#    define MOZ_HAVE_NO_SANITIZE_ATTR
#endif
#endif

#if defined(__clang__)
#if defined(MOZ_HAVE_NO_SANITIZE_ATTR)
#    define MOZ_HAVE_UNSIGNED_OVERFLOW_SANITIZE_ATTR
#    define MOZ_HAVE_SIGNED_OVERFLOW_SANITIZE_ATTR
#endif
#endif

#if defined(MOZ_HAVE_UNSIGNED_OVERFLOW_SANITIZE_ATTR)
#  define MOZ_NO_SANITIZE_UNSIGNED_OVERFLOW \
    __attribute__((no_sanitize("unsigned-integer-overflow")))
#else
#  define MOZ_NO_SANITIZE_UNSIGNED_OVERFLOW /* nothing */
#endif

#if defined(MOZ_HAVE_SIGNED_OVERFLOW_SANITIZE_ATTR)
#  define MOZ_NO_SANITIZE_SIGNED_OVERFLOW \
    __attribute__((no_sanitize("signed-integer-overflow")))
#else
#  define MOZ_NO_SANITIZE_SIGNED_OVERFLOW /* nothing */
#endif

#undef MOZ_HAVE_NO_SANITIZE_ATTR

#if defined(__GNUC__) || defined(__clang__)
#  define MOZ_ALLOCATOR __attribute__((malloc, warn_unused_result))
#  define MOZ_INFALLIBLE_ALLOCATOR \
    __attribute__((malloc, warn_unused_result, returns_nonnull))
#else
#  define MOZ_ALLOCATOR
#  define MOZ_INFALLIBLE_ALLOCATOR
#endif

#if defined(MOZ_HAVE_NO_STACK_PROTECTOR)
#  define MOZ_NO_STACK_PROTECTOR MOZ_HAVE_NO_STACK_PROTECTOR
#else
#  define MOZ_NO_STACK_PROTECTOR /* no support */
#endif

#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(gsl::Owner)
#    define MOZ_GSL_OWNER [[gsl::Owner]]
#else
#    define MOZ_GSL_OWNER /* nothing */
#endif
#else
#  define MOZ_GSL_OWNER /* nothing */
#endif

#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(gsl::Pointer)
#    define MOZ_GSL_POINTER [[gsl::Pointer]]
#else
#    define MOZ_GSL_POINTER /* nothing */
#endif
#else
#  define MOZ_GSL_POINTER /* nothing */
#endif

#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#    define MOZ_LIFETIME_BOUND [[clang::lifetimebound]]
#else
#    define MOZ_LIFETIME_BOUND /* nothing */
#endif
#else
#  define MOZ_LIFETIME_BOUND /* nothing */
#endif

#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetime_capture_by)
#    define MOZ_LIFETIME_CAPTURE_BY(x) [[clang::lifetime_capture_by(x)]]
#else
#    define MOZ_LIFETIME_CAPTURE_BY(x) /* nothing */
#endif
#else
#  define MOZ_LIFETIME_CAPTURE_BY(x) /* nothing */
#endif

#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::reinitializes)
#    define MOZ_REINITIALIZES [[clang::reinitializes]]
#else
#    define MOZ_REINITIALIZES /* nothing */
#endif
#else
#  define MOZ_REINITIALIZES /* nothing */
#endif

/**
 * MOZ_NULL_AFTER_MOVE indicates that the associated type behaves as
 * std::unique_ptr once being moved, i.e. it's considered empty/null, and only
 * calls to opererator*(), operator-> and operator[]() are reported by static
 * analysis as invalid use-after-move.
 *
 * See:
 * https://clang.llvm.org/extra/clang-tidy/checks/bugprone/use-after-move.html#use
 */
#if defined(__clang__)
#  define MOZ_NULL_AFTER_MOVE                                  \
    [[clang::annotate("clang-tidy", "bugprone-use-after-move", \
                      "null_after_move")]]
#else
#  define MOZ_NULL_AFTER_MOVE /* nothing */
#endif

#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::standalone_debug)
#    define MOZ_STANDALONE_DEBUG [[clang::standalone_debug]]
#else
#    define MOZ_STANDALONE_DEBUG /* nothing */
#endif
#else
#  define MOZ_STANDALONE_DEBUG /* nothing */
#endif

#if defined(__cplusplus)

#if defined(_MSC_VER)
#    define MOZ_PUSH_DISABLE_NONTRIVIAL_UNION_WARNINGS          \
      __pragma(warning(push)) __pragma(warning(disable : 4582)) \
          __pragma(warning(disable : 4583))
#    define MOZ_POP_DISABLE_NONTRIVIAL_UNION_WARNINGS __pragma(warning(pop))
#else
#    define MOZ_PUSH_DISABLE_NONTRIVIAL_UNION_WARNINGS /* nothing */
#    define MOZ_POP_DISABLE_NONTRIVIAL_UNION_WARNINGS  /* nothing */
#endif

/*
 * The following macros are attributes that support the static analysis plugin
 * included with Mozilla, and will be implemented (when such support is enabled)
 * as C++11 attributes. Since such attributes are legal pretty much everywhere
 * and have subtly different semantics depending on their placement, the
 * following is a guide on where to place the attributes.
 *
 * Attributes that apply to a struct or class precede the name of the class:
 * (Note that this is different from the placement of final for classes!)
 *
 *   class MOZ_CLASS_ATTRIBUTE SomeClass {};
 *
 * Attributes that apply to functions follow the parentheses and const
 * qualifiers but precede final, override and the function body:
 *
 *   void DeclaredFunction() MOZ_FUNCTION_ATTRIBUTE;
 *   void SomeFunction() MOZ_FUNCTION_ATTRIBUTE {}
 *   void PureFunction() const MOZ_FUNCTION_ATTRIBUTE = 0;
 *   void OverriddenFunction() MOZ_FUNCTION_ATTIRBUTE override;
 *
 * Attributes that apply to variables or parameters follow the variable's name:
 *
 *   int variable MOZ_VARIABLE_ATTRIBUTE;
 *
 * Attributes that apply to types follow the type name:
 *
 *   typedef int MOZ_TYPE_ATTRIBUTE MagicInt;
 *   int MOZ_TYPE_ATTRIBUTE someVariable;
 *   int* MOZ_TYPE_ATTRIBUTE magicPtrInt;
 *   int MOZ_TYPE_ATTRIBUTE* ptrToMagicInt;
 *
 * Attributes that apply to statements precede the statement:
 *
 *   MOZ_IF_ATTRIBUTE if (x == 0)
 *   MOZ_DO_ATTRIBUTE do { } while (0);
 *
 * Attributes that apply to labels precede the label:
 *
 *   MOZ_LABEL_ATTRIBUTE target:
 *     goto target;
 *   MOZ_CASE_ATTRIBUTE case 5:
 *   MOZ_DEFAULT_ATTRIBUTE default:
 *
 * The static analyses that are performed by the plugin are as follows:
 *
 * MOZ_CAN_RUN_SCRIPT: Applies to functions which can run script. Callers of
 *   this function must also be marked as MOZ_CAN_RUN_SCRIPT, and all refcounted
 *   arguments must be strongly held in the caller. Note that MOZ_CAN_RUN_SCRIPT
 *   should only be applied to function declarations, not definitions. If you
 *   need to apply it to a definition (eg because both are generated by a macro)
 *   use MOZ_CAN_RUN_SCRIPT_FOR_DEFINITION.
 *
 *   MOZ_CAN_RUN_SCRIPT can be applied to XPIDL-generated declarations by
 *   annotating the method or attribute as [can_run_script] in the .idl file.
 *
 * MOZ_CAN_RUN_SCRIPT_FOR_DEFINITION: Same as MOZ_CAN_RUN_SCRIPT, but usable on
 *   a definition. If the declaration is in a header file, users of that header
 *   file may not see the annotation.
 * MOZ_CAN_RUN_SCRIPT_BOUNDARY: Applies to functions which need to call
 *   MOZ_CAN_RUN_SCRIPT functions, but should not themselves be considered
 *   MOZ_CAN_RUN_SCRIPT. This should generally be avoided but can be used in
 *   two cases:
 *     1) As a temporary measure to limit the scope of changes when adding
 *        MOZ_CAN_RUN_SCRIPT.  Such a use must be accompanied by a follow-up bug
 *        to replace the MOZ_CAN_RUN_SCRIPT_BOUNDARY with MOZ_CAN_RUN_SCRIPT and
 *        a comment linking to that bug.
 *     2) If we can reason that the MOZ_CAN_RUN_SCRIPT callees of the function
 *        do not in fact run script (for example, because their behavior depends
 *        on arguments and we pass the arguments that don't allow script
 *        execution).  Such a use must be accompanied by a comment that explains
 *        why it's OK to have the MOZ_CAN_RUN_SCRIPT_BOUNDARY, as well as
 *        comments in the callee pointing out that if its behavior changes the
 *        caller might need adjusting.  And perhaps also a followup bug to
 *        refactor things so the "script" and "no script" codepaths do not share
 *        a chokepoint.
 *   Importantly, any use MUST be accompanied by a comment explaining why it's
 *   there, and should ideally have an action plan for getting rid of the
 *   MOZ_CAN_RUN_SCRIPT_BOUNDARY annotation.
 * MOZ_MUST_OVERRIDE: Applies to all C++ member functions. All immediate
 *   subclasses must provide an exact override of this method; if a subclass
 *   does not override this method, the compiler will emit an error. This
 *   attribute is not limited to virtual methods, so if it is applied to a
 *   nonvirtual method and the subclass does not provide an equivalent
 *   definition, the compiler will emit an error.
 * MOZ_STATIC_CLASS: Applies to all classes. Any class with this annotation is
 *   expected to live in static memory, so it is a compile-time error to use
 *   it, or an array of such objects, as the type of a variable declaration, or
 *   as a temporary object, or as the type of a new expression (unless
 *   placement new is being used). If a member of another class uses this
 *   class, or if another class inherits from this class, then it is considered
 *   to be a static class as well, although this attribute need not be provided
 *   in such cases.
 * MOZ_STATIC_LOCAL_CLASS: Applies to all classes. Any class with this
 *   annotation is expected to be a static local variable, so it is
 *   a compile-time error to use it, or an array of such objects, or as a
 *   temporary object, or as the type of a new expression. If another class
 *   inherits from this class then it is considered to be a static local
 *   class as well, although this attribute need not be provided in such cases.
 *   It is also a compile-time error for any class with this annotation to have
 *   a non-trivial destructor.
 * MOZ_STACK_CLASS: Applies to all classes. Any class with this annotation is
 *   expected to live on the stack, so it is a compile-time error to use it, or
 *   an array of such objects, as a global or static variable, or as the type of
 *   a new expression (unless placement new is being used). If a member of
 *   another class uses this class, or if another class inherits from this
 *   class, then it is considered to be a stack class as well, although this
 *   attribute need not be provided in such cases.
 * MOZ_NONHEAP_CLASS: Applies to all classes. Any class with this annotation is
 *   expected to live on the stack or in static storage, so it is a compile-time
 *   error to use it, or an array of such objects, as the type of a new
 *   expression. If a member of another class uses this class, or if another
 *   class inherits from this class, then it is considered to be a non-heap
 *   class as well, although this attribute need not be provided in such cases.
 * MOZ_RUNINIT: Applies to global variables with runtime initialization.
 * MOZ_GLOBINIT: Applies to global variables with potential runtime
 *   initialization (e.g. inside macro or when initialisation status depends on
 *   template parameter).
 * MOZ_HEAP_CLASS: Applies to all classes. Any class with this annotation is
 *   expected to live on the heap, so it is a compile-time error to use it, or
 *   an array of such objects, as the type of a variable declaration, or as a
 *   temporary object. If a member of another class uses this class, or if
 *   another class inherits from this class, then it is considered to be a heap
 *   class as well, although this attribute need not be provided in such cases.
 * MOZ_NON_TEMPORARY_CLASS: Applies to all classes. Any class with this
 *   annotation is expected not to live in a temporary. If a member of another
 *   class uses this class or if another class inherits from this class, then it
 *   is considered to be a non-temporary class as well, although this attribute
 *   need not be provided in such cases.
 * MOZ_TEMPORARY_CLASS: Applies to all classes. Any class with this annotation
 *   is expected to only live in a temporary. If another class inherits from
 *   this class, then it is considered to be a temporary class as well, although
 *   this attribute need not be provided in such cases.
 * MOZ_RAII: Applies to all classes. Any class with this annotation is assumed
 *   to be a RAII guard, which is expected to live on the stack in an automatic
 *   allocation. It is prohibited from being allocated in a temporary, static
 *   storage, or on the heap. This is a combination of MOZ_STACK_CLASS and
 *   MOZ_NON_TEMPORARY_CLASS.
 * MOZ_ONLY_USED_TO_AVOID_STATIC_CONSTRUCTORS: Applies to all classes that are
 *   intended to prevent introducing static initializers.  This attribute
 *   currently makes it a compile-time error to instantiate these classes
 *   anywhere other than at the global scope, or as a static member of a class.
 *   In non-debug mode, it also prohibits non-trivial constructors and
 *   destructors.
 * MOZ_TRIVIAL_CTOR_DTOR: Applies to all classes that must have both a trivial
 *   or constexpr constructor and a trivial destructor. Setting this attribute
 *   on a class makes it a compile-time error for that class to get a
 *   non-trivial constructor or destructor for any reason.
 * MOZ_ALLOW_TEMPORARY: Applies to constructors. This indicates that using the
 *   constructor is allowed in temporary expressions, if it would have otherwise
 *   been forbidden by the type being a MOZ_NON_TEMPORARY_CLASS. Useful for
 *   constructors like Maybe(Nothing).
 * MOZ_HEAP_ALLOCATOR: Applies to any function. This indicates that the return
 *   value is allocated on the heap, and will as a result check such allocations
 *   during MOZ_STACK_CLASS and MOZ_NONHEAP_CLASS annotation checking.
 * MOZ_IMPLICIT: Applies to constructors. Implicit conversion constructors
 *   are disallowed by default unless they are marked as MOZ_IMPLICIT. This
 *   attribute must be used for constructors which intend to provide implicit
 *   conversions.
 * MOZ_IS_REFPTR: Applies to class declarations of ref pointer to mark them as
 *   such for use with static-analysis.
 *   A ref pointer is an object wrapping a pointer and automatically taking care
 *   of its refcounting upon construction/destruction/transfer of ownership.
 *   This annotation implies MOZ_IS_SMARTPTR_TO_REFCOUNTED.
 * MOZ_IS_SMARTPTR_TO_REFCOUNTED: Applies to class declarations of smart
 *   pointers to ref counted classes to mark them as such for use with
 *   static-analysis.
 * MOZ_NO_ARITHMETIC_EXPR_IN_ARGUMENT: Applies to functions. Makes it a compile
 *   time error to pass arithmetic expressions on variables to the function.
 * MOZ_OWNING_REF: Applies to declarations of pointers to reference counted
 *   types.  This attribute tells the compiler that the raw pointer is a strong
 *   reference, where ownership through methods such as AddRef and Release is
 *   managed manually.  This can make the compiler ignore these pointers when
 *   validating the usage of pointers otherwise.
 *
 *   Example uses include owned pointers inside of unions, and pointers stored
 *   in POD types where a using a smart pointer class would make the object
 *   non-POD.
 * MOZ_NON_OWNING_REF: Applies to declarations of pointers to reference counted
 *   types.  This attribute tells the compiler that the raw pointer is a weak
 *   reference, which is ensured to be valid by a guarantee that the reference
 *   will be nulled before the pointer becomes invalid.  This can make the
 *   compiler ignore these pointers when validating the usage of pointers
 *   otherwise.
 *
 *   Examples include an mOwner pointer, which is nulled by the owning class's
 *   destructor, and is null-checked before dereferencing.
 * MOZ_UNSAFE_REF: Applies to declarations of pointers to reference counted
 *   types.  Occasionally there are non-owning references which are valid, but
 *   do not take the form of a MOZ_NON_OWNING_REF.  Their safety may be
 *   dependent on the behaviour of API consumers.  The string argument passed
 *   to this macro documents the safety conditions.  This can make the compiler
 *   ignore these pointers when validating the usage of pointers elsewhere.
 *
 *   Examples include an nsAtom* member which is known at compile time to point
 *   to a static atom which is valid throughout the lifetime of the program, or
 *   an API which stores a pointer, but doesn't take ownership over it, instead
 *   requiring the API consumer to correctly null the value before it becomes
 *   invalid.
 *
 *   Use of this annotation is discouraged when a strong reference or one of
 *   the above two annotations can be used instead.
 * MOZ_NON_TERMINATED_STRING: Applies to function declarations.  Indicates that
 *   the return value is a character pointer that is not null-terminated.
 *   Makes it a compile time error to pass the return value of such a function
 *   as an argument to a printf-like function (one annotated with
 *   MOZ_FORMAT_PRINTF), since printf %s expects null-terminated input.
 * MOZ_NO_ADDREF_RELEASE_ON_RETURN: Applies to function declarations.  Makes it
 *   a compile time error to call AddRef or Release on the return value of a
 *   function.  This is intended to be used with operator->() of our smart
 *   pointer classes to ensure that the refcount of an object wrapped in a
 *   smart pointer is not manipulated directly.
 * MOZ_NEEDS_NO_VTABLE_TYPE: Applies to template class declarations.  Makes it
 *   a compile time error to instantiate this template with a type parameter
 *   which has a VTable.
 * MOZ_NON_MEMMOVABLE: Applies to class declarations for types that are not safe
 *   to be moved in memory using memmove().
 * MOZ_NEEDS_MEMMOVABLE_TYPE: Applies to template class declarations where the
 *   template arguments are required to be safe to move in memory using
 *   memmove().  Passing MOZ_NON_MEMMOVABLE types to these templates is a
 *   compile time error.
 * MOZ_NEEDS_MEMMOVABLE_MEMBERS: Applies to class declarations where each member
 *   must be safe to move in memory using memmove().  MOZ_NON_MEMMOVABLE types
 *   used in members of these classes are compile time errors.
 * MOZ_NO_DANGLING_ON_TEMPORARIES: Applies to method declarations which return
 *   a pointer that is freed when the destructor of the class is called. This
 *   prevents these methods from being called on temporaries of the class,
 *   reducing risks of use-after-free.
 *   This attribute cannot be applied to && methods.
 *   In some cases, adding a deleted &&-qualified overload is too restrictive as
 *   this method should still be callable as a non-escaping argument to another
 *   function. This annotation can be used in those cases.
 * MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS: Applies to template class
 *   declarations where an instance of the template should be considered, for
 *   static analysis purposes, to inherit any type annotations (such as
 *   MOZ_STACK_CLASS) from its template arguments.
 * MOZ_INIT_OUTSIDE_CTOR: Applies to class member declarations. Occasionally
 *   there are class members that are not initialized in the constructor,
 *   but logic elsewhere in the class ensures they are initialized prior to use.
 *   Using this attribute on a member disables the check that this member must
 *   be initialized in constructors via list-initialization, in the constructor
 *   body, or via functions called from the constructor body.
 * MOZ_IS_CLASS_INIT: Applies to class method declarations. Occasionally the
 *   constructor doesn't initialize all of the member variables and another
 *   function is used to initialize the rest. This marker is used to make the
 *   static analysis tool aware that the marked function is part of the
 *   initialization process and to include the marked function in the scan
 *   mechanism that determines which member variables still remain
 *   uninitialized.
 * MOZ_NON_PARAM: Applies to types. Makes it compile time error to use the type
 *   in parameter without pointer or reference.
 * MOZ_NON_AUTOABLE: Applies to class declarations. Makes it a compile time
 *   error to use `auto` in place of this type in variable declarations.  This
 *   is intended to be used with types which are intended to be implicitly
 *   constructed into other other types before being assigned to variables.
 * MOZ_REQUIRED_BASE_METHOD: Applies to virtual class method declarations.
 *   Sometimes derived classes override methods that need to be called by their
 *   overridden counterparts. This marker indicates that the marked method must
 *   be called by the method that it overrides.
 * MOZ_MUST_RETURN_FROM_CALLER_IF_THIS_IS_ARG: Applies to method declarations.
 *   Callers of the annotated method must return from that function within the
 *   calling block using an explicit `return` statement if the "this" value for
 *   the call is a parameter of the caller.  Only calls to Constructors,
 *   references to local and member variables, and calls to functions or
 *   methods marked as MOZ_MAY_CALL_AFTER_MUST_RETURN may be made after the
 *   MOZ_MUST_RETURN_FROM_CALLER_IF_THIS_IS_ARG call.
 * MOZ_MAY_CALL_AFTER_MUST_RETURN: Applies to function or method declarations.
 *   Calls to these methods may be made in functions after calls a
 *   MOZ_MUST_RETURN_FROM_CALLER_IF_THIS_IS_ARG method.
 * MOZ_UNANNOTATED/MOZ_ANNOTATED: Applies to Mutexes/Monitors and variations on
 *   them. MOZ_UNANNOTATED indicates that the Mutex/Monitor/etc hasn't been
 *   examined and annotated using macros from mfbt/ThreadSafety --
 *   MOZ_GUARDED_BY()/REQUIRES()/etc. MOZ_ANNOTATED is used in rare cases to
 *   indicate that is has been looked at, but it did not need any
 *   MOZ_GUARDED_BY()/REQUIRES()/etc (and thus static analysis knows it can
 * ignore this Mutex/Monitor/etc)
 * MOZ_ENUM_SERIALIZER_ALLOW_SENTINEL_UPPER_BOUND: Applies to ParamTraits
 *   specializations that inherit from ContiguousEnumSerializerInclusive.
 *   Suppresses the EnumSerializer checker warning about including a sentinel
 *   enumerator (e.g. one named *_END, *Count, *Invalid) as a valid serialized
 *   value. Use only when the sentinel is genuinely a valid value to send.
 * MOZ_ENUM_SERIALIZER_ALLOW_MIN_MISMATCH: Applies to ParamTraits
 *   specializations that inherit from ContiguousEnumSerializer[Inclusive].
 *   Suppresses the EnumSerializer checker warning about the min template
 *   argument not matching the first (lowest-valued) enumerator. Use when
 *   deliberately excluding lower enumerators from being serialized.
 */

#if defined(XGILL_PLUGIN)
#    pragma GCC diagnostic ignored "-Wignored-attributes"
#    pragma GCC diagnostic ignored "-Wattributes"
#endif

#if defined(MOZ_CLANG_PLUGIN) || defined(XGILL_PLUGIN)
#    define MOZ_CAN_RUN_SCRIPT __attribute__((annotate("moz_can_run_script")))
#    define MOZ_CAN_RUN_SCRIPT_FOR_DEFINITION         \
      __attribute__((annotate("moz_can_run_script"))) \
      __attribute__((annotate("moz_can_run_script_for_definition")))
#    define MOZ_CAN_RUN_SCRIPT_BOUNDARY \
      __attribute__((annotate("moz_can_run_script_boundary")))
#    define MOZ_MUST_OVERRIDE __attribute__((annotate("moz_must_override")))
#    define MOZ_STATIC_CLASS __attribute__((annotate("moz_global_class")))
#    define MOZ_STATIC_LOCAL_CLASS                        \
      __attribute__((annotate("moz_static_local_class"))) \
      __attribute__((annotate("moz_trivial_dtor")))
#    define MOZ_STACK_CLASS __attribute__((annotate("moz_stack_class")))
#    define MOZ_NONHEAP_CLASS __attribute__((annotate("moz_nonheap_class")))
#    define MOZ_HEAP_CLASS __attribute__((annotate("moz_heap_class")))
#    define MOZ_NON_TEMPORARY_CLASS \
      __attribute__((annotate("moz_non_temporary_class")))
#    define MOZ_TEMPORARY_CLASS __attribute__((annotate("moz_temporary_class")))
#    define MOZ_TRIVIAL_CTOR_DTOR \
      __attribute__((annotate("moz_trivial_ctor_dtor")))
#    define MOZ_ALLOW_TEMPORARY __attribute__((annotate("moz_allow_temporary")))
#if defined(DEBUG)
#      define MOZ_ONLY_USED_TO_AVOID_STATIC_CONSTRUCTORS \
        __attribute__((annotate("moz_global_class")))
#else
#      define MOZ_ONLY_USED_TO_AVOID_STATIC_CONSTRUCTORS \
        __attribute__((annotate("moz_global_class"))) MOZ_TRIVIAL_CTOR_DTOR
#endif
#    define MOZ_IMPLICIT __attribute__((annotate("moz_implicit")))
#    define MOZ_IS_SMARTPTR_TO_REFCOUNTED \
      __attribute__((annotate("moz_is_smartptr_to_refcounted")))
#    define MOZ_IS_REFPTR MOZ_IS_SMARTPTR_TO_REFCOUNTED
#    define MOZ_NO_ARITHMETIC_EXPR_IN_ARGUMENT \
      __attribute__((annotate("moz_no_arith_expr_in_arg")))
#    define MOZ_OWNING_REF __attribute__((annotate("moz_owning_ref")))
#    define MOZ_NON_OWNING_REF __attribute__((annotate("moz_non_owning_ref")))
#    define MOZ_UNSAFE_REF(reason) __attribute__((annotate("moz_unsafe_ref")))
#    define MOZ_NON_TERMINATED_STRING \
      __attribute__((annotate("moz_non_terminated_string")))
#    define MOZ_NO_ADDREF_RELEASE_ON_RETURN \
      __attribute__((annotate("moz_no_addref_release_on_return")))
#    define MOZ_NEEDS_NO_VTABLE_TYPE \
      __attribute__((annotate("moz_needs_no_vtable_type")))
#    define MOZ_NON_MEMMOVABLE __attribute__((annotate("moz_non_memmovable")))
#    define MOZ_NEEDS_MEMMOVABLE_TYPE \
      __attribute__((annotate("moz_needs_memmovable_type")))
#    define MOZ_NEEDS_MEMMOVABLE_MEMBERS \
      __attribute__((annotate("moz_needs_memmovable_members")))
#    define MOZ_NO_DANGLING_ON_TEMPORARIES \
      __attribute__((annotate("moz_no_dangling_on_temporaries")))
#    define MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS \
      __attribute__((                                       \
          annotate("moz_inherit_type_annotations_from_template_args")))
#    define MOZ_NON_AUTOABLE __attribute__((annotate("moz_non_autoable")))
#    define MOZ_INIT_OUTSIDE_CTOR
#    define MOZ_IS_CLASS_INIT
#    define MOZ_NON_PARAM __attribute__((annotate("moz_non_param")))
#    define MOZ_ENUM_SERIALIZER_ALLOW_SENTINEL_UPPER_BOUND \
      __attribute__((                                      \
          annotate("moz_enum_serializer_allow_sentinel_upper_bound")))
#    define MOZ_ENUM_SERIALIZER_ALLOW_MIN_MISMATCH \
      __attribute__((annotate("moz_enum_serializer_allow_min_mismatch")))
#    define MOZ_REQUIRED_BASE_METHOD \
      __attribute__((annotate("moz_required_base_method")))
#    define MOZ_MUST_RETURN_FROM_CALLER_IF_THIS_IS_ARG \
      __attribute__((annotate("moz_must_return_from_caller_if_this_is_arg")))
#    define MOZ_MAY_CALL_AFTER_MUST_RETURN \
      __attribute__((annotate("moz_may_call_after_must_return")))
#    define MOZ_KNOWN_LIVE __attribute__((annotate("moz_known_live")))
#if defined(MOZ_CLANG_PLUGIN)
#      define MOZ_UNANNOTATED __attribute__((annotate("moz_unannotated")))
#      define MOZ_ANNOTATED __attribute__((annotate("moz_annotated")))
#      define MOZ_RUNINIT __attribute__((annotate("moz_global_var")))
#      define MOZ_GLOBINIT \
        MOZ_RUNINIT __attribute__((annotate("moz_generated")))
#else
#      define MOZ_UNANNOTATED /* nothing */
#      define MOZ_ANNOTATED   /* nothing */
#      define MOZ_RUNINIT     /* nothing */
#      define MOZ_GLOBINIT    /* nothing */
#endif

#if defined(__GLIBCXX__) && (__GLIBCXX__ <= 20230707)
#      define MOZ_GLIBCXX_CONSTINIT MOZ_RUNINIT
#else
#      define MOZ_GLIBCXX_CONSTINIT constinit
#endif

#if defined(__clang__)
#      define MOZ_HEAP_ALLOCATOR                                         \
        _Pragma("clang diagnostic push")                                 \
            _Pragma("clang diagnostic ignored \"-Wgcc-compat\"")         \
                __attribute__((annotate("moz_heap_allocator"))) _Pragma( \
                    "clang diagnostic pop")
#else
#      define MOZ_HEAP_ALLOCATOR __attribute__((annotate("moz_heap_allocator")))
#endif
#else
#    define MOZ_CAN_RUN_SCRIPT                              /* nothing */
#    define MOZ_CAN_RUN_SCRIPT_FOR_DEFINITION               /* nothing */
#    define MOZ_CAN_RUN_SCRIPT_BOUNDARY                     /* nothing */
#    define MOZ_MUST_OVERRIDE                               /* nothing */
#    define MOZ_STATIC_CLASS                                /* nothing */
#    define MOZ_RUNINIT                                     /* nothing */
#    define MOZ_GLOBINIT                                    /* nothing */
#    define MOZ_GLIBCXX_CONSTINIT                           /* nothing */
#    define MOZ_RELEASE_CONSTINIT                           /* nothing */
#    define MOZ_STATIC_LOCAL_CLASS                          /* nothing */
#    define MOZ_STACK_CLASS                                 /* nothing */
#    define MOZ_NONHEAP_CLASS                               /* nothing */
#    define MOZ_HEAP_CLASS                                  /* nothing */
#    define MOZ_NON_TEMPORARY_CLASS                         /* nothing */
#    define MOZ_TEMPORARY_CLASS                             /* nothing */
#    define MOZ_TRIVIAL_CTOR_DTOR                           /* nothing */
#    define MOZ_ALLOW_TEMPORARY                             /* nothing */
#    define MOZ_ONLY_USED_TO_AVOID_STATIC_CONSTRUCTORS      /* nothing */
#    define MOZ_IMPLICIT                                    /* nothing */
#    define MOZ_IS_SMARTPTR_TO_REFCOUNTED                   /* nothing */
#    define MOZ_IS_REFPTR                                   /* nothing */
#    define MOZ_NO_ARITHMETIC_EXPR_IN_ARGUMENT              /* nothing */
#    define MOZ_HEAP_ALLOCATOR                              /* nothing */
#    define MOZ_OWNING_REF                                  /* nothing */
#    define MOZ_NON_OWNING_REF                              /* nothing */
#    define MOZ_UNSAFE_REF(reason)                          /* nothing */
#    define MOZ_NON_TERMINATED_STRING                       /* nothing */
#    define MOZ_NO_ADDREF_RELEASE_ON_RETURN                 /* nothing */
#    define MOZ_NEEDS_NO_VTABLE_TYPE                        /* nothing */
#    define MOZ_NON_MEMMOVABLE                              /* nothing */
#    define MOZ_NEEDS_MEMMOVABLE_TYPE                       /* nothing */
#    define MOZ_NEEDS_MEMMOVABLE_MEMBERS                    /* nothing */
#    define MOZ_NO_DANGLING_ON_TEMPORARIES                  /* nothing */
#    define MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS /* nothing */
#    define MOZ_INIT_OUTSIDE_CTOR                           /* nothing */
#    define MOZ_IS_CLASS_INIT                               /* nothing */
#    define MOZ_NON_PARAM                                   /* nothing */
#    define MOZ_ENUM_SERIALIZER_ALLOW_SENTINEL_UPPER_BOUND  /* nothing */
#    define MOZ_ENUM_SERIALIZER_ALLOW_MIN_MISMATCH          /* nothing */
#    define MOZ_NON_AUTOABLE                                /* nothing */
#    define MOZ_REQUIRED_BASE_METHOD                        /* nothing */
#    define MOZ_MUST_RETURN_FROM_CALLER_IF_THIS_IS_ARG      /* nothing */
#    define MOZ_MAY_CALL_AFTER_MUST_RETURN                  /* nothing */
#    define MOZ_KNOWN_LIVE                                  /* nothing */
#    define MOZ_UNANNOTATED                                 /* nothing */
#    define MOZ_ANNOTATED                                   /* nothing */
#endif

#  define MOZ_RAII MOZ_NON_TEMPORARY_CLASS MOZ_STACK_CLASS


#if defined(XGILL_PLUGIN)

#    undef MOZ_MUST_OVERRIDE
#    undef MOZ_CAN_RUN_SCRIPT_FOR_DEFINITION
#    undef MOZ_CAN_RUN_SCRIPT
#    undef MOZ_CAN_RUN_SCRIPT_BOUNDARY
#    define MOZ_MUST_OVERRIDE                 /* nothing */
#    define MOZ_CAN_RUN_SCRIPT_FOR_DEFINITION /* nothing */
#    define MOZ_CAN_RUN_SCRIPT                /* nothing */
#    define MOZ_CAN_RUN_SCRIPT_BOUNDARY       /* nothing */

#endif

#endif

#if defined(__MINGW32__)
#  define MOZ_FORMAT_PRINTF(stringIndex, firstToCheck) \
    __attribute__((format(__MINGW_PRINTF_FORMAT, stringIndex, firstToCheck)))
#if !defined(__MINGW_WPRINTF_FORMAT)
#if defined(__clang__)
#      define __MINGW_WPRINTF_FORMAT wprintf
#elif defined(_UCRT) || __USE_MINGW_ANSI_STDIO
#      define __MINGW_WPRINTF_FORMAT gnu_wprintf
#else
#      define __MINGW_WPRINTF_FORMAT ms_wprintf
#endif
#endif
#  define MOZ_FORMAT_WPRINTF(stringIndex, firstToCheck) \
    __attribute__((format(__MINGW_WPRINTF_FORMAT, stringIndex, firstToCheck)))
#elif __GNUC__ || __clang__
#  define MOZ_FORMAT_PRINTF(stringIndex, firstToCheck) \
    __attribute__((format(printf, stringIndex, firstToCheck)))
#  define MOZ_FORMAT_WPRINTF(stringIndex, firstToCheck) \
    __attribute__((format(wprintf, stringIndex, firstToCheck)))
#else
#  define MOZ_FORMAT_PRINTF(stringIndex, firstToCheck)
#  define MOZ_FORMAT_WPRINTF(stringIndex, firstToCheck)
#endif

#  define MOZ_XPCOM_ABI

#if defined(_MSC_VER)
#  define MOZ_EMPTY_BASES __declspec(empty_bases)
#else
#  define MOZ_EMPTY_BASES
#endif

#if defined(__clang__)
#  define MOZ_CAN_RUN_SCRIPT_BOUNDARY_LAMBDA MOZ_CAN_RUN_SCRIPT_BOUNDARY
#else
#  define MOZ_CAN_RUN_SCRIPT_BOUNDARY_LAMBDA
#endif

#endif
