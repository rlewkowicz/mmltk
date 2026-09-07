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

#include "absl/time/internal/cctz/src/time_zone_impl.h"

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "absl/base/config.h"
#include "absl/time/internal/cctz/src/time_zone_fixed.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace time_internal {
namespace cctz {

namespace {

using TimeZoneImplByName =
    std::unordered_map<std::string, const time_zone::Impl*>;
TimeZoneImplByName* time_zone_map = nullptr;

std::mutex& TimeZoneMutex() {
  static std::mutex* time_zone_mutex = new std::mutex;
  return *time_zone_mutex;
}

}  

time_zone time_zone::Impl::UTC() { return time_zone(UTCImpl()); }

bool time_zone::Impl::LoadTimeZone(const std::string& name, time_zone* tz) {
  const Impl* const utc_impl = UTCImpl();

  auto offset = seconds::zero();
  if (FixedOffsetFromName(name, &offset) && offset == seconds::zero()) {
    *tz = time_zone(utc_impl);
    return true;
  }

  {
    std::lock_guard<std::mutex> lock(TimeZoneMutex());
    if (time_zone_map != nullptr) {
      TimeZoneImplByName::const_iterator itr = time_zone_map->find(name);
      if (itr != time_zone_map->end()) {
        *tz = time_zone(itr->second);
        return itr->second != utc_impl;
      }
    }
  }

  std::unique_ptr<const Impl> new_impl(new Impl(name));

  std::lock_guard<std::mutex> lock(TimeZoneMutex());
  if (time_zone_map == nullptr) time_zone_map = new TimeZoneImplByName;
  const Impl*& impl = (*time_zone_map)[name];
  if (impl == nullptr) {  
    impl = new_impl->zone_ ? new_impl.release() : utc_impl;
  }
  *tz = time_zone(impl);
  return impl != utc_impl;
}

void time_zone::Impl::ClearTimeZoneMapTestOnly() {
  std::lock_guard<std::mutex> lock(TimeZoneMutex());
  if (time_zone_map != nullptr) {
    static auto* cleared = new std::deque<const time_zone::Impl*>;
    for (const auto& element : *time_zone_map) {
      cleared->push_back(element.second);
    }
    time_zone_map->clear();
  }
}

time_zone::Impl::Impl() : name_("UTC"), zone_(TimeZoneIf::UTC()) {}

time_zone::Impl::Impl(const std::string& name)
    : name_(name), zone_(TimeZoneIf::Make(name_)) {}

const time_zone::Impl* time_zone::Impl::UTCImpl() {
  static const Impl* utc_impl = new Impl;
  return utc_impl;
}

}  
}  
ABSL_NAMESPACE_END
}  
