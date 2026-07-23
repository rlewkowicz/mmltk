// Copyright 2016 Google Inc. All Rights Reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//   https://www.apache.org/licenses/LICENSE-2.0
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.


#ifndef ABSL_TIME_INTERNAL_CCTZ_TIME_ZONE_POSIX_H_
#define ABSL_TIME_INTERNAL_CCTZ_TIME_ZONE_POSIX_H_

#include <cstdint>
#include <string>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace time_internal {
namespace cctz {

struct PosixTransition {
  enum DateFormat { J, N, M };

  struct Date {
    struct NonLeapDay {
      std::int_fast16_t day;  
    };
    struct Day {
      std::int_fast16_t day;  
    };
    struct MonthWeekWeekday {
      std::int_fast8_t month;    
      std::int_fast8_t week;     
      std::int_fast8_t weekday;  
    };

    DateFormat fmt;

    union {
      NonLeapDay j;
      Day n;
      MonthWeekWeekday m;
    };
  };

  struct Time {
    std::int_fast32_t offset;  
  };

  Date date;
  Time time;
};

struct PosixTimeZone {
  std::string std_abbr;
  std::int_fast32_t std_offset;

  std::string dst_abbr;
  std::int_fast32_t dst_offset;
  PosixTransition dst_start;
  PosixTransition dst_end;
};

bool ParsePosixSpec(const std::string& spec, PosixTimeZone* res);

}  
}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_TIME_INTERNAL_CCTZ_TIME_ZONE_POSIX_H_
