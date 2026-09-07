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

#ifndef ABSL_TIME_INTERNAL_CCTZ_ZONE_INFO_SOURCE_H_
#define ABSL_TIME_INTERNAL_CCTZ_ZONE_INFO_SOURCE_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace time_internal {
namespace cctz {

class ZoneInfoSource {
 public:
  virtual ~ZoneInfoSource();

  virtual std::size_t Read(void* ptr, std::size_t size) = 0;  
  virtual int Skip(std::size_t offset) = 0;                   

  virtual std::string Version() const;
};

}  
}  
ABSL_NAMESPACE_END
}  

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace time_internal {
namespace cctz_extension {

using ZoneInfoSourceFactory =
    std::unique_ptr<absl::time_internal::cctz::ZoneInfoSource> (*)(
        const std::string&,
        const std::function<std::unique_ptr<
            absl::time_internal::cctz::ZoneInfoSource>(const std::string&)>&);

extern ZoneInfoSourceFactory zone_info_source_factory;

}  
}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_TIME_INTERNAL_CCTZ_ZONE_INFO_SOURCE_H_
