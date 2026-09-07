// Copyright 2023 Google Inc.  All rights reserved.
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#if !defined(GOOGLE_PROTOBUF_REFLECTION_MODE_H__)
#define GOOGLE_PROTOBUF_REFLECTION_MODE_H__

#include <cstddef>

#include "google/protobuf/port_def.inc"

namespace google {
namespace protobuf {
namespace internal {

enum class ReflectionMode {
  kDefault,
  kDebugString,
  kDiagnostics,
};

ReflectionMode GetReflectionMode();

class PROTOBUF_EXPORT ScopedReflectionMode final {
 public:
  explicit ScopedReflectionMode(ReflectionMode mode);

  ~ScopedReflectionMode();

  static ReflectionMode current_reflection_mode();

  ScopedReflectionMode(const ScopedReflectionMode&) = delete;
  ScopedReflectionMode& operator=(const ScopedReflectionMode&) = delete;

 private:
#if !defined(PROTOBUF_NO_THREADLOCAL)
  const ReflectionMode previous_mode_;
  PROTOBUF_CONSTINIT static PROTOBUF_THREAD_LOCAL ReflectionMode
      reflection_mode_;
#endif
};

#if !defined(PROTOBUF_NO_THREADLOCAL)


inline ScopedReflectionMode::ScopedReflectionMode(ReflectionMode mode)
    : previous_mode_(reflection_mode_) {
  if (mode > reflection_mode_) {
    reflection_mode_ = mode;
  }
}

inline ScopedReflectionMode::~ScopedReflectionMode() {
  reflection_mode_ = previous_mode_;
}

inline ReflectionMode ScopedReflectionMode::current_reflection_mode() {
  return reflection_mode_;
}


#else

inline ScopedReflectionMode::ScopedReflectionMode(ReflectionMode mode) {}
inline ScopedReflectionMode::~ScopedReflectionMode() {}
inline ReflectionMode ScopedReflectionMode::current_reflection_mode() {
  return ReflectionMode::kDefault;
}

#endif

inline ReflectionMode GetReflectionMode() {
  return ScopedReflectionMode::current_reflection_mode();
}

}  
}  
}  

#include "google/protobuf/port_undef.inc"

#endif
