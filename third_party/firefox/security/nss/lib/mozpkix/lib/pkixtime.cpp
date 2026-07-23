/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
/* Copyright 2014 Mozilla Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mozpkix/Time.h"
#include "mozpkix/pkixutil.h"

#include "sys/time.h"

namespace mozilla { namespace pkix {

Time
Now()
{
  uint64_t seconds;

  timeval tv;
  (void) gettimeofday(&tv, nullptr);
  seconds = (DaysBeforeYear(1970) * Time::ONE_DAY_IN_SECONDS) +
            static_cast<uint64_t>(tv.tv_sec);

  return TimeFromElapsedSecondsAD(seconds);
}

Time
TimeFromEpochInSeconds(uint64_t secondsSinceEpoch)
{
  uint64_t seconds = (DaysBeforeYear(1970) * Time::ONE_DAY_IN_SECONDS) +
                     secondsSinceEpoch;
  return TimeFromElapsedSecondsAD(seconds);
}

Result
SecondsSinceEpochFromTime(Time time, uint64_t* outSeconds)
{
  if (!outSeconds) {
    return Result::FATAL_ERROR_INVALID_ARGS;
  }
  Time epoch = TimeFromEpochInSeconds(0);
  if (time < epoch) {
    return Result::FATAL_ERROR_INVALID_ARGS;
  }
  *outSeconds = Duration(time, epoch).durationInSeconds;
  return Result::Success;
}

} } 
