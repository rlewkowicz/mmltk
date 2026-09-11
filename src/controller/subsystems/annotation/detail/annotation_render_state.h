#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "src/controller/contracts/annotation.h"
#include "src/controller/presentation/visual_system_types.h"

namespace mmltk::controller {

// One retained description has exclusive input custody while being filled and
// exclusive renderer custody from submission through GPU settlement.
struct AnnotationRenderState final {
    contracts::AnnotationUiState ui{};
    contracts::AnnotationObject preview{};
    std::optional<std::size_t> preview_object;
    std::uint64_t generation = 0U;
    std::uint64_t document_epoch = 0U;
    VisualRegion crop{};

    [[nodiscard]] std::size_t ObjectCount() const noexcept {
        return ui.scene.objects.size() + (preview_object == ui.scene.objects.size() ? 1U : 0U);
    }
    [[nodiscard]] const contracts::AnnotationObject& ObjectAt(const std::size_t index) const {
        return preview_object == index ? preview : ui.scene.objects.at(index);
    }
};

}  // namespace mmltk::controller
