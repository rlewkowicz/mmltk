#pragma once

#include "mmltk/live/live_capture_region.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace mmltk::live {

struct UiCropSnapshot {
    bool has_crop = false;
    LiveCaptureRegion region{};
    std::uint64_t generation = 0;
};

class UiCropState {
   public:
    UiCropState() : snapshot_(std::make_shared<UiCropSnapshot>()) {}

    void set(const LiveCaptureRegion& region) {
        publish(true, region);
    }

    void clear() {
        publish(false, LiveCaptureRegion{});
    }

    [[nodiscard]] std::shared_ptr<const UiCropSnapshot> snapshot() const {
        return snapshot_.load(std::memory_order_acquire);
    }

   private:
    void publish(bool has_crop, const LiveCaptureRegion& region) {
        const std::shared_ptr<const UiCropSnapshot> current = snapshot_.load(std::memory_order_acquire);

        auto next = std::make_shared<UiCropSnapshot>();
        next->has_crop = has_crop;
        next->region = region;
        next->generation = current ? current->generation + 1U : 1U;

        std::shared_ptr<const UiCropSnapshot> immutable = next;
        snapshot_.store(std::move(immutable), std::memory_order_release);
    }

    std::atomic<std::shared_ptr<const UiCropSnapshot>> snapshot_;
};

struct RuntimeCropState {
    bool has_crop = false;
    LiveCaptureRegion region{};
    std::uint64_t last_ui_generation = 0;

    void sync_from(const UiCropState& ui_state) {
        const std::shared_ptr<const UiCropSnapshot> current = ui_state.snapshot();
        if (!current || current->generation == last_ui_generation) {
            return;
        }

        has_crop = current->has_crop;
        region = current->region;
        last_ui_generation = current->generation;
    }

    [[nodiscard]] UiCropSnapshot snapshot() const {
        return UiCropSnapshot{has_crop, region, last_ui_generation};
    }
};

}  
