#include "src/backend/media/live/manual_overlay_document.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
namespace mmltk::backend::media::live {
bool manual_overlay_mask_region_contained(const ManualOverlayMaskRegion& region, const std::uint32_t target_width, const std::uint32_t target_height) noexcept {
 return region.width > 0U && region.height > 0U && target_width > 0U && target_height > 0U && static_cast<std::uint64_t>(region.capture_x) + region.width <= target_width &&
        static_cast<std::uint64_t>(region.capture_y) + region.height <= target_height;
}
std::optional<ManualOverlayDeferredMaskProjection> validate_manual_overlay_deferred_mask(const ManualOverlayDeferredMaskMapping& mapping, const ManualOverlayMaskRegion& region,
 const std::span<const ManualOverlayMaskRun> runs, const std::uint32_t target_width, const std::uint32_t target_height) noexcept {
 constexpr std::uint32_t kIntMaximum = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
 if (mapping.source_width == 0U || mapping.source_height == 0U || mapping.source_crop_width == 0U || mapping.source_crop_height == 0U || mapping.output_width == 0U || mapping.output_height == 0U ||
     region.width == 0U || region.height == 0U || target_width == 0U || target_height == 0U || runs.empty() || mapping.source_width > kIntMaximum || mapping.source_height > kIntMaximum ||
     mapping.source_crop_width > kIntMaximum || mapping.source_crop_height > kIntMaximum || mapping.output_width > kIntMaximum || mapping.output_height > kIntMaximum || region.width > kIntMaximum ||
     region.height > kIntMaximum || runs.size() > std::numeric_limits<std::uint32_t>::max() || runs.size() > std::numeric_limits<std::size_t>::max() / 2U)
  return std::nullopt;
 const auto contained = [](const std::uint32_t origin, const std::uint32_t extent, const std::uint32_t bound) noexcept { return static_cast<std::uint64_t>(origin) + extent <= bound; };
 if (!contained(mapping.source_crop_x, mapping.source_crop_width, mapping.source_width) || !contained(mapping.source_crop_y, mapping.source_crop_height, mapping.source_height) ||
     !contained(mapping.view_x, region.capture_x, mapping.output_width) || !contained(mapping.view_y, region.capture_y, mapping.output_height) ||
     static_cast<std::uint64_t>(mapping.view_x) + region.capture_x + region.width > mapping.output_width ||
     static_cast<std::uint64_t>(mapping.view_y) + region.capture_y + region.height > mapping.output_height || !manual_overlay_mask_region_contained(region, target_width, target_height))
  return std::nullopt;
 const std::uint64_t source_pixels = static_cast<std::uint64_t>(mapping.source_width) * mapping.source_height;
 std::uint64_t previous_end = 0U;
 for (const ManualOverlayMaskRun& run : runs) {
  const std::uint64_t end = static_cast<std::uint64_t>(run.offset) + run.length;
  if (run.length == 0U || run.offset < previous_end || end > source_pixels) return std::nullopt;
  previous_end = end;
 }
 return ManualOverlayDeferredMaskProjection{mapping, region, static_cast<std::uint32_t>(runs.size()), runs.size() * 2U};
}
ManualOverlayDocument::ManualOverlayDocument() : snapshot_(std::make_shared<ManualOverlayDocumentSnapshot>()) {}
void ManualOverlayDocument::publish_snapshot(ManualOverlayDocumentSnapshot snapshot) { publish_snapshot(std::make_shared<ManualOverlayDocumentSnapshot>(std::move(snapshot))); }
void ManualOverlayDocument::publish_snapshot(std::shared_ptr<ManualOverlayDocumentSnapshot> snapshot) {
 if (snapshot == nullptr) { throw std::invalid_argument("manual overlay publication requires an immutable snapshot"); }
 const std::shared_ptr<const ManualOverlayDocumentSnapshot> current = snapshot_.load(std::memory_order_acquire);
 if (current != nullptr && snapshot->same_content(*current)) { return; }
 snapshot->generation = current ? current->generation + 1U : 1U;
 std::shared_ptr<const ManualOverlayDocumentSnapshot> immutable = std::move(snapshot);
 snapshot_.store(std::move(immutable), std::memory_order_release);
}
void ManualOverlayDocument::clear(const std::uint32_t capture_width, const std::uint32_t capture_height) {
 ManualOverlayDocumentSnapshot snapshot;
 snapshot.capture_width = capture_width;
 snapshot.capture_height = capture_height;
 publish_snapshot(std::move(snapshot));
}
std::shared_ptr<const ManualOverlayDocumentSnapshot> ManualOverlayDocument::snapshot() const { return snapshot_.load(std::memory_order_acquire); }
std::shared_ptr<const ManualOverlayDocumentSnapshot> ManualOverlayDocument::snapshot_if_changed(const std::uint64_t last_seen_generation) const {
 std::shared_ptr<const ManualOverlayDocumentSnapshot> current = snapshot_.load(std::memory_order_acquire);
 if (!current || current->generation == last_seen_generation) { return {}; }
 return current;
}
}  // namespace mmltk::backend::media::live
