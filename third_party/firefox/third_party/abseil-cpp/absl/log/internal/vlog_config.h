// Copyright 2022 The Abseil Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_LOG_INTERNAL_VLOG_CONFIG_H_
#define ABSL_LOG_INTERNAL_VLOG_CONFIG_H_

// IWYU pragma: private, include "absl/log/log.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/nullability.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/strings/string_view.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace log_internal {

class SyntheticBinary;
class VLogSite;

int RegisterAndInitialize(VLogSite* absl_nonnull v);
void UpdateVLogSites();

class VLogSite final {
 public:
  explicit constexpr VLogSite(const char* absl_nonnull f)
      : file_(f), v_(kUninitialized), next_(nullptr) {}
  VLogSite(const VLogSite&) = delete;
  VLogSite& operator=(const VLogSite&) = delete;

  ABSL_ATTRIBUTE_ALWAYS_INLINE
  bool IsEnabled(int level) {
    int stale_v = v_.load(std::memory_order_relaxed);
    if (ABSL_PREDICT_TRUE(level > stale_v)) {
      return false;
    }

#if ABSL_HAVE_BUILTIN(__builtin_constant_p) || defined(__GNUC__)
    if (__builtin_constant_p(level)) {
      if (level == 0) return SlowIsEnabled0(stale_v);
      if (level == 1) return SlowIsEnabled1(stale_v);
      if (level == 2) return SlowIsEnabled2(stale_v);
      if (level == 3) return SlowIsEnabled3(stale_v);
      if (level == 4) return SlowIsEnabled4(stale_v);
      if (level == 5) return SlowIsEnabled5(stale_v);
    }
#endif
    return SlowIsEnabled(stale_v, level);
  }

 private:
  friend int log_internal::RegisterAndInitialize(VLogSite* absl_nonnull v);
  friend void log_internal::UpdateVLogSites();
  friend class log_internal::SyntheticBinary;
  static constexpr int kUninitialized = (std::numeric_limits<int>::max)();

  ABSL_ATTRIBUTE_NOINLINE
  bool SlowIsEnabled(int stale_v, int level);
  ABSL_ATTRIBUTE_NOINLINE bool SlowIsEnabled0(int stale_v);
  ABSL_ATTRIBUTE_NOINLINE bool SlowIsEnabled1(int stale_v);
  ABSL_ATTRIBUTE_NOINLINE bool SlowIsEnabled2(int stale_v);
  ABSL_ATTRIBUTE_NOINLINE bool SlowIsEnabled3(int stale_v);
  ABSL_ATTRIBUTE_NOINLINE bool SlowIsEnabled4(int stale_v);
  ABSL_ATTRIBUTE_NOINLINE bool SlowIsEnabled5(int stale_v);

  const char* absl_nonnull const file_;
  std::atomic<int> v_;
  std::atomic<VLogSite*> next_;
};
static_assert(std::is_trivially_destructible_v<VLogSite>,
              "VLogSite must be trivially destructible");

int VLogLevel(absl::string_view file);

int RegisterAndInitialize(VLogSite* absl_nonnull v);

void UpdateVLogSites();

void UpdateVModule(absl::string_view vmodule);

int UpdateGlobalVLogLevel(int v);

int PrependVModule(absl::string_view module_pattern, int log_level);

void OnVLogVerbosityUpdate(std::function<void()> cb);

VLogSite* absl_nullable SetVModuleListHeadForTestOnly(
    VLogSite* absl_nullable v);

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_LOG_INTERNAL_VLOG_CONFIG_H_
