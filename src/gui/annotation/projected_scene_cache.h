#pragma once

#include "gui/annotation/common.h"
#include "gui/annotation/document/document.h"
#include "gui/annotation/render/renderer.h"
#include "gui/annotation/session.h"

#include <memory>
#include <optional>
#include <utility>

namespace mmltk::gui {

struct AnnotationProjectedSceneCacheInputs {
    const AnnotationDocument* document = nullptr;
    const AnnotationSession* session = nullptr;
    const AnnotationFrame* frame = nullptr;
};

struct AnnotationProjectedSceneCacheLayout {
    std::uint64_t document_generation = 0;
    std::uint64_t frame_id = 0;
    std::optional<mmltk::live::LiveFrameId> live_frame_id;
    std::uint32_t frame_width = 0;
    std::uint32_t frame_height = 0;
    std::uint32_t frame_view_x = 0;
    std::uint32_t frame_view_y = 0;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
};

struct AnnotationProjectedSceneCacheKey : AnnotationProjectedSceneCacheLayout {
    std::optional<std::size_t> selected_object_index;
};

[[nodiscard]] inline std::optional<std::size_t> projected_scene_cache_selected_object_index(
    const AnnotationDocument& document, const AnnotationSession& session) noexcept {
    const auto& selected_object_index = session.selected_object_index();
    if (!selected_object_index.has_value() || *selected_object_index >= document.size()) {
        return std::nullopt;
    }
    return selected_object_index;
}

[[nodiscard]] inline AnnotationProjectedSceneCacheKey make_annotation_projected_scene_cache_key(
    const AnnotationDocument& document, const AnnotationSession& session, const AnnotationFrame& frame) noexcept {
    return AnnotationProjectedSceneCacheKey{
        AnnotationProjectedSceneCacheLayout{
            document.generation(),
            frame.frame_id,
            frame.live_frame_id,
            frame.width,
            frame.height,
            frame.view_x,
            frame.view_y,
            frame.capture_width,
            frame.capture_height,
        },
        projected_scene_cache_selected_object_index(document, session),
    };
}

[[nodiscard]] inline std::optional<AnnotationProjectedSceneCacheKey> make_annotation_projected_scene_cache_key(
    const AnnotationProjectedSceneCacheInputs& inputs) noexcept {
    if (inputs.document == nullptr || inputs.session == nullptr || inputs.frame == nullptr) {
        return std::nullopt;
    }
    return make_annotation_projected_scene_cache_key(*inputs.document, *inputs.session, *inputs.frame);
}

[[nodiscard]] inline std::shared_ptr<const AnnotationProjectedScene> refresh_annotation_projected_scene(
    const AnnotationFrame& frame, const AnnotationDocument& document,
    std::shared_ptr<const AnnotationProjectedScene> projected_scene,
    const std::optional<std::size_t> selected_object_index) {
    if (projected_scene == nullptr || projected_scene->document_generation != document.generation()) {
        return std::make_shared<AnnotationProjectedScene>(
            AnnotationRenderer::build_projected_scene(frame, document, selected_object_index));
    }
    if (projected_scene->selected_object_index != selected_object_index) {
        return std::make_shared<AnnotationProjectedScene>(AnnotationRenderer::refresh_projected_scene_selection(
            frame, document, *projected_scene, selected_object_index));
    }
    return projected_scene;
}

class AnnotationProjectedSceneCache {
   public:
    [[nodiscard]] const std::shared_ptr<const AnnotationProjectedScene>& scene() const noexcept {
        return state_.scene;
    }

    void invalidate() noexcept {
        state_ = {};
    }

    [[nodiscard]] bool matches(const AnnotationProjectedSceneCacheKey& key) const noexcept {
        return state_.scene != nullptr && state_matches_key(key);
    }

    [[nodiscard]] bool matches(const AnnotationProjectedSceneCacheInputs& inputs) const noexcept {
        const std::optional<AnnotationProjectedSceneCacheKey> key = make_annotation_projected_scene_cache_key(inputs);
        return key.has_value() && matches(*key);
    }

    void update(std::shared_ptr<const AnnotationProjectedScene> scene,
                const AnnotationProjectedSceneCacheKey& key) noexcept {
        if (scene == nullptr) {
            invalidate();
            return;
        }

        store_scene(std::move(scene), key);
    }

    void update(std::shared_ptr<const AnnotationProjectedScene> scene,
                const AnnotationProjectedSceneCacheInputs& inputs) noexcept {
        const std::optional<AnnotationProjectedSceneCacheKey> key = make_annotation_projected_scene_cache_key(inputs);
        if (!key.has_value()) {
            invalidate();
            return;
        }
        update(std::move(scene), *key);
    }

    [[nodiscard]] bool can_refresh_selection(const AnnotationProjectedSceneCacheKey& key) const noexcept {
        return state_.scene != nullptr && state_.selected_object_index != key.selected_object_index &&
               state_matches_layout(key);
    }

    [[nodiscard]] std::shared_ptr<const AnnotationProjectedScene> resolve(
        const AnnotationProjectedSceneCacheInputs& inputs) {
        const std::optional<AnnotationProjectedSceneCacheKey> key = make_annotation_projected_scene_cache_key(inputs);
        if (!key.has_value()) {
            invalidate();
            return {};
        }

        if (matches(*key)) {
            return state_.scene;
        }

        if (can_refresh_selection(*key) && inputs.document != nullptr && inputs.frame != nullptr) {
            update(std::make_shared<AnnotationProjectedScene>(AnnotationRenderer::refresh_projected_scene_selection(
                       *inputs.frame, *inputs.document, *state_.scene, key->selected_object_index)),
                   *key);
            return state_.scene;
        }

        if (inputs.document == nullptr || inputs.frame == nullptr) {
            invalidate();
            return {};
        }

        update(std::make_shared<AnnotationProjectedScene>(AnnotationRenderer::build_projected_scene(
                   *inputs.frame, *inputs.document, key->selected_object_index)),
               *key);
        return state_.scene;
    }

   private:
    struct State : AnnotationProjectedSceneCacheLayout {
        std::shared_ptr<const AnnotationProjectedScene> scene;
        std::optional<std::size_t> selected_object_index;
    };

    void store_scene(std::shared_ptr<const AnnotationProjectedScene> scene,
                     const AnnotationProjectedSceneCacheKey& key) noexcept {
        state_.scene = std::move(scene);
        state_.document_generation = key.document_generation;
        state_.selected_object_index = key.selected_object_index;
        state_.frame_id = key.frame_id;
        state_.live_frame_id = key.live_frame_id;
        state_.frame_width = key.frame_width;
        state_.frame_height = key.frame_height;
        state_.frame_view_x = key.frame_view_x;
        state_.frame_view_y = key.frame_view_y;
        state_.capture_width = key.capture_width;
        state_.capture_height = key.capture_height;
    }

    [[nodiscard]] bool state_matches_key(const AnnotationProjectedSceneCacheKey& key) const noexcept {
        return state_matches_layout(key) && state_.selected_object_index == key.selected_object_index;
    }

    [[nodiscard]] bool state_matches_layout(const AnnotationProjectedSceneCacheKey& key) const noexcept {
        return state_.document_generation == key.document_generation && state_.frame_id == key.frame_id &&
               state_.live_frame_id == key.live_frame_id && state_.frame_width == key.frame_width &&
               state_.frame_height == key.frame_height && state_.frame_view_x == key.frame_view_x &&
               state_.frame_view_y == key.frame_view_y && state_.capture_width == key.capture_width &&
               state_.capture_height == key.capture_height;
    }

    State state_{};
};

}  // namespace mmltk::gui
