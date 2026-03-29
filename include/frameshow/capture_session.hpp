#pragma once

#include <memory>
#include <string>

#include "frameshow/capture_types.hpp"
#include "frameshow/status.hpp"

namespace frameshow {

class CaptureSession {
 public:
  explicit CaptureSession(CaptureConfig config = {});
  ~CaptureSession();

  CaptureSession(const CaptureSession&) = delete;
  CaptureSession& operator=(const CaptureSession&) = delete;
  CaptureSession(CaptureSession&&) noexcept;
  CaptureSession& operator=(CaptureSession&&) noexcept;

  [[nodiscard]] Status start();
  [[nodiscard]] Status stop();
  [[nodiscard]] Status set_capture_region(CaptureRegion region);
  [[nodiscard]] CaptureRegion snapshot_capture_region() const;
  [[nodiscard]] CaptureFormatInfo snapshot_format() const;
  // Primary hot-path CUDA frame handoff for inference. Only the newest published frame is retained.
  [[nodiscard]] Status try_acquire_latest_inference_frame(InferenceFrameView* out_view);
  [[nodiscard]] Status release_inference_frame(std::uint32_t buffer_index);
  // Best-effort UI/debug tap mirrored from inference. This path is allowed to drop or tear frames.
  [[nodiscard]] Status try_acquire_latest_preview(PreviewFrameView* out_view);
  [[nodiscard]] Status mark_preview_displayed(std::uint32_t buffer_index,
                                              cudaStream_t stream);
  [[nodiscard]] Status release_preview(std::uint32_t buffer_index);
  [[nodiscard]] CaptureStats snapshot_stats() const;
  [[nodiscard]] std::string last_error() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace frameshow
