/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef _mozilla_LoggingCore_h
#define _mozilla_LoggingCore_h

#include "mozilla/Atomics.h"
#include "mozilla/Types.h"

namespace mozilla {
enum class LogLevel {
  Disabled = 0,
  Error,
  Warning,
  Info,
  Debug,
  Verbose,
};

MFBT_API LogLevel ToLogLevel(int32_t aLevel);

using AtomicLogLevel = Atomic<LogLevel, Relaxed>;

}  

#endif /* _mozilla_LoggingCore_h */
