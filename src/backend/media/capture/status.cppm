module;
#include <cstdint>
#include <string>

export module mmltk.backend.media.capture.status;

export namespace mmltk::backend::media::capture {

enum class StatusCode : std::uint8_t {
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

}  // namespace mmltk::backend::media::capture
