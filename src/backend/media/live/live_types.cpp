#include "src/backend/media/live/live_types.h"
#include <limits>
#include <utility>
namespace mmltk::backend::media::live {
bool LiveManualOverlayUploadLimits::valid() const noexcept {
 return mask_bytes != 0U && run_values != 0U && point_values != 0U && edge_values != 0U && brush_values >= kManualOverlayBrushValueCount &&
        run_values <= std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t) && point_values <= std::numeric_limits<std::size_t>::max() / sizeof(int) &&
        edge_values <= std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t) && brush_values <= std::numeric_limits<std::size_t>::max() / sizeof(int);
}
LiveCompositeOutputLease::LiveCompositeOutputLease(
 void* owner, const CompleteCallback complete, const AbandonCallback abandon, LiveOutputFrame view, const PhysicalFrameRevision frame_revision) noexcept
    : owner_(owner), complete_(complete), abandon_(abandon), view_(view), frame_revision_(frame_revision) {}
LiveCompositeOutputLease LiveCompositeOutputLease::Create(
 void* owner, const CompleteCallback complete, const AbandonCallback abandon, LiveOutputFrame view, const PhysicalFrameRevision frame_revision) noexcept {
 return LiveCompositeOutputLease{owner, complete, abandon, view, frame_revision};
}
LiveCompositeOutputLease::~LiveCompositeOutputLease() noexcept { Abandon(); }
LiveCompositeOutputLease::LiveCompositeOutputLease(LiveCompositeOutputLease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      complete_(std::exchange(other.complete_, nullptr)),
      abandon_(std::exchange(other.abandon_, nullptr)),
      view_(std::exchange(other.view_, {})),
      frame_revision_(std::exchange(other.frame_revision_, {})) {}
LiveCompositeOutputLease& LiveCompositeOutputLease::operator=(LiveCompositeOutputLease&& other) noexcept {
 if (this == &other) return *this;
 Abandon();
 owner_ = std::exchange(other.owner_, nullptr);
 complete_ = std::exchange(other.complete_, nullptr);
 abandon_ = std::exchange(other.abandon_, nullptr);
 view_ = std::exchange(other.view_, {});
 frame_revision_ = std::exchange(other.frame_revision_, {});
 return *this;
}
void LiveCompositeOutputLease::Complete() && noexcept {
 if (owner_ == nullptr || complete_ == nullptr) return;
 complete_(owner_, frame_revision_);
 Clear();
}
void LiveCompositeOutputLease::Abandon() noexcept {
 if (owner_ != nullptr && abandon_ != nullptr) abandon_(owner_, frame_revision_);
 Clear();
}
void LiveCompositeOutputLease::Clear() noexcept {
 owner_ = nullptr;
 complete_ = nullptr;
 abandon_ = nullptr;
 view_ = {};
 frame_revision_ = {};
}
bool try_acquire_live_output(void* source, const PhysicalFrameRevision revision, const LiveOutputAcquire acquire, LiveCompositeOutputLease* output) {
 return source != nullptr && revision.valid() && acquire != nullptr && output != nullptr && !static_cast<bool>(*output) && acquire(source, revision, output);
}
void LiveRevisionWait::Reset() noexcept {
 std::scoped_lock lock(mutex_);
 failed_ = false;
 notification_generation_ = 0U;
 last_revision_ = 0U;
}
void LiveRevisionWait::RevisionReady() noexcept {
 {
  std::scoped_lock lock(mutex_);
  if (notification_generation_ == std::numeric_limits<std::uint64_t>::max()) {
   failed_ = true;
  } else {
   ++notification_generation_;
  }
 }
 ready_.notify_one();
}
void LiveRevisionWait::Fail() noexcept {
 {
  std::scoped_lock lock(mutex_);
  failed_ = true;
 }
 ready_.notify_all();
}
bool LiveRevisionWait::failed() const noexcept {
 std::scoped_lock lock(mutex_);
 return failed_;
}
std::optional<PhysicalFrameRevision> LiveRevisionWait::Wait(const std::stop_token stop, void* source, const Snapshot snapshot) {
 std::unique_lock lock(mutex_);
 if (failed_ || stop.stop_requested()) return std::nullopt;
 const std::uint64_t checked_generation = notification_generation_;
 auto observed = source != nullptr && snapshot != nullptr ? snapshot(source) : std::nullopt;
 if (!observed || !observed->valid() || observed->revision <= last_revision_) {
  ready_.wait(lock, stop, [this, checked_generation] { return failed_ || notification_generation_ != checked_generation; });
  if (failed_ || stop.stop_requested()) return std::nullopt;
  observed = source != nullptr && snapshot != nullptr ? snapshot(source) : std::nullopt;
 }
 if (!observed || !observed->valid() || observed->revision <= last_revision_) return std::nullopt;
 last_revision_ = observed->revision;
 return observed;
}
}  // namespace mmltk::backend::media::live
