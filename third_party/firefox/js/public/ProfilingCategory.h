/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ProfilingCategory_h
#define js_ProfilingCategory_h

#include "jstypes.h"  // JS_PUBLIC_API

#define JS_EXECUTION_CATEGORY_LIST(BEGIN_CATEGORY, SUBCATEGORY, END_CATEGORY) \
  BEGIN_CATEGORY(OTHER, "Other", "grey")                                    \
  SUBCATEGORY(OTHER, OTHER, "Other")                                         \
  END_CATEGORY                                                               \
  BEGIN_CATEGORY(JS, "JavaScript", "yellow")                                \
  SUBCATEGORY(JS, JS, "Other")                                               \
  SUBCATEGORY(JS, JS_Parsing, "Parsing")                                     \
  SUBCATEGORY(JS, JS_BaselineCompilation, "JIT Compile (baseline)")          \
  SUBCATEGORY(JS, JS_IonCompilation, "JIT Compile (ion)")                   \
  SUBCATEGORY(JS, JS_Interpreter, "Interpreter")                             \
  SUBCATEGORY(JS, JS_BaselineInterpret, "JIT (baseline-interpreter)")        \
  SUBCATEGORY(JS, JS_Baseline, "JIT (baseline)")                            \
  SUBCATEGORY(JS, JS_IonMonkey, "JIT (ion)")                                \
  SUBCATEGORY(JS, JS_Builtin, "Builtin API")                                 \
  SUBCATEGORY(JS, JS_WasmIon, "Wasm (ion)")                                 \
  SUBCATEGORY(JS, JS_WasmBaseline, "Wasm (baseline)")                       \
  SUBCATEGORY(JS, JS_WasmOther, "Wasm (other)")                             \
  END_CATEGORY                                                               \
  BEGIN_CATEGORY(GCCC, "GC / CC", "orange")                                \
  SUBCATEGORY(GCCC, GCCC, "Other")                                          \
  SUBCATEGORY(GCCC, GCCC_MinorGC, "Minor GC")                               \
  SUBCATEGORY(GCCC, GCCC_MajorGC, "Major GC (Other)")                       \
  SUBCATEGORY(GCCC, GCCC_MajorGC_Mark, "Major GC (Mark)")                   \
  SUBCATEGORY(GCCC, GCCC_MajorGC_Sweep, "Major GC (Sweep)")                 \
  SUBCATEGORY(GCCC, GCCC_MajorGC_Compact, "Major GC (Compact)")             \
  SUBCATEGORY(GCCC, GCCC_UnmarkGray, "Unmark Gray")                         \
  SUBCATEGORY(GCCC, GCCC_Barrier, "Barrier")                                \
  END_CATEGORY                                                               \
  BEGIN_CATEGORY(DOM, "DOM", "blue")                                       \
  SUBCATEGORY(DOM, DOM, "Other")                                            \
  END_CATEGORY                                                               \
  BEGIN_CATEGORY(PROFILER, "Profiler", "lightred")                         \
  SUBCATEGORY(PROFILER, PROFILER, "Other")                                  \
  END_CATEGORY

namespace JS {

// clang-format off

#define CATEGORY_ENUM_BEGIN_CATEGORY(name, labelAsString, color)
#define CATEGORY_ENUM_SUBCATEGORY(supercategory, name, labelAsString) name,
#define CATEGORY_ENUM_END_CATEGORY
enum class ProfilingCategoryPair : uint32_t {
  JS_EXECUTION_CATEGORY_LIST(CATEGORY_ENUM_BEGIN_CATEGORY,
                             CATEGORY_ENUM_SUBCATEGORY,
                             CATEGORY_ENUM_END_CATEGORY)
  COUNT,
  LAST = COUNT - 1,
};
#undef CATEGORY_ENUM_BEGIN_CATEGORY
#undef CATEGORY_ENUM_SUBCATEGORY
#undef CATEGORY_ENUM_END_CATEGORY

#define SUPERCATEGORY_ENUM_BEGIN_CATEGORY(name, labelAsString, color) name,
#define SUPERCATEGORY_ENUM_SUBCATEGORY(supercategory, name, labelAsString)
#define SUPERCATEGORY_ENUM_END_CATEGORY
enum class ProfilingCategory : uint32_t {
  JS_EXECUTION_CATEGORY_LIST(SUPERCATEGORY_ENUM_BEGIN_CATEGORY,
                             SUPERCATEGORY_ENUM_SUBCATEGORY,
                             SUPERCATEGORY_ENUM_END_CATEGORY)
  COUNT,
  LAST = COUNT - 1,
};
#undef SUPERCATEGORY_ENUM_BEGIN_CATEGORY
#undef SUPERCATEGORY_ENUM_SUBCATEGORY
#undef SUPERCATEGORY_ENUM_END_CATEGORY

// clang-format on

struct ProfilingCategoryPairInfo {
  ProfilingCategory mCategory;
  uint32_t mSubcategoryIndex;
  const char* mLabel;
};

JS_PUBLIC_API const ProfilingCategoryPairInfo& GetProfilingCategoryPairInfo(
    ProfilingCategoryPair aCategoryPair);

}  

#endif /* js_ProfilingCategory_h */
