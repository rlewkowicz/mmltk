#include "mmltk/live/manual_overlay_document.h"

#include "manual_overlay_compare_utils.h"

#include <utility>

namespace mmltk::live {

ManualOverlayDocument::ManualOverlayDocument()
    : snapshot_(std::make_shared<ManualOverlayDocumentSnapshot>()) {}

void ManualOverlayDocument::publish_snapshot(ManualOverlayDocumentSnapshot snapshot) {
    const std::shared_ptr<const ManualOverlayDocumentSnapshot> current =
        std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
    if (current != nullptr && mmltk::overlay_compare::manual_snapshot_equals(snapshot, *current)) {
        return;
    }
    snapshot.generation = current ? current->generation + 1U : 1U;

    auto next = std::make_shared<ManualOverlayDocumentSnapshot>(std::move(snapshot));
    std::shared_ptr<const ManualOverlayDocumentSnapshot> immutable = std::move(next);
    std::atomic_store_explicit(&snapshot_, std::move(immutable), std::memory_order_release);
}

void ManualOverlayDocument::clear(const std::uint32_t capture_width,
                                  const std::uint32_t capture_height) {
    ManualOverlayDocumentSnapshot snapshot;
    snapshot.capture_width = capture_width;
    snapshot.capture_height = capture_height;
    publish_snapshot(std::move(snapshot));
}

std::shared_ptr<const ManualOverlayDocumentSnapshot> ManualOverlayDocument::snapshot() const {
    return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
}

std::shared_ptr<const ManualOverlayDocumentSnapshot> ManualOverlayDocument::snapshot_if_changed(
    const std::uint64_t last_seen_generation) const {
    std::shared_ptr<const ManualOverlayDocumentSnapshot> current =
        std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
    if (!current || current->generation == last_seen_generation) {
        return {};
    }
    return current;
}

} // namespace mmltk::live
