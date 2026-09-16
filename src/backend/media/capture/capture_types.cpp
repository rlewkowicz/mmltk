#include "src/backend/media/capture/capture_types.h"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>
namespace mmltk::backend::media::capture {
FilledCaptureSlotLease::FilledCaptureSlotLease(const CaptureSessionIdentity identity, const std::uint32_t slot, const std::uint64_t sequence,
                                               const std::uint8_t* const data, const std::size_t bytes, const std::size_t stride_bytes,
                                               const std::uint32_t pixel_format, const CaptureRegion region, const std::uint64_t capture_ns_in,
                                               const bool short_frame_in) noexcept
    : identity_(identity),
      slot_(slot),
      sequence_(sequence),
      data_(data),
      bytes_(bytes),
      stride_bytes_(stride_bytes),
      pixel_format_(pixel_format),
      region_(region),
      capture_ns_(capture_ns_in),
      short_frame_(short_frame_in) {}
FilledCaptureSlotLease::~FilledCaptureSlotLease() noexcept {
    if (valid()) std::terminate();
}
FilledCaptureSlotLease::FilledCaptureSlotLease(FilledCaptureSlotLease&& other) noexcept { *this = std::move(other); }
FilledCaptureSlotLease& FilledCaptureSlotLease::operator=(FilledCaptureSlotLease&& other) noexcept {
    if (this == &other) return *this;
    if (valid()) std::terminate();
    identity_ = other.identity_;
    slot_ = other.slot_;
    sequence_ = other.sequence_;
    data_ = other.data_;
    bytes_ = other.bytes_;
    stride_bytes_ = other.stride_bytes_;
    pixel_format_ = other.pixel_format_;
    region_ = other.region_;
    capture_ns_ = other.capture_ns_;
    short_frame_ = other.short_frame_;
    other.reset();
    return *this;
}
bool FilledCaptureSlotLease::valid() const noexcept {
    return identity_.valid() && sequence_ != 0U && data_ != nullptr && bytes_ != 0U && stride_bytes_ != 0U && pixel_format_ != 0U && region_.width != 0U &&
           region_.height != 0U && region_.height <= bytes_ / stride_bytes_;
}
void FilledCaptureSlotLease::reset() noexcept {
    identity_ = {};
    slot_ = 0U;
    sequence_ = 0U;
    data_ = nullptr;
    bytes_ = 0U;
    stride_bytes_ = 0U;
    pixel_format_ = 0U;
    region_ = {};
    capture_ns_ = 0U;
    short_frame_ = false;
}
FilledCaptureSlotLease FilledCaptureSlotLeaseAuthority::Create(const CaptureSessionIdentity identity, const std::uint32_t slot, const std::uint64_t sequence,
                                                               const std::uint8_t* const data, const std::size_t bytes, const std::size_t stride_bytes,
                                                               const std::uint32_t pixel_format, const CaptureRegion region, const std::uint64_t capture_ns,
                                                               const bool short_frame) noexcept {
    return FilledCaptureSlotLease{identity, slot, sequence, data, bytes, stride_bytes, pixel_format, region, capture_ns, short_frame};
}
void FilledCaptureSlotLeaseAuthority::Consume(FilledCaptureSlotLease& lease) noexcept { lease.reset(); }
}  // namespace mmltk::backend::media::capture
