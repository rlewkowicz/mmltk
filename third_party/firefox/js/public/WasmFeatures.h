/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_WasmFeatures_h
#define js_WasmFeatures_h


#ifdef ENABLE_WASM_RELAXED_SIMD
#  define WASM_RELAXED_SIMD_ENABLED 1
#else
#  define WASM_RELAXED_SIMD_ENABLED 0
#endif
#ifdef ENABLE_WASM_MEMORY_CONTROL
#  define WASM_MEMORY_CONTROL_ENABLED 1
#else
#  define WASM_MEMORY_CONTROL_ENABLED 0
#endif
#ifdef ENABLE_WASM_JSPI
#  define WASM_JSPI_ENABLED 1
#else
#  define WASM_JSPI_ENABLED 0
#endif
#ifdef ENABLE_WASM_MOZ_INTGEMM
#  define WASM_MOZ_INTGEMM_ENABLED 1
#else
#  define WASM_MOZ_INTGEMM_ENABLED 0
#endif
#ifdef ENABLE_WASM_BRANCH_HINTING
#  define WASM_BRANCH_HINTING_ENABLED 1
#else
#  define WASM_BRANCH_HINTING_ENABLED 0
#endif
#ifdef ENABLE_WASM_CUSTOM_PAGE_SIZES
#  define WASM_CUSTOM_PAGE_SIZES_ENABLED 1
#else
#  define WASM_CUSTOM_PAGE_SIZES_ENABLED 0
#endif
#ifdef ENABLE_WASM_COMPACT_IMPORTS
#  define WASM_COMPACT_IMPORTS_ENABLED 1
#else
#  define WASM_COMPACT_IMPORTS_ENABLED 0
#endif
#ifdef ENABLE_WASM_COMPONENTS
#  define WASM_COMPONENTS_ENABLED 1
#else
#  define WASM_COMPONENTS_ENABLED 0
#endif

// clang-format off
#define JS_FOR_WASM_FEATURES(FEATURE)                                   \
  FEATURE(                                                              \
     RelaxedSimd,                               \
     relaxedSimd,                               \
     WASM_RELAXED_SIMD_ENABLED,                 \
     AnyCompilerAvailable(cx),                  \
     js::jit::JitSupportsWasmSimd(),            \
     false,                                     \
     relaxed_simd)                              \
  FEATURE(                                                              \
     MemoryControl,                             \
     memoryControl,                             \
     WASM_MEMORY_CONTROL_ENABLED,               \
     AnyCompilerAvailable(cx),                  \
     true,                                      \
     false,                                     \
     memory_control)                            \
  FEATURE(                                                              \
     JSPromiseIntegration,                      \
     jsPromiseIntegration,                      \
     WASM_JSPI_ENABLED,                         \
     IonPlatformSupport(),                      \
     true,                                      \
     false,                                     \
     js_promise_integration)                    \
  FEATURE(                                                              \
     StackSwitching,                            \
     stackSwitching,                            \
     WASM_JSPI_ENABLED,                         \
     IonPlatformSupport(),                      \
     true,                                      \
     false,                                     \
     stack_switching)                           \
  FEATURE(                                                              \
     MozIntGemm,                                \
     mozIntGemm,                                \
     WASM_MOZ_INTGEMM_ENABLED,                  \
     AnyCompilerAvailable(cx),                  \
     IsPrivilegedContext(cx),                   \
     false,                                     \
     moz_intgemm)                               \
  FEATURE(                                                              \
     TestSerialization,                         \
     testSerialization,                         \
     1,                                         \
     IonAvailable(cx),                          \
     true,                                      \
     false,                                     \
     test_serialization)                        \
  FEATURE(                                                              \
     BranchHinting,                             \
     branchHinting,                             \
     WASM_BRANCH_HINTING_ENABLED,               \
     true,                                      \
     true,                                      \
     false,                                     \
     branch_hinting)                            \
  FEATURE(                                                              \
     CustomPageSizes,                           \
     customPageSizes,                           \
     WASM_CUSTOM_PAGE_SIZES_ENABLED,            \
     BaselineAvailable(cx),                     \
     true,                                      \
     false,                                     \
     custom_page_sizes)                         \
  FEATURE(                                                              \
     CompactImports,                            \
     compactImports,                            \
     WASM_COMPACT_IMPORTS_ENABLED,              \
     AnyCompilerAvailable(cx),                  \
     true,                                      \
     false,                                     \
     compact_imports)                           \
  FEATURE(                                                              \
     WideArithmetic,                            \
     wideArithmetic,                            \
     1,                                         \
     AnyCompilerAvailable(cx),                  \
     true,                                      \
     false,                                     \
     wide_arithmetic)                           \
  FEATURE(                                                              \
     Components,                                \
     components,                                \
     WASM_COMPONENTS_ENABLED,                   \
     AnyCompilerAvailable(cx),                  \
     true,                                      \
     false,                                     \
     components)

// clang-format on

#endif  // js_WasmFeatures_h
