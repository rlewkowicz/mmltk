export module mmltk.backend.media.live.live_capture_region;
import mmltk.backend.media.capture.capture_types;
export namespace mmltk::backend::media::live {
using LiveCaptureRegion = mmltk::backend::media::capture::CaptureRegion;
[[nodiscard]] constexpr bool valid_region(const LiveCaptureRegion region) noexcept { return region.width != 0U && region.height != 0U; }
}  // namespace mmltk::backend::media::live
