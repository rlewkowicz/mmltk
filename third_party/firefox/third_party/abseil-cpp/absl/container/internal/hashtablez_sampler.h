// Copyright 2018 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0

#ifndef ABSL_CONTAINER_INTERNAL_HASHTABLEZ_SAMPLER_H_
#define ABSL_CONTAINER_INTERNAL_HASHTABLEZ_SAMPLER_H_

#include <cstddef>
#include <cstdint>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace container_internal {

class HashtablezInfoHandle {
 public:
  explicit HashtablezInfoHandle() = default;
  explicit HashtablezInfoHandle(std::nullptr_t) {}

  void Unregister() {}
  bool IsSampled() const { return false; }
  void RecordStorageChanged(size_t, size_t) {}
  void RecordRehash(size_t) {}
  void RecordReservation(size_t) {}
  void RecordClearedReservation() {}
  void RecordInsertMiss(size_t, size_t) {}
  void RecordErase() {}

  friend void swap(HashtablezInfoHandle&, HashtablezInfoHandle&) {}
};

inline bool ShouldSampleNextTable() { return false; }

inline HashtablezInfoHandle ForcedTrySample(size_t, size_t, size_t,
                                             uint16_t) {
  return HashtablezInfoHandle(nullptr);
}

inline HashtablezInfoHandle Sample(size_t, size_t, size_t, uint16_t) {
  return HashtablezInfoHandle(nullptr);
}

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_CONTAINER_INTERNAL_HASHTABLEZ_SAMPLER_H_
