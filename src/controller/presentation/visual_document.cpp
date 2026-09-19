#include "src/controller/presentation/visual_document.h"
#include <algorithm>
#include "src/backend/imaging/resample/image_resize.h"
#include <atomic>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include "src/controller/contracts/application_boundary.h"
namespace mmltk::controller {
std::uint64_t VisualDocument::NextIdentity() {
    static std::atomic_uint64_t next{1U};
    const auto identity = next.fetch_add(1U, std::memory_order_relaxed);
    if (identity == 0U || identity == std::numeric_limits<std::uint64_t>::max()) std::terminate();
    return identity;
}
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
    VisualExtent target{};
    void Apply(contracts::AnnotationPoint& point) const {
        point.x *= scale;
        point.y *= scale;
        if (!point.finite()) throw contracts::InvalidIntentError("Annotation coordinate is not finite");
        if (crop.valid()) {
            point.x = std::clamp(point.x - static_cast<float>(crop.x), 0.0F, static_cast<float>(crop.width));
            point.y = std::clamp(point.y - static_cast<float>(crop.y), 0.0F, static_cast<float>(crop.height));
            if (target.valid()) {
                point.x *= static_cast<float>(target.width) / static_cast<float>(crop.width);
                point.y *= static_cast<float>(target.height) / static_cast<float>(crop.height);
            }
        }
    }
};
template <class T>
void project_spatial_members(T& value, const PointProjection projection) {
    if constexpr (std::is_same_v<T, contracts::AnnotationPoint>) {
        projection.Apply(value);
    } else if constexpr (contains_annotation_point<T>()) {
        if constexpr (std::ranges::range<T>) {
            for (auto& item : value) project_spatial_members(item, projection);
        } else {
            mmltk::frameworks::reflection::visit_materialized_members<T>(
                [&]<class Declaration>(const auto&) { project_spatial_members(value.*Declaration::pointer, projection); });
        }
    }
}
}  // namespace
VisualExtent visual_materialized_extent(const VisualFrame& frame, const bool original) {
    if (!original) return frame.extent;
    const auto crop = frame.content.valid() ? frame.content : VisualRegion{0U, 0U, frame.extent.width, frame.extent.height};
    if (!frame.source_extent.valid()) return {crop.width, crop.height};
    const auto geometry = mmltk::backend::imaging::resample::compute_image_resize_geometry(
        frame.source_extent.width, frame.source_extent.height, crop.width, crop.height, mmltk::backend::imaging::resample::ImageResizeMode::Letterbox);
    return {geometry.resized_width, geometry.resized_height};
}
contracts::AnnotationSceneContent materialize_visual_document(const VisualDocument& document, const VisualExtent extent, VisualRegion crop,
                                                              VisualExtent target) {
    if (!crop.valid()) crop = {.width = extent.width, .height = extent.height};
    if (!extent.valid() || crop.x > extent.width || crop.y > extent.height || crop.width > extent.width - crop.x || crop.height > extent.height - crop.y ||
        crop.width > std::numeric_limits<std::uint16_t>::max() || crop.height > std::numeric_limits<std::uint16_t>::max())
        throw contracts::InvalidIntentError("Annotation crop is outside the image extent");
    if (!target.valid()) target = {crop.width, crop.height};
    if (target.width > std::numeric_limits<std::uint16_t>::max() || target.height > std::numeric_limits<std::uint16_t>::max())
        throw contracts::InvalidIntentError("Annotation target exceeds the document extent");
    auto scene = document.scene;
    scene.frame_width = static_cast<std::uint16_t>(target.width);
    scene.frame_height = static_cast<std::uint16_t>(target.height);
    scene.frame_ready = true;
    project_spatial_members(scene.objects, {.crop = crop, .target = target});
    std::size_t total_runs = 0U;
    for (std::size_t index = 0U; index < scene.objects.size(); ++index) {
        auto& object = scene.objects[index];
        if (!object.mask.present) continue;
        object.mask.runs.clear();
        if (index >= document.mask_bounds.size()) throw contracts::InvalidIntentError("Annotation mask bounds are unavailable");
        const auto& bounds = document.mask_bounds[index];
        if (!std::ranges::all_of(bounds, [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; }) || bounds[0] > bounds[2] ||
            bounds[1] > bounds[3])
            throw contracts::InvalidIntentError("Annotation mask bounds are invalid");
        if (bounds[0] == bounds[2] || bounds[1] == bounds[3]) continue;
        if (!document.mask_contains) throw contracts::InvalidIntentError("Annotation mask support is unavailable");
        const auto coordinate = [](std::uint32_t pixel, std::uint32_t offset, std::uint32_t size, std::uint32_t output, std::uint32_t full) {
            return (static_cast<float>(offset) + std::floor(static_cast<float>(pixel) * static_cast<float>(size) / static_cast<float>(output)) + 0.5F) /
                   static_cast<float>(full);
        };
        // Binary search the actual nearest-sampling coordinates. This includes
        // ties at closed support edges and avoids inverse-rounding omissions.
        const auto interval = [&](float low, float high, std::uint32_t offset, std::uint32_t size, std::uint32_t output, std::uint32_t full) {
            const auto pixels = std::views::iota(0U, output);
            const auto first = std::ranges::lower_bound(pixels, low, {}, [&](auto pixel) { return coordinate(pixel, offset, size, output, full); });
            const auto last = std::ranges::upper_bound(pixels, high, {}, [&](auto pixel) { return coordinate(pixel, offset, size, output, full); });
            return std::pair{static_cast<std::uint32_t>(first - pixels.begin()), static_cast<std::uint32_t>(last - pixels.begin())};
        };
        const auto [first_x, last_x] = interval(bounds[0], bounds[2], crop.x, crop.width, target.width, extent.width);
        const auto [first_y, last_y] = interval(bounds[1], bounds[3], crop.y, crop.height, target.height, extent.height);
        for (auto y = first_y; y < last_y; ++y) {
            const auto normalized_y = coordinate(y, crop.y, crop.height, target.height, extent.height);
            auto run_start = last_x;
            const auto append = [&](std::uint32_t end) {
                if (total_runs == contracts::kAnnotationMaskRunCapacity)
                    throw contracts::InvalidIntentError("Annotation import exceeds the document mask-run capacity");
                object.mask.runs.push_back({static_cast<std::uint16_t>(y), static_cast<std::uint16_t>(run_start), static_cast<std::uint16_t>(end - 1U)});
                ++total_runs;
            };
            for (auto x = first_x; x < last_x; ++x) {
                const bool supported = document.mask_contains(index, coordinate(x, crop.x, crop.width, target.width, extent.width), normalized_y);
                if (supported && run_start == last_x) run_start = x;
                if (!supported && run_start != last_x) {
                    append(x);
                    run_start = last_x;
                }
            }
            if (run_start != last_x) append(last_x);
        }
    }
    if (!scene.valid()) throw contracts::InvalidIntentError("Annotation import exceeds the document geometry or catalog capacity");
    return scene;
}
std::shared_ptr<const VisualDocument> scale_visual_document(const std::shared_ptr<const VisualDocument>& source, const std::uint32_t scale) {
    if (!source || scale == 0U) throw contracts::InvalidIntentError("Visual document scale is invalid");
    auto target = std::make_shared<VisualDocument>(*source);
    project_spatial_members(target->scene.objects, {.scale = static_cast<float>(scale)});
    // The scene is a lightweight semantic projection until import chooses a
    // checked editable extent. Its normalized mask predicate remains valid.
    target->scene.frame_ready = false;
    return target;
}
}  // namespace mmltk::controller
