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

#include "absl/base/config.h"
#include "absl/time/internal/cctz/include/cctz/time_zone.h"



#if defined(__Fuchsia__)
#include <fuchsia/intl/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/directory.h>
#include <zircon/types.h>
#endif

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "absl/time/internal/cctz/src/time_zone_fixed.h"
#include "absl/time/internal/cctz/src/time_zone_impl.h"


namespace absl {
ABSL_NAMESPACE_BEGIN
namespace time_internal {
namespace cctz {

std::string time_zone::name() const { return effective_impl().Name(); }

time_zone::absolute_lookup time_zone::lookup(
    const time_point<seconds>& tp) const {
  return effective_impl().BreakTime(tp);
}

time_zone::civil_lookup time_zone::lookup(const civil_second& cs) const {
  return effective_impl().MakeTime(cs);
}

bool time_zone::next_transition(const time_point<seconds>& tp,
                                civil_transition* trans) const {
  return effective_impl().NextTransition(tp, trans);
}

bool time_zone::prev_transition(const time_point<seconds>& tp,
                                civil_transition* trans) const {
  return effective_impl().PrevTransition(tp, trans);
}

std::string time_zone::version() const { return effective_impl().Version(); }

std::string time_zone::description() const {
  return effective_impl().Description();
}

const time_zone::Impl& time_zone::effective_impl() const {
  if (impl_ == nullptr) {
    return *time_zone::Impl::UTC().impl_;
  }
  return *impl_;
}

bool load_time_zone(const std::string& name, time_zone* tz) {
  return time_zone::Impl::LoadTimeZone(name, tz);
}

time_zone utc_time_zone() {
  return time_zone::Impl::UTC();  
}

time_zone fixed_time_zone(const seconds& offset) {
  time_zone tz;
  load_time_zone(FixedOffsetToName(offset), &tz);
  return tz;
}

time_zone local_time_zone() {
  const char* zone = ":localtime";
#if defined(__Fuchsia__)
  std::string primary_tz;
  [&]() {

    const zx::duration kTimeout = zx::msec(500);

    async::Loop loop(&kAsyncLoopConfigNeverAttachToThread);

    fuchsia::intl::PropertyProviderHandle handle;
    zx_status_t status = fdio_service_connect_by_name(
        fuchsia::intl::PropertyProvider::Name_,
        handle.NewRequest().TakeChannel().release());
    if (status != ZX_OK) {
      return;
    }

    fuchsia::intl::PropertyProviderPtr intl_provider;
    status = intl_provider.Bind(std::move(handle), loop.dispatcher());
    if (status != ZX_OK) {
      return;
    }

    intl_provider->GetProfile(
        [&loop, &primary_tz](fuchsia::intl::Profile profile) {
          if (!profile.time_zones().empty()) {
            primary_tz = profile.time_zones()[0].id;
          }
          loop.Quit();
        });
    loop.Run(zx::deadline_after(kTimeout));
  }();

  if (!primary_tz.empty()) {
    zone = primary_tz.c_str();
  }
#endif

  char* tz_env = nullptr;
#if defined(_MSC_VER)
  _dupenv_s(&tz_env, nullptr, "TZ");
#else
  tz_env = std::getenv("TZ");
#endif
  if (tz_env) zone = tz_env;

  if (*zone == ':') ++zone;

  char* localtime_env = nullptr;
  if (strcmp(zone, "localtime") == 0) {
#if defined(_MSC_VER)
    _dupenv_s(&localtime_env, nullptr, "LOCALTIME");
#else
    zone = "/etc/localtime";  
    localtime_env = std::getenv("LOCALTIME");
#endif
    if (localtime_env) zone = localtime_env;
  }

  const std::string name = zone;
#if defined(_MSC_VER)
  free(localtime_env);
  free(tz_env);
#endif

  time_zone tz;
  load_time_zone(name, &tz);  
  return tz;
}

}  
}  
ABSL_NAMESPACE_END
}  
