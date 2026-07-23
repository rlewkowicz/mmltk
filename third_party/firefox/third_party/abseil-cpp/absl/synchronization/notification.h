// Copyright 2017 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_SYNCHRONIZATION_NOTIFICATION_H_
#define ABSL_SYNCHRONIZATION_NOTIFICATION_H_

#include <atomic>

#include "absl/base/config.h"
#include "absl/base/internal/tracing.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

class Notification {
 public:
  Notification() : notified_yet_(false) {}
  explicit Notification(bool prenotify) : notified_yet_(prenotify) {}
  Notification(const Notification&) = delete;
  Notification& operator=(const Notification&) = delete;
  ~Notification();

  [[nodiscard]] bool HasBeenNotified() const {
    if (HasBeenNotifiedInternal(&this->notified_yet_)) {
      base_internal::TraceObserved(this, TraceObjectKind());
      return true;
    }
    return false;
  }

  void WaitForNotification() const;

  bool WaitForNotificationWithTimeout(absl::Duration timeout) const;

  bool WaitForNotificationWithDeadline(absl::Time deadline) const;

  void Notify();

 private:
  static inline constexpr base_internal::ObjectKind TraceObjectKind() {
    return base_internal::ObjectKind::kNotification;
  }

  static inline bool HasBeenNotifiedInternal(
      const std::atomic<bool>* notified_yet) {
    return notified_yet->load(std::memory_order_acquire);
  }

  mutable Mutex mutex_;
  std::atomic<bool> notified_yet_;  
};

ABSL_NAMESPACE_END
}  

#endif  // ABSL_SYNCHRONIZATION_NOTIFICATION_H_
