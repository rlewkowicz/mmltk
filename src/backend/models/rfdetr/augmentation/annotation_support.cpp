#include "annotation_support.h"
#include "spatial_erasure.h"
#include "src/backend/imaging/sampling.h"
#include <cmath>
#include <stdexcept>
namespace mmltk::backend::models::rfdetr {
namespace {
constexpr std::array<float, 6> identity{1, 0, 0, 0, 1, 0};
AugmentationAnnotationSupport mask_extent(std::span<const mmltk::backend::data::RLEPair> runs, int width, int height) {
    AugmentationAnnotationSupport support{{1, 1, 0, 0}, 0, true};
    const float image_width = static_cast<float>(width), image_height = static_cast<float>(height);
    std::uint64_t end = 0;
    for (const auto& run : runs) {
        if (run.length == 0 || run.start < end || static_cast<std::uint64_t>(run.start) + run.length > static_cast<std::uint64_t>(width) * height)
            throw std::runtime_error("mask_rle run is empty, overlaps a previous run, or exceeds compiled image bounds");
        end = static_cast<std::uint64_t>(run.start) + run.length;
        support.area_pixels += static_cast<float>(run.length);
        const auto first_y = static_cast<std::uint64_t>(run.start) / width;
        const auto last_y = (end - 1) / width;
        auto& bounds = support.box_xyxy;
        bounds[0] = std::min(bounds[0], first_y == last_y ? static_cast<float>(run.start % width) / image_width : 0.0F);
        bounds[2] = std::max(bounds[2], first_y == last_y ? static_cast<float>((end - 1) % width + 1) / image_width : 1.0F);
        bounds[1] = std::min(bounds[1], static_cast<float>(first_y) / image_height);
        bounds[3] = std::max(bounds[3], static_cast<float>(last_y + 1) / image_height);
    }
    return support;
}
bool contains(const std::array<float, 4>& box, std::span<const mmltk::backend::data::RLEPair> runs, const std::array<float, 6>& inverse, float x, float y,
              int width, int height) {
    // CLEANUP-IGNORE: Inverse support sampling and forward box-corner projection intentionally express opposite coordinate mappings.
    const float sx = inverse[0] * x + inverse[1] * y + inverse[2];
    const float sy = inverse[3] * x + inverse[4] * y + inverse[5];
    if (sx < 0 || sx > 1 || sy < 0 || sy > 1) return false;
    if (runs.empty()) return sx >= box[0] && sx <= box[2] && sy >= box[1] && sy <= box[3];
    const auto pixel =
        mmltk::backend::imaging::sampling::support_pixel_index(sy, height) * width + mmltk::backend::imaging::sampling::support_pixel_index(sx, width);
    return mmltk::backend::imaging::sampling::rle_support_contains(runs.data(), runs.size(), pixel);
}
}  // namespace
bool augmentation_changes_support(const AugmentationImagePlan* plan) noexcept {
    return plan != nullptr &&
           (plan->forward != identity || plan->paste_donor_slot >= 0 || plan->erasure.dropout_probability > 0 || plan->erasure.rectangular != 0);
}
AugmentationAnnotationSupport resolve_augmentation_annotation_support(const std::array<float, 4>& source_box,
                                                                      std::span<const mmltk::backend::data::RLEPair> source_mask, int width, int height,
                                                                      const AugmentationImagePlan* plan, bool donor, bool mask_present) {
    if (width <= 0 || height <= 0) throw std::invalid_argument("augmentation support requires positive image dimensions");
    const float image_width = static_cast<float>(width), image_height = static_cast<float>(height);
    mask_present = mask_present || !source_mask.empty();
    if (donor && plan != nullptr && plan->paste_masked && source_mask.empty()) return {};
    if (donor && (plan == nullptr || !plan->paste_masked)) {
        source_mask = {};
        mask_present = false;
    }
    const auto original = source_mask.empty() ? AugmentationAnnotationSupport{transform_augmentation_box_xyxy(source_box, identity),
                                                                              augmentation_box_area(source_box) * image_width * image_height, true}
                                              : mask_extent(source_mask, width, height);
    if (!augmentation_changes_support(plan)) {
        auto result = original;
        result.box_xyxy = transform_augmentation_box_xyxy(source_box, identity);
        if (mask_present && source_mask.empty()) result.area_pixels = 0;
        return result;
    }
    const auto& inverse = donor ? plan->paste_inverse : plan->inverse;
    auto candidate = donor ? plan->paste_output_box : transform_augmentation_box_xyxy(source_mask.empty() ? source_box : original.box_xyxy, plan->forward);
    if (donor && !source_mask.empty()) {
        const float scale = 1 / inverse[0];
        candidate = transform_augmentation_box_xyxy(original.box_xyxy, {scale, 0, -inverse[2] * scale, 0, scale, -inverse[5] * scale});
    }
    const auto detection_box = transform_augmentation_box_xyxy(source_box, plan->forward);
    const bool has_paste = !donor && plan->paste_donor_slot >= 0 && (!plan->paste_masked || plan->paste_support_count != 0);
    const bool modifies_visibility = donor || has_paste || plan->erasure.dropout_probability > 0 || plan->erasure.rectangular != 0;
    if (!modifies_visibility && source_mask.empty()) {
        return {detection_box, mask_present ? 0.0F : augmentation_box_area(detection_box) * image_width * image_height,
                augmentation_box_area(detection_box) > 0};
    }
    if (has_paste && plan->paste_masked && (plan->paste_support == nullptr || plan->paste_support_count == 0))
        throw std::invalid_argument("masked augmentation paste requires original host donor support");
    const std::span<const mmltk::backend::data::RLEPair> donor_mask =
        has_paste && plan->paste_masked ? std::span{plan->paste_support, plan->paste_support_count} : std::span<const mmltk::backend::data::RLEPair>{};
    AugmentationAnnotationSupport result;
    int min_x = width, min_y = height, max_x = -1, max_y = -1;
    const int x0 = std::clamp(static_cast<int>(std::floor(candidate[0] * image_width)), 0, width);
    const int x1 = std::clamp(static_cast<int>(std::ceil(candidate[2] * image_width)), 0, width);
    const int y0 = std::clamp(static_cast<int>(std::floor(candidate[1] * image_height)), 0, height);
    const int y1 = std::clamp(static_cast<int>(std::ceil(candidate[3] * image_height)), 0, height);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float nx = (static_cast<float>(x) + 0.5F) / image_width, ny = (static_cast<float>(y) + 0.5F) / image_height;
            if (augment_math::erases_pixel(plan->erasure, x, y, width, height) || !contains(source_box, source_mask, inverse, nx, ny, width, height) ||
                (has_paste && contains(plan->paste_source_box, donor_mask, plan->paste_inverse, nx, ny, width, height)))
                continue;
            ++result.area_pixels;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
    result.present = max_x >= 0;
    if (result.present)
        result.box_xyxy = {static_cast<float>(min_x) / image_width, static_cast<float>(min_y) / image_height, static_cast<float>(max_x + 1) / image_width,
                           static_cast<float>(max_y + 1) / image_height};
    // Raster support governs custom erasure and occlusion. Pure geometry keeps
    // the supplied continuous detection box, even when its resized mask vanishes.
    if (!modifies_visibility) {
        result.box_xyxy = detection_box;
        result.present = augmentation_box_area(detection_box) > 0;
    }
    if (mask_present && source_mask.empty()) result.area_pixels = 0;
    return result;
}
}  // namespace mmltk::backend::models::rfdetr
