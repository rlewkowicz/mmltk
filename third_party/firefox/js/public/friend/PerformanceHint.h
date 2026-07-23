/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_friend_PerformanceHint_h
#define js_friend_PerformanceHint_h

#include "jstypes.h"       // JS_PUBLIC_API
#include "js/TypeDecls.h"  // JSContext

namespace js {
namespace gc {


enum class PerformanceHint { Normal, InPageLoad };

extern JS_PUBLIC_API void SetPerformanceHint(JSContext* cx,
                                             PerformanceHint hint);

} 
} 

#endif  // js_friend_PerformanceHint_h
