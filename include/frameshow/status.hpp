#pragma once

#include <string>

namespace frameshow {

enum class StatusCode {
  kOk = 0,
  kNotReady,
  kInvalidArgument,
  kAlreadyRunning,
  kNotRunning,
  kNoDevice,
  kCudaError,
  kUnsupported,
  kInternalError,
};

struct Status {
  StatusCode code = StatusCode::kOk;
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == StatusCode::kOk; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  static Status Ok() { return {}; }
};

}  // namespace frameshow
