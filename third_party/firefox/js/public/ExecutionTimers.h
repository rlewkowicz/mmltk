/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ExecutionTimers_h
#define js_ExecutionTimers_h

#include "mozilla/TimeStamp.h"

#include "jstypes.h"

struct JS_PUBLIC_API JSContext;

namespace JS {

struct JSTimers {
  mozilla::TimeDuration executionTime;
  mozilla::TimeDuration delazificationTime;
};

extern JS_PUBLIC_API void SetMeasuringExecutionTimeEnabled(JSContext* cx,
                                                           bool value);
extern JS_PUBLIC_API JSTimers GetJSTimers(JSContext* cx);

}  

#endif  // js_ExecutionTimers_h
