/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_experimental_CodeCoverage_h
#define js_experimental_CodeCoverage_h

#include "jstypes.h"     // JS_PUBLIC_API
#include "js/Utility.h"  // JS::UniqueChars

struct JS_PUBLIC_API JSContext;

namespace js {

extern JS_PUBLIC_API void EnableCodeCoverage();

extern JS_PUBLIC_API JS::UniqueChars GetCodeCoverageSummary(JSContext* cx,
                                                            size_t* length);
extern JS_PUBLIC_API JS::UniqueChars GetCodeCoverageSummaryAll(JSContext* cx,
                                                               size_t* length);

}  

#endif  // js_experimental_CodeCoverage_h
