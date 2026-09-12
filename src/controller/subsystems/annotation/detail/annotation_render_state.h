#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "src/controller/contracts/annotation.h"

namespace mmltk::controller {

// One retained description has exclusive input custody while being filled and
// exclusive renderer custody from submission through GPU settlement.
struct AnnotationRenderState final {
    std::shared_ptr<const contracts::AnnotationSceneContent> scene;
    std::shared_ptr<const std::vector<contracts::AnnotationObjectIdentity>> identities;
    contracts::AnnotationEditorFacts editor{};
    std::uint64_t scene_revision = 0U;
    contracts::AnnotationObject preview{};
    std::optional<std::size_t> preview_object;
    std::uint64_t preview_identity = 0U;
    std::uint64_t generation = 0U;
    std::uint64_t document_epoch = 0U;

    [[nodiscard]] std::size_t ObjectCount() const noexcept {
        return scene->objects.size() + (preview_object == scene->objects.size() ? 1U : 0U);
    }
    [[nodiscard]] const contracts::AnnotationObject& ObjectAt(const std::size_t index) const {
        return preview_object == index ? preview : scene->objects.at(index);
    }
};

}  // namespace mmltk::controller
