#include "src/controller/presentation/visual_document.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mmltk::controller {
namespace {

// Inspect canonical field declarations once at compile time. Nonspatial trees
// (notably potentially large RLE storage) generate no runtime traversal.
template <class T>
consteval bool contains_annotation_point() {
    if constexpr (std::is_same_v<T, contracts::AnnotationPoint>) {
        return true;
    } else if constexpr (std::ranges::range<T>) {
        return contains_annotation_point<std::ranges::range_value_t<T>>();
    } else if constexpr (requires { materialized_field_policies(std::type_identity<T>{}); }) {
        bool found = false;
        mmltk::frameworks::reflection::visit_materialized_members<T>([&]<class Declaration>(const auto&) {
            using Field = std::remove_cvref_t<decltype(std::declval<T&>().*Declaration::pointer)>;
            found = found || contains_annotation_point<Field>();
        });
        return found;
    } else {
        return false;
    }
}

struct PointProjection final {
    float scale = 1.0F;
    VisualRegion crop{};

    void Apply(contracts::AnnotationPoint& point) const {
        point.x *= scale;
        point.y *= scale;
        if (!point.finite()) throw contracts::InvalidIntentError("Annotation coordinate is not finite");
        if (crop.valid()) {
            point.x = std::clamp(point.x - static_cast<float>(crop.x), 0.0F, static_cast<float>(crop.width));
            point.y = std::clamp(point.y - static_cast<float>(crop.y), 0.0F, static_cast<float>(crop.height));
        }
    }
};

template <class T>
void project_spatial_members(T& value, const PointProjection projection) {
    if constexpr (std::is_same_v<T, contracts::AnnotationPoint>) {
        projection.Apply(value);
    } else if constexpr (contains_annotation_point<T>()) {
        if constexpr (std::ranges::range<T>) {
            for (auto& item : value)
                project_spatial_members(item, projection);
        } else {
            mmltk::frameworks::reflection::visit_materialized_members<T>(
                [&]<class Declaration>(const auto&) { project_spatial_members(value.*Declaration::pointer, projection); });
        }
    }
}

}  // namespace

contracts::AnnotationSceneContent materialize_visual_document(const VisualDocument& document, const VisualExtent extent,
                                                              VisualRegion crop) {
    if (!crop.valid()) crop = {.width = extent.width, .height = extent.height};
    if (!extent.valid() || crop.x > extent.width || crop.y > extent.height || crop.width > extent.width - crop.x ||
        crop.height > extent.height - crop.y || crop.width > std::numeric_limits<std::uint16_t>::max() ||
        crop.height > std::numeric_limits<std::uint16_t>::max())
        throw contracts::InvalidIntentError("Annotation crop is outside the image extent");
    auto scene = document.scene;
    scene.frame_width = static_cast<std::uint16_t>(crop.width);
    scene.frame_height = static_cast<std::uint16_t>(crop.height);
    scene.frame_ready = true;
    project_spatial_members(scene.objects, {.crop = crop});
    std::size_t total_runs = 0U;
    for (std::size_t index = 0U; index < scene.objects.size(); ++index) {
        auto& object = scene.objects[index];
        if (!object.mask.present) continue;
        object.mask.runs.clear();
        if (!document.mask_contains) throw contracts::InvalidIntentError("Annotation mask support is unavailable");
        const auto first_x = static_cast<std::uint32_t>(std::floor(object.box.first.x));
        const auto last_x = std::min(crop.width, static_cast<std::uint32_t>(std::ceil(object.box.second.x)));
        const auto first_y = static_cast<std::uint32_t>(std::floor(object.box.first.y));
        const auto last_y = std::min(crop.height, static_cast<std::uint32_t>(std::ceil(object.box.second.y)));
        for (auto y = first_y; y < last_y; ++y) {
            auto x = first_x;
            const auto supported = [&](std::uint32_t px) {
                return document.mask_contains(index, (static_cast<float>(crop.x + px) + 0.5F) / static_cast<float>(extent.width),
                                              (static_cast<float>(crop.y + y) + 0.5F) / static_cast<float>(extent.height));
            };
            while (x < last_x) {
                if (!supported(x)) {
                    ++x;
                    continue;
                }
                const auto first = x++;
                while (x < last_x && supported(x))
                    ++x;
                if (total_runs == contracts::kAnnotationMaskRunCapacity)
                    throw contracts::InvalidIntentError("Annotation import exceeds the document mask-run capacity");
                object.mask.runs.push_back(
                    {static_cast<std::uint16_t>(y), static_cast<std::uint16_t>(first), static_cast<std::uint16_t>(x - 1U)});
                ++total_runs;
            }
        }
    }
    if (!scene.valid()) throw contracts::InvalidIntentError("Annotation import exceeds the document geometry or catalog capacity");
    return scene;
}

std::shared_ptr<const VisualDocument> scale_visual_document(const std::shared_ptr<const VisualDocument>& source,
                                                            const std::uint32_t scale) {
    if (!source || scale == 0U) throw contracts::InvalidIntentError("Visual document scale is invalid");
    auto target = std::make_shared<VisualDocument>(*source);
    project_spatial_members(target->scene.objects, {.scale = static_cast<float>(scale)});
    // The scene is a lightweight semantic projection until import chooses a
    // checked editable extent. Its normalized mask predicate remains valid.
    target->scene.frame_ready = false;
    return target;
}

}  // namespace mmltk::controller
