/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_BuildId_h
#define js_BuildId_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Vector.h"  // js::Vector

namespace js {

class SystemAllocPolicy;

}  

namespace JS {

using BuildIdCharVector = js::Vector<char, 0, js::SystemAllocPolicy>;

using BuildIdOp = bool (*)(BuildIdCharVector* buildId);

extern JS_PUBLIC_API void SetProcessBuildIdOp(BuildIdOp buildIdOp);

[[nodiscard]] extern JS_PUBLIC_API bool GetOptimizedEncodingBuildId(
    BuildIdCharVector* buildId);

[[nodiscard]] extern JS_PUBLIC_API bool GetScriptTranscodingBuildId(
    BuildIdCharVector* buildId);

}  

#endif /* js_BuildId_h */
