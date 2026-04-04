#include "gui/annotation/document/edit.h"

#include "gui/annotation_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace mmltk::gui {

namespace {

struct EditableMaskState {
    AnnotationBox box{};
    AnnotationMaskRegion region{};
    std::vector<std::uint8_t> mask;
};

bool mask_region_valid(const AnnotationMaskRegion& region) {
    return region.width > 0U && region.height > 0U;
}

AnnotationMaskRegion mask_region_from_box(const AnnotationBox& box) {
    return AnnotationMaskRegion{
        static_cast<std::uint32_t>(std::max(0, box.x1)),
        static_cast<std::uint32_t>(std::max(0, box.y1)),
        static_cast<std::uint32_t>(std::max(0, box.x2 - box.x1)),
        static_cast<std::uint32_t>(std::max(0, box.y2 - box.y1)),
    };
}

AnnotationBox box_from_mask_region(const AnnotationMaskRegion& region) {
    return AnnotationBox{
        static_cast<int>(region.capture_x),
        static_cast<int>(region.capture_y),
        static_cast<int>(region.capture_x + region.width),
        static_cast<int>(region.capture_y + region.height),
    };
}

bool boxes_equal(const AnnotationBox& lhs, const AnnotationBox& rhs) {
    return lhs.x1 == rhs.x1 &&
           lhs.y1 == rhs.y1 &&
           lhs.x2 == rhs.x2 &&
           lhs.y2 == rhs.y2;
}

bool points_equal(const AnnotationPoint& lhs, const AnnotationPoint& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

AnnotationBox local_box_to_capture_box(const AnnotationMaskRegion& region, const AnnotationBox& local_box) {
    return AnnotationBox{
        static_cast<int>(region.capture_x) + local_box.x1,
        static_cast<int>(region.capture_y) + local_box.y1,
        static_cast<int>(region.capture_x) + local_box.x2,
        static_cast<int>(region.capture_y) + local_box.y2,
    };
}

AnnotationPoint clamp_capture_point(const AnnotationPoint& point,
                                    const std::uint32_t capture_width,
                                    const std::uint32_t capture_height) {
    const float max_x = capture_width == 0U ? 0.0f : static_cast<float>(capture_width - 1U);
    const float max_y = capture_height == 0U ? 0.0f : static_cast<float>(capture_height - 1U);
    return AnnotationPoint{
        std::clamp(point.x, 0.0f, max_x),
        std::clamp(point.y, 0.0f, max_y),
    };
}

void translate_clamped_point(AnnotationPoint* point,
                             const float dx,
                             const float dy,
                             const std::uint32_t capture_width,
                             const std::uint32_t capture_height) {
    if (point == nullptr) {
        return;
    }
    *point = clamp_capture_point(AnnotationPoint{
                                     point->x + dx,
                                     point->y + dy,
                                 },
                                 capture_width,
                                 capture_height);
}

void synchronize_opposite_spline_handle(AnnotationSplineKnot* knot,
                                        const AnnotationHandleRole moved_role,
                                        const std::uint32_t capture_width,
                                        const std::uint32_t capture_height) {
    if (knot == nullptr || knot->handle_mode == AnnotationSplineHandleMode::Corner) {
        return;
    }

    AnnotationSplineHandle* moved = nullptr;
    AnnotationSplineHandle* opposite = nullptr;
    if (moved_role == AnnotationHandleRole::SplineInHandle) {
        moved = &knot->in_handle;
        opposite = &knot->out_handle;
    } else if (moved_role == AnnotationHandleRole::SplineOutHandle) {
        moved = &knot->out_handle;
        opposite = &knot->in_handle;
    } else {
        return;
    }

    if (!moved->enabled) {
        return;
    }

    const float vx = moved->position.x - knot->position.x;
    const float vy = moved->position.y - knot->position.y;
    const float moved_length = std::hypot(vx, vy);
    if (moved_length <= std::numeric_limits<float>::epsilon()) {
        opposite->enabled = false;
        opposite->position = knot->position;
        return;
    }

    const bool opposite_was_enabled = opposite->enabled;
    opposite->enabled = true;
    float opposite_length = moved_length;
    if (knot->handle_mode == AnnotationSplineHandleMode::Smooth) {
        const float current_length = opposite_was_enabled
                                         ? std::hypot(opposite->position.x - knot->position.x,
                                                      opposite->position.y - knot->position.y)
                                         : 0.0f;
        if (current_length > std::numeric_limits<float>::epsilon()) {
            opposite_length = current_length;
        }
    }

    const float scale = opposite_length / moved_length;
    opposite->position = clamp_capture_point(AnnotationPoint{
                                                 knot->position.x - vx * scale,
                                                 knot->position.y - vy * scale,
                                             },
                                             capture_width,
                                             capture_height);
}

bool mask_shape_valid(const AnnotationMaskShape& shape) {
    return mask_region_valid(shape.region) &&
           shape.mask.size() ==
               static_cast<std::size_t>(shape.region.width) *
                   static_cast<std::size_t>(shape.region.height);
}

EditableMaskState make_empty_mask_state() {
    return EditableMaskState{};
}

EditableMaskState make_full_box_mask_state(const AnnotationBox& box) {
    EditableMaskState state;
    state.box = box;
    if (!annotation_box_has_area(box)) {
        return state;
    }
    state.region = mask_region_from_box(box);
    state.mask.assign(static_cast<std::size_t>(state.region.width) * static_cast<std::size_t>(state.region.height), 1U);
    return state;
}

std::optional<EditableMaskState> editable_mask_state_from_object(const AnnotationObject& object,
                                                                 const std::uint32_t capture_width,
                                                                 const std::uint32_t capture_height) {
    if (const auto* box_shape = std::get_if<AnnotationBoxShape>(&object.shape); box_shape != nullptr) {
        const AnnotationBox clamped = normalize_annotation_box(box_shape->box, capture_width, capture_height);
        if (!annotation_box_has_area(clamped)) {
            return std::nullopt;
        }
        return make_full_box_mask_state(clamped);
    }

    const auto* mask_shape = std::get_if<AnnotationMaskShape>(&object.shape);
    if (mask_shape == nullptr) {
        return std::nullopt;
    }
    if (mask_shape_valid(*mask_shape)) {
        EditableMaskState state;
        state.region = mask_shape->region;
        state.mask = mask_shape->mask;
        const AnnotationBox region_box = normalize_annotation_box(box_from_mask_region(mask_shape->region),
                                                                  capture_width,
                                                                  capture_height);
        if (annotation_box_has_area(mask_shape->box)) {
            state.box = normalize_annotation_box(mask_shape->box, capture_width, capture_height);
        } else if (const std::optional<AnnotationBox> local_box =
                       annotation_bbox_from_mask(mask_shape->mask, mask_shape->region.width, mask_shape->region.height);
                   local_box.has_value()) {
            state.box = local_box_to_capture_box(mask_shape->region, *local_box);
        } else {
            state.box = region_box;
        }
        if (annotation_box_has_area(state.box) || mask_region_valid(state.region)) {
            return state;
        }
    }

    const AnnotationBox fallback_box = normalize_annotation_box(mask_shape->box, capture_width, capture_height);
    if (!annotation_box_has_area(fallback_box)) {
        return std::nullopt;
    }
    return make_full_box_mask_state(fallback_box);
}

void write_mask_state_to_object(AnnotationObject* object, EditableMaskState state) {
    if (object == nullptr) {
        return;
    }
    if (!mask_region_valid(state.region) || state.mask.empty() || !annotation_box_has_area(state.box)) {
        object->shape = AnnotationMaskShape{};
        return;
    }
    object->shape = AnnotationMaskShape{
        state.box,
        state.region,
        std::move(state.mask),
        0U,
        std::nullopt,
    };
}

void ensure_mask_bounds(EditableMaskState* state,
                        const AnnotationBox& required_box,
                        const std::uint32_t capture_width,
                        const std::uint32_t capture_height) {
    if (state == nullptr) {
        return;
    }
    const AnnotationBox clamped_required = normalize_annotation_box(required_box, capture_width, capture_height);
    if (!annotation_box_has_area(clamped_required)) {
        return;
    }

    if (!mask_region_valid(state->region) || state->mask.empty()) {
        state->region = mask_region_from_box(clamped_required);
        state->mask.assign(static_cast<std::size_t>(state->region.width) * static_cast<std::size_t>(state->region.height), 0U);
        return;
    }

    const AnnotationBox current_box = box_from_mask_region(state->region);
    const AnnotationBox union_box{
        std::min(current_box.x1, clamped_required.x1),
        std::min(current_box.y1, clamped_required.y1),
        std::max(current_box.x2, clamped_required.x2),
        std::max(current_box.y2, clamped_required.y2),
    };
    if (boxes_equal(union_box, current_box)) {
        return;
    }

    const AnnotationMaskRegion expanded_region = mask_region_from_box(union_box);
    std::vector<std::uint8_t> expanded(static_cast<std::size_t>(expanded_region.width) *
                                           static_cast<std::size_t>(expanded_region.height),
                                       0U);
    const int dx = static_cast<int>(state->region.capture_x) - static_cast<int>(expanded_region.capture_x);
    const int dy = static_cast<int>(state->region.capture_y) - static_cast<int>(expanded_region.capture_y);
    for (std::uint32_t row = 0; row < state->region.height; ++row) {
        const std::size_t src_offset =
            static_cast<std::size_t>(row) * static_cast<std::size_t>(state->region.width);
        const std::size_t dst_offset =
            static_cast<std::size_t>(static_cast<int>(row) + dy) * static_cast<std::size_t>(expanded_region.width) +
            static_cast<std::size_t>(dx);
        std::copy_n(state->mask.begin() + static_cast<std::ptrdiff_t>(src_offset),
                    state->region.width,
                    expanded.begin() + static_cast<std::ptrdiff_t>(dst_offset));
    }
    state->region = expanded_region;
    state->mask = std::move(expanded);
}

void trim_mask_state(EditableMaskState* state) {
    if (state == nullptr || !mask_region_valid(state->region) || state->mask.empty()) {
        if (state != nullptr) {
            *state = make_empty_mask_state();
        }
        return;
    }

    const std::optional<AnnotationBox> local_box =
        annotation_bbox_from_mask(state->mask, state->region.width, state->region.height);
    if (!local_box.has_value()) {
        *state = make_empty_mask_state();
        return;
    }

    const AnnotationMaskRegion trimmed_region{
        state->region.capture_x + static_cast<std::uint32_t>(local_box->x1),
        state->region.capture_y + static_cast<std::uint32_t>(local_box->y1),
        static_cast<std::uint32_t>(local_box->x2 - local_box->x1),
        static_cast<std::uint32_t>(local_box->y2 - local_box->y1),
    };
    std::vector<std::uint8_t> trimmed(static_cast<std::size_t>(trimmed_region.width) *
                                          static_cast<std::size_t>(trimmed_region.height),
                                      0U);
    for (std::uint32_t row = 0; row < trimmed_region.height; ++row) {
        const std::size_t src_offset =
            static_cast<std::size_t>(local_box->y1 + static_cast<int>(row)) * static_cast<std::size_t>(state->region.width) +
            static_cast<std::size_t>(local_box->x1);
        const std::size_t dst_offset = static_cast<std::size_t>(row) * static_cast<std::size_t>(trimmed_region.width);
        std::copy_n(state->mask.begin() + static_cast<std::ptrdiff_t>(src_offset),
                    trimmed_region.width,
                    trimmed.begin() + static_cast<std::ptrdiff_t>(dst_offset));
    }
    state->region = trimmed_region;
    state->box = box_from_mask_region(trimmed_region);
    state->mask = std::move(trimmed);
}

void set_mask_pixel(EditableMaskState* state, const int local_x, const int local_y, const bool enabled) {
    if (state == nullptr ||
        local_x < 0 ||
        local_y < 0 ||
        local_x >= static_cast<int>(state->region.width) ||
        local_y >= static_cast<int>(state->region.height)) {
        return;
    }
    state->mask[static_cast<std::size_t>(local_y) * static_cast<std::size_t>(state->region.width) +
                static_cast<std::size_t>(local_x)] = enabled ? 1U : 0U;
}

void paint_circle(EditableMaskState* state,
                  const int capture_x,
                  const int capture_y,
                  const int radius,
                  const bool erase) {
    if (state == nullptr || radius <= 0) {
        return;
    }
    const int local_center_x = capture_x - static_cast<int>(state->region.capture_x);
    const int local_center_y = capture_y - static_cast<int>(state->region.capture_y);
    const int radius_sq = radius * radius;
    const int min_x = std::max(0, local_center_x - radius);
    const int max_x = std::min(static_cast<int>(state->region.width), local_center_x + radius + 1);
    const int min_y = std::max(0, local_center_y - radius);
    const int max_y = std::min(static_cast<int>(state->region.height), local_center_y + radius + 1);
    for (int y = min_y; y < max_y; ++y) {
        const int dy = y - local_center_y;
        for (int x = min_x; x < max_x; ++x) {
            const int dx = x - local_center_x;
            if (dx * dx + dy * dy > radius_sq) {
                continue;
            }
            set_mask_pixel(state, x, y, !erase);
        }
    }
}

void fill_connected_component(std::vector<std::uint8_t>* mask,
                              const std::uint32_t width,
                              const std::uint32_t height,
                              const int seed_x,
                              const int seed_y,
                              const std::uint8_t target,
                              const std::uint8_t replacement) {
    if (mask == nullptr || width == 0U || height == 0U) {
        return;
    }
    if (seed_x < 0 || seed_y < 0 || seed_x >= static_cast<int>(width) || seed_y >= static_cast<int>(height)) {
        return;
    }
    const std::size_t seed_index =
        static_cast<std::size_t>(seed_y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(seed_x);
    if ((*mask)[seed_index] != target || target == replacement) {
        return;
    }

    std::queue<std::pair<int, int>> queue;
    queue.emplace(seed_x, seed_y);
    (*mask)[seed_index] = replacement;
    constexpr std::array<std::pair<int, int>, 4> kOffsets{{
        {1, 0},
        {-1, 0},
        {0, 1},
        {0, -1},
    }};
    while (!queue.empty()) {
        const auto [x, y] = queue.front();
        queue.pop();
        for (const auto [dx, dy] : kOffsets) {
            const int nx = x + dx;
            const int ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= static_cast<int>(width) || ny >= static_cast<int>(height)) {
                continue;
            }
            const std::size_t index =
                static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) + static_cast<std::size_t>(nx);
            if ((*mask)[index] != target) {
                continue;
            }
            (*mask)[index] = replacement;
            queue.emplace(nx, ny);
        }
    }
}

std::vector<std::uint8_t> keep_largest_component(std::vector<std::uint8_t> mask,
                                                 const std::uint32_t width,
                                                 const std::uint32_t height) {
    if (width == 0U || height == 0U || mask.empty()) {
        return mask;
    }

    std::vector<std::uint32_t> labels(mask.size(), 0U);
    std::vector<std::size_t> sizes{0U};
    std::uint32_t next_label = 1U;
    constexpr std::array<std::pair<int, int>, 4> kOffsets{{
        {1, 0},
        {-1, 0},
        {0, 1},
        {0, -1},
    }};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t start = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            if (mask[start] == 0U || labels[start] != 0U) {
                continue;
            }
            labels[start] = next_label;
            std::size_t component_size = 0U;
            std::queue<std::pair<int, int>> queue;
            queue.emplace(static_cast<int>(x), static_cast<int>(y));
            while (!queue.empty()) {
                const auto [cx, cy] = queue.front();
                queue.pop();
                ++component_size;
                for (const auto [dx, dy] : kOffsets) {
                    const int nx = cx + dx;
                    const int ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= static_cast<int>(width) || ny >= static_cast<int>(height)) {
                        continue;
                    }
                    const std::size_t index =
                        static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) + static_cast<std::size_t>(nx);
                    if (mask[index] == 0U || labels[index] != 0U) {
                        continue;
                    }
                    labels[index] = next_label;
                    queue.emplace(nx, ny);
                }
            }
            sizes.push_back(component_size);
            ++next_label;
        }
    }

    if (sizes.size() <= 1U) {
        return mask;
    }
    std::size_t keep_label = 1U;
    for (std::size_t label = 2U; label < sizes.size(); ++label) {
        if (sizes[label] > sizes[keep_label]) {
            keep_label = label;
        }
    }
    for (std::size_t index = 0; index < mask.size(); ++index) {
        mask[index] = labels[index] == keep_label ? 1U : 0U;
    }
    return mask;
}

std::vector<std::uint8_t> fill_holes(std::vector<std::uint8_t> mask,
                                     const std::uint32_t width,
                                     const std::uint32_t height) {
    if (width == 0U || height == 0U || mask.empty()) {
        return mask;
    }

    std::vector<std::uint8_t> outside(mask.size(), 0U);
    std::queue<std::pair<int, int>> queue;
    const auto push_background = [&](const int x, const int y) {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) {
            return;
        }
        const std::size_t index =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
        if (mask[index] != 0U || outside[index] != 0U) {
            return;
        }
        outside[index] = 1U;
        queue.emplace(x, y);
    };

    for (std::uint32_t x = 0; x < width; ++x) {
        push_background(static_cast<int>(x), 0);
        push_background(static_cast<int>(x), static_cast<int>(height) - 1);
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        push_background(0, static_cast<int>(y));
        push_background(static_cast<int>(width) - 1, static_cast<int>(y));
    }

    constexpr std::array<std::pair<int, int>, 4> kOffsets{{
        {1, 0},
        {-1, 0},
        {0, 1},
        {0, -1},
    }};
    while (!queue.empty()) {
        const auto [x, y] = queue.front();
        queue.pop();
        for (const auto [dx, dy] : kOffsets) {
            push_background(x + dx, y + dy);
        }
    }

    for (std::size_t index = 0; index < mask.size(); ++index) {
        if (mask[index] == 0U && outside[index] == 0U) {
            mask[index] = 1U;
        }
    }
    return mask;
}

std::vector<std::uint32_t> build_integral_mask(const std::vector<std::uint8_t>& mask,
                                               const std::uint32_t width,
                                               const std::uint32_t height) {
    std::vector<std::uint32_t> integral(static_cast<std::size_t>(width + 1U) * static_cast<std::size_t>(height + 1U), 0U);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint32_t row_sum = 0U;
        for (std::uint32_t x = 0; x < width; ++x) {
            row_sum += mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] != 0U ? 1U : 0U;
            integral[static_cast<std::size_t>(y + 1U) * static_cast<std::size_t>(width + 1U) + static_cast<std::size_t>(x + 1U)] =
                integral[static_cast<std::size_t>(y) * static_cast<std::size_t>(width + 1U) + static_cast<std::size_t>(x + 1U)] + row_sum;
        }
    }
    return integral;
}

std::uint32_t rect_sum(const std::vector<std::uint32_t>& integral,
                       const std::uint32_t stride,
                       const int x1,
                       const int y1,
                       const int x2,
                       const int y2) {
    const std::size_t top_left = static_cast<std::size_t>(y1) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x1);
    const std::size_t top_right = static_cast<std::size_t>(y1) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x2);
    const std::size_t bottom_left = static_cast<std::size_t>(y2) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x1);
    const std::size_t bottom_right = static_cast<std::size_t>(y2) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x2);
    return integral[bottom_right] - integral[top_right] - integral[bottom_left] + integral[top_left];
}

template <typename ThresholdFn>
std::vector<std::uint8_t> morphological_filter(const std::vector<std::uint8_t>& mask,
                                               const std::uint32_t width,
                                               const std::uint32_t height,
                                               const int radius,
                                               ThresholdFn threshold_fn) {
    if (width == 0U || height == 0U || mask.empty() || radius <= 0) {
        return mask;
    }
    const std::vector<std::uint32_t> integral = build_integral_mask(mask, width, height);
    const std::uint32_t stride = width + 1U;
    std::vector<std::uint8_t> output(mask.size(), 0U);
    for (int y = 0; y < static_cast<int>(height); ++y) {
        const int y1 = std::max(0, y - radius);
        const int y2 = std::min(static_cast<int>(height), y + radius + 1);
        for (int x = 0; x < static_cast<int>(width); ++x) {
            const int x1 = std::max(0, x - radius);
            const int x2 = std::min(static_cast<int>(width), x + radius + 1);
            const std::uint32_t covered = rect_sum(integral, stride, x1, y1, x2, y2);
            const auto area = static_cast<std::uint32_t>((x2 - x1) * (y2 - y1));
            output[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
                threshold_fn(covered, area) ? 1U : 0U;
        }
    }
    return output;
}

std::vector<std::uint8_t> dilate_mask_square(const std::vector<std::uint8_t>& mask,
                                             const std::uint32_t width,
                                             const std::uint32_t height,
                                             const int radius) {
    return morphological_filter(mask, width, height, radius,
        [](std::uint32_t covered, std::uint32_t /*area*/) { return covered > 0U; });
}

std::vector<std::uint8_t> erode_mask_square(const std::vector<std::uint8_t>& mask,
                                            const std::uint32_t width,
                                            const std::uint32_t height,
                                            const int radius) {
    return morphological_filter(mask, width, height, radius,
        [](std::uint32_t covered, std::uint32_t area) { return covered == area; });
}

AnnotationSkeletonNode* get_skeleton_node(AnnotationObject* object, const std::size_t node_index) {
    if (object == nullptr) {
        return nullptr;
    }
    auto* skeleton = std::get_if<AnnotationSkeletonShape>(&object->shape);
    if (skeleton == nullptr || node_index >= skeleton->nodes.size()) {
        return nullptr;
    }
    return &skeleton->nodes[node_index];
}

AnnotationSplineKnot* get_spline_knot(AnnotationObject* object, const std::size_t knot_index) {
    if (object == nullptr) {
        return nullptr;
    }
    auto* spline = std::get_if<AnnotationSplineShape>(&object->shape);
    if (spline == nullptr || knot_index >= spline->knots.size()) {
        return nullptr;
    }
    return &spline->knots[knot_index];
}

std::optional<EditableMaskState> prepare_mask_edit(const AnnotationObject* object,
                                                   const std::uint32_t capture_width,
                                                   const std::uint32_t capture_height) {
    if (object == nullptr || !annotation_object_supports_mask_editing(*object) ||
        capture_width == 0U || capture_height == 0U) {
        return std::nullopt;
    }
    return editable_mask_state_from_object(*object, capture_width, capture_height);
}

} // namespace

const char* annotation_mask_cleanup_label(const AnnotationMaskCleanupOp op) noexcept {
    switch (op) {
    case AnnotationMaskCleanupOp::LargestComponent:
        return "Largest";
    case AnnotationMaskCleanupOp::FillHoles:
        return "Fill Holes";
    case AnnotationMaskCleanupOp::Dilate:
        return "Dilate";
    case AnnotationMaskCleanupOp::Erode:
        return "Erode";
    case AnnotationMaskCleanupOp::Open:
        return "Open";
    case AnnotationMaskCleanupOp::Close:
        return "Close";
    }
    return "Unknown";
}

std::string next_annotation_object_id(const std::size_t object_count) {
    return "manual-" + std::to_string(object_count + 1U);
}

AnnotationPoint annotation_frame_capture_center(const AnnotationFrame& frame) {
    const AnnotationBox view_box = annotation_frame_view_box(frame);
    const float center_x =
        static_cast<float>(view_box.x1 + std::max(view_box.x1, view_box.x2 - 1)) * 0.5f;
    const float center_y =
        static_cast<float>(view_box.y1 + std::max(view_box.y1, view_box.y2 - 1)) * 0.5f;
    return AnnotationPoint{center_x, center_y};
}

AnnotationObject make_point_annotation_object(const std::size_t object_count,
                                              const std::size_t category_index,
                                              const AnnotationPoint& point) {
    AnnotationObject object;
    object.object_id = next_annotation_object_id(object_count);
    object.category_index = category_index;
    object.shape = AnnotationPointShape{point};
    return object;
}

AnnotationObject make_spline_annotation_object(const std::size_t object_count,
                                               const std::size_t category_index) {
    AnnotationObject object;
    object.object_id = next_annotation_object_id(object_count);
    object.category_index = category_index;
    object.shape = AnnotationSplineShape{};
    return object;
}

AnnotationObject make_skeleton_annotation_object(const std::size_t object_count,
                                                 const std::size_t category_index,
                                                 const AnnotationCategory* category) {
    AnnotationObject object;
    object.object_id = next_annotation_object_id(object_count);
    object.category_index = category_index;

    AnnotationSkeletonShape shape;
    if (category != nullptr) {
        shape.nodes.reserve(category->keypoints.size());
        for (const std::string& keypoint : category->keypoints) {
            shape.nodes.push_back(AnnotationSkeletonNode{keypoint, {}, false});
        }
        shape.edges.reserve(category->skeleton_edges.size());
        for (const AnnotationCategorySkeletonEdge& edge : category->skeleton_edges) {
            shape.edges.push_back(AnnotationSkeletonEdge{edge.source_index, edge.target_index});
        }
    }

    object.shape = std::move(shape);
    return object;
}

bool remap_skeleton_annotation_object_to_category(AnnotationObject* object,
                                                  const AnnotationCategory* category) {
    if (object == nullptr) {
        return false;
    }

    auto* skeleton = std::get_if<AnnotationSkeletonShape>(&object->shape);
    if (skeleton == nullptr || category == nullptr) {
        return false;
    }
    if (category->keypoints.empty() && category->skeleton_edges.empty()) {
        return false;
    }

    AnnotationSkeletonShape remapped;
    remapped.nodes.reserve(category->keypoints.size());
    remapped.edges.reserve(category->skeleton_edges.size());

    std::vector<bool> consumed_nodes(skeleton->nodes.size(), false);
    for (std::size_t keypoint_index = 0; keypoint_index < category->keypoints.size(); ++keypoint_index) {
        const std::string& keypoint = category->keypoints[keypoint_index];
        std::size_t matched_node_index = skeleton->nodes.size();
        for (std::size_t node_index = 0; node_index < skeleton->nodes.size(); ++node_index) {
            if (!consumed_nodes[node_index] &&
                skeleton->nodes[node_index].key == keypoint) {
                matched_node_index = node_index;
                break;
            }
        }
        if (matched_node_index >= skeleton->nodes.size() &&
            keypoint_index < skeleton->nodes.size() &&
            !consumed_nodes[keypoint_index]) {
            matched_node_index = keypoint_index;
        }

        if (matched_node_index < skeleton->nodes.size()) {
            consumed_nodes[matched_node_index] = true;
            AnnotationSkeletonNode node = skeleton->nodes[matched_node_index];
            node.key = keypoint;
            remapped.nodes.push_back(std::move(node));
        } else {
            remapped.nodes.push_back(AnnotationSkeletonNode{
                keypoint,
                {},
                false,
            });
        }
    }

    for (const AnnotationCategorySkeletonEdge& edge : category->skeleton_edges) {
        if (edge.source_index >= remapped.nodes.size() ||
            edge.target_index >= remapped.nodes.size()) {
            continue;
        }
        remapped.edges.push_back(AnnotationSkeletonEdge{
            edge.source_index,
            edge.target_index,
        });
    }

    const bool nodes_changed =
        remapped.nodes.size() != skeleton->nodes.size() ||
        !std::equal(remapped.nodes.begin(),
                    remapped.nodes.end(),
                    skeleton->nodes.begin(),
                    skeleton->nodes.end(),
                    [] (const AnnotationSkeletonNode& lhs, const AnnotationSkeletonNode& rhs) {
                        return lhs.key == rhs.key &&
                               lhs.point.x == rhs.point.x &&
                               lhs.point.y == rhs.point.y &&
                               lhs.visible == rhs.visible;
                    });
    const bool edges_changed =
        remapped.edges.size() != skeleton->edges.size() ||
        !std::equal(remapped.edges.begin(),
                    remapped.edges.end(),
                    skeleton->edges.begin(),
                    skeleton->edges.end(),
                    [] (const AnnotationSkeletonEdge& lhs, const AnnotationSkeletonEdge& rhs) {
                        return lhs.source_index == rhs.source_index &&
                               lhs.target_index == rhs.target_index;
                    });
    if (!nodes_changed && !edges_changed) {
        return false;
    }

    *skeleton = std::move(remapped);
    return true;
}

bool set_point_annotation_position(AnnotationObject* object,
                                   const AnnotationPoint& point,
                                   const std::uint32_t capture_width,
                                   const std::uint32_t capture_height) {
    if (object == nullptr) {
        return false;
    }
    auto* point_shape = std::get_if<AnnotationPointShape>(&object->shape);
    if (point_shape == nullptr) {
        return false;
    }

    const float max_x = capture_width == 0U ? 0.0f : static_cast<float>(capture_width - 1U);
    const float max_y = capture_height == 0U ? 0.0f : static_cast<float>(capture_height - 1U);
    const AnnotationPoint clamped_point{
        std::clamp(point.x, 0.0f, max_x),
        std::clamp(point.y, 0.0f, max_y),
    };
    if (point_shape->point.x == clamped_point.x &&
        point_shape->point.y == clamped_point.y) {
        return false;
    }
    point_shape->point = clamped_point;
    return true;
}

bool set_annotation_object_handle_position(AnnotationObject* object,
                                           const AnnotationHandleId& handle,
                                           const AnnotationPoint& point,
                                           const std::uint32_t capture_width,
                                           const std::uint32_t capture_height) {
    if (object == nullptr || handle.role == AnnotationHandleRole::None) {
        return false;
    }

    const AnnotationPoint clamped_point =
        clamp_capture_point(point, capture_width, capture_height);
    return std::visit(
        [&](auto& shape) -> bool {
            using T = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<T, AnnotationPointShape>) {
                if (handle.role != AnnotationHandleRole::Point ||
                    handle.element_index != 0U ||
                    points_equal(shape.point, clamped_point)) {
                    return false;
                }
                shape.point = clamped_point;
                return true;
            } else if constexpr (std::is_same_v<T, AnnotationSplineShape>) {
                if (handle.element_index >= shape.knots.size()) {
                    return false;
                }
                AnnotationSplineKnot& knot = shape.knots[handle.element_index];
                if (handle.role == AnnotationHandleRole::SplineKnot) {
                    if (points_equal(knot.position, clamped_point)) {
                        return false;
                    }
                    const float dx = clamped_point.x - knot.position.x;
                    const float dy = clamped_point.y - knot.position.y;
                    knot.position = clamped_point;
                    if (knot.in_handle.enabled) {
                        translate_clamped_point(&knot.in_handle.position,
                                                dx,
                                                dy,
                                                capture_width,
                                                capture_height);
                    }
                    if (knot.out_handle.enabled) {
                        translate_clamped_point(&knot.out_handle.position,
                                                dx,
                                                dy,
                                                capture_width,
                                                capture_height);
                    }
                    return true;
                }
                AnnotationSplineHandle* moved_handle =
                    handle.role == AnnotationHandleRole::SplineInHandle ? &knot.in_handle
                    : handle.role == AnnotationHandleRole::SplineOutHandle ? &knot.out_handle
                                                                           : nullptr;
                if (moved_handle == nullptr ||
                    (moved_handle->enabled && points_equal(moved_handle->position, clamped_point))) {
                    return false;
                }
                moved_handle->enabled = true;
                moved_handle->position = clamped_point;
                synchronize_opposite_spline_handle(&knot,
                                                   handle.role,
                                                   capture_width,
                                                   capture_height);
                return true;
            } else if constexpr (std::is_same_v<T, AnnotationSkeletonShape>) {
                if (handle.role != AnnotationHandleRole::SkeletonNode ||
                    handle.element_index >= shape.nodes.size()) {
                    return false;
                }
                AnnotationSkeletonNode& node = shape.nodes[handle.element_index];
                if (node.visible && points_equal(node.point, clamped_point)) {
                    return false;
                }
                node.point = clamped_point;
                node.visible = true;
                return true;
            } else {
                return false;
            }
        },
        object->shape);
}

bool append_spline_knot(AnnotationObject* object, const AnnotationPoint& point) {
    if (object == nullptr) {
        return false;
    }
    auto* spline = std::get_if<AnnotationSplineShape>(&object->shape);
    if (spline == nullptr) {
        return false;
    }
    spline->knots.push_back(AnnotationSplineKnot{
        point,
        {},
        {},
        AnnotationSplineHandleMode::Corner,
    });
    return true;
}

bool insert_spline_knot(AnnotationObject* object,
                        const std::size_t segment_index,
                        const AnnotationPoint& point) {
    if (object == nullptr) {
        return false;
    }
    auto* spline = std::get_if<AnnotationSplineShape>(&object->shape);
    if (spline == nullptr || spline->knots.empty()) {
        return false;
    }

    const std::size_t segment_count =
        spline->closed ? spline->knots.size() : spline->knots.size() - 1U;
    if (segment_count == 0U || segment_index >= segment_count) {
        return false;
    }
    const std::size_t insert_index = segment_index + 1U;
    spline->knots.insert(spline->knots.begin() + static_cast<std::ptrdiff_t>(insert_index),
                         AnnotationSplineKnot{
                             point,
                             {},
                             {},
                             AnnotationSplineHandleMode::Corner,
                         });
    return true;
}

bool remove_spline_knot(AnnotationObject* object, const std::size_t knot_index) {
    if (object == nullptr) {
        return false;
    }
    auto* spline = std::get_if<AnnotationSplineShape>(&object->shape);
    if (spline == nullptr || knot_index >= spline->knots.size()) {
        return false;
    }
    spline->knots.erase(spline->knots.begin() + static_cast<std::ptrdiff_t>(knot_index));
    if (spline->closed && spline->knots.size() < 3U) {
        spline->closed = false;
    }
    return true;
}

bool close_spline_shape(AnnotationObject* object) {
    if (object == nullptr) {
        return false;
    }
    auto* spline = std::get_if<AnnotationSplineShape>(&object->shape);
    if (spline == nullptr || spline->closed || spline->knots.size() < 3U) {
        return false;
    }
    spline->closed = true;
    return true;
}

bool reopen_spline_shape(AnnotationObject* object) {
    if (object == nullptr) {
        return false;
    }
    auto* spline = std::get_if<AnnotationSplineShape>(&object->shape);
    if (spline == nullptr || !spline->closed) {
        return false;
    }
    spline->closed = false;
    return true;
}

bool cycle_spline_knot_handle_mode(AnnotationObject* object, const std::size_t knot_index) {
    AnnotationSplineKnot* knot = get_spline_knot(object, knot_index);
    if (knot == nullptr) {
        return false;
    }
    AnnotationSplineHandleMode next_mode = AnnotationSplineHandleMode::Corner;
    switch (knot->handle_mode) {
    case AnnotationSplineHandleMode::Corner:
        next_mode = AnnotationSplineHandleMode::Smooth;
        break;
    case AnnotationSplineHandleMode::Smooth:
        next_mode = AnnotationSplineHandleMode::Mirrored;
        break;
    case AnnotationSplineHandleMode::Mirrored:
        next_mode = AnnotationSplineHandleMode::Corner;
        break;
    }
    return set_spline_knot_handle_mode(object, knot_index, next_mode);
}

bool set_spline_knot_handle_mode(AnnotationObject* object,
                                 const std::size_t knot_index,
                                 const AnnotationSplineHandleMode mode) {
    AnnotationSplineKnot* knot_ptr = get_spline_knot(object, knot_index);
    if (knot_ptr == nullptr) {
        return false;
    }
    AnnotationSplineKnot& knot = *knot_ptr;
    if (knot.handle_mode == mode) {
        return false;
    }
    knot.handle_mode = mode;
    if (mode == AnnotationSplineHandleMode::Corner) {
        return true;
    }
    if (knot.in_handle.enabled) {
        synchronize_opposite_spline_handle(&knot,
                                           AnnotationHandleRole::SplineInHandle,
                                           std::numeric_limits<std::uint32_t>::max(),
                                           std::numeric_limits<std::uint32_t>::max());
    } else if (knot.out_handle.enabled) {
        synchronize_opposite_spline_handle(&knot,
                                           AnnotationHandleRole::SplineOutHandle,
                                           std::numeric_limits<std::uint32_t>::max(),
                                           std::numeric_limits<std::uint32_t>::max());
    }
    return true;
}

bool place_skeleton_node(AnnotationObject* object, const AnnotationPoint& point) {
    if (object == nullptr) {
        return false;
    }
    auto* skeleton = std::get_if<AnnotationSkeletonShape>(&object->shape);
    if (skeleton == nullptr) {
        return false;
    }
    for (AnnotationSkeletonNode& node : skeleton->nodes) {
        if (!node.visible) {
            node.point = point;
            node.visible = true;
            return true;
        }
    }
    const bool generated_chain =
        skeleton->nodes.empty() ||
        std::ranges::all_of(skeleton->nodes,
                    [] (const AnnotationSkeletonNode& node) {
                        return node.key.compare(0, 6, "joint-") == 0;
                    });
    if (generated_chain &&
        (skeleton->nodes.empty() || skeleton->edges.size() + 1U == skeleton->nodes.size())) {
        const std::size_t next_index = skeleton->nodes.size() + 1U;
        if (!skeleton->nodes.empty()) {
            skeleton->edges.push_back(AnnotationSkeletonEdge{
                skeleton->nodes.size() - 1U,
                skeleton->nodes.size(),
            });
        }
        skeleton->nodes.push_back(AnnotationSkeletonNode{
            "joint-" + std::to_string(next_index),
            point,
            true,
        });
        return true;
    }

    float best_distance_sq = std::numeric_limits<float>::max();
    AnnotationSkeletonNode* best_node = nullptr;
    for (AnnotationSkeletonNode& node : skeleton->nodes) {
        if (!node.visible) {
            continue;
        }
        const float dx = node.point.x - point.x;
        const float dy = node.point.y - point.y;
        const float distance_sq = dx * dx + dy * dy;
        if (distance_sq < best_distance_sq) {
            best_distance_sq = distance_sq;
            best_node = &node;
        }
    }
    if (best_node == nullptr) {
        return false;
    }
    best_node->point = point;
    return true;
}

bool place_skeleton_node_at(AnnotationObject* object,
                            const std::size_t node_index,
                            const AnnotationPoint& point) {
    AnnotationSkeletonNode* node = get_skeleton_node(object, node_index);
    if (node == nullptr) {
        return false;
    }
    if (node->visible && points_equal(node->point, point)) {
        return false;
    }
    node->point = point;
    node->visible = true;
    return true;
}

bool set_skeleton_node_visibility(AnnotationObject* object,
                                  const std::size_t node_index,
                                  const bool visible) {
    AnnotationSkeletonNode* node = get_skeleton_node(object, node_index);
    if (node == nullptr) {
        return false;
    }
    if (node->visible == visible) {
        return false;
    }
    node->visible = visible;
    return true;
}

bool reset_skeleton_node(AnnotationObject* object, const std::size_t node_index) {
    AnnotationSkeletonNode* node = get_skeleton_node(object, node_index);
    if (node == nullptr) {
        return false;
    }
    if (!node->visible && points_equal(node->point, AnnotationPoint{})) {
        return false;
    }
    node->point = {};
    node->visible = false;
    return true;
}

std::optional<std::size_t> first_hidden_skeleton_node_index(const AnnotationSkeletonShape* shape) {
    if (shape == nullptr) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < shape->nodes.size(); ++index) {
        if (!shape->nodes[index].visible) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> next_skeleton_node_index(const AnnotationSkeletonShape* shape,
                                                    const std::optional<std::size_t> after_index) {
    if (shape == nullptr) {
        return std::nullopt;
    }
    if (after_index.has_value()) {
        for (std::size_t index = *after_index + 1U; index < shape->nodes.size(); ++index) {
            if (!shape->nodes[index].visible) {
                return index;
            }
        }
    }
    return first_hidden_skeleton_node_index(shape);
}

std::size_t visible_skeleton_node_count(const AnnotationSkeletonShape& shape) {
    return static_cast<std::size_t>(std::ranges::count_if(shape.nodes,
                                                  [] (const AnnotationSkeletonNode& node) {
                                                      return node.visible;
                                                  }));
}

bool set_annotation_object_box(AnnotationObject* object,
                               const AnnotationBox& box,
                               const std::uint32_t capture_width,
                               const std::uint32_t capture_height) {
    if (object == nullptr || !annotation_object_supports_mask_editing(*object)) {
        return false;
    }
    const AnnotationBox clamped = normalize_annotation_box(box, capture_width, capture_height);
    if (const auto* current_box = std::get_if<AnnotationBoxShape>(&object->shape);
        current_box != nullptr && boxes_equal(current_box->box, clamped)) {
        return false;
    }
    object->shape = AnnotationBoxShape{clamped};
    return true;
}

bool paint_annotation_object_mask(AnnotationObject* object,
                                  const int capture_x,
                                  const int capture_y,
                                  int radius,
                                  const bool erase,
                                  const std::uint32_t capture_width,
                                  const std::uint32_t capture_height) {
    radius = std::max(radius, 1);
    const AnnotationBox stamp_box = normalize_annotation_box(
        AnnotationBox{capture_x - radius, capture_y - radius, capture_x + radius + 1, capture_y + radius + 1},
        capture_width,
        capture_height);
    if (!annotation_box_has_area(stamp_box)) {
        return false;
    }

    std::optional<EditableMaskState> state = prepare_mask_edit(object, capture_width, capture_height);
    if (!state.has_value()) {
        if (erase) {
            return false;
        }
        state = make_empty_mask_state();
    }
    if (!erase) {
        ensure_mask_bounds(&*state, stamp_box, capture_width, capture_height);
    } else if (!mask_region_valid(state->region) || state->mask.empty()) {
        return false;
    }
    paint_circle(&*state, capture_x, capture_y, radius, erase);
    trim_mask_state(&*state);
    write_mask_state_to_object(object, std::move(*state));
    return true;
}

bool fill_annotation_object_mask(AnnotationObject* object,
                                 const int capture_x,
                                 const int capture_y,
                                 const std::uint32_t capture_width,
                                 const std::uint32_t capture_height) {
    std::optional<EditableMaskState> state = prepare_mask_edit(object, capture_width, capture_height);
    if (!state.has_value() || !mask_region_valid(state->region) || state->mask.empty()) {
        return false;
    }
    const int local_x = capture_x - static_cast<int>(state->region.capture_x);
    const int local_y = capture_y - static_cast<int>(state->region.capture_y);
    if (local_x < 0 || local_y < 0 ||
        local_x >= static_cast<int>(state->region.width) ||
        local_y >= static_cast<int>(state->region.height)) {
        return false;
    }
    const std::size_t seed_index =
        static_cast<std::size_t>(local_y) * static_cast<std::size_t>(state->region.width) + static_cast<std::size_t>(local_x);
    if (state->mask[seed_index] != 0U) {
        return false;
    }
    fill_connected_component(&state->mask, state->region.width, state->region.height, local_x, local_y, 0U, 1U);
    trim_mask_state(&*state);
    write_mask_state_to_object(object, std::move(*state));
    return true;
}

bool cleanup_annotation_object_mask(AnnotationObject* object,
                                    const AnnotationMaskCleanupOp op,
                                    int radius,
                                    const std::uint32_t capture_width,
                                    const std::uint32_t capture_height) {
    std::optional<EditableMaskState> state = prepare_mask_edit(object, capture_width, capture_height);
    if (!state.has_value() || !mask_region_valid(state->region) || state->mask.empty()) {
        return false;
    }
    radius = std::max(radius, 1);

    switch (op) {
    case AnnotationMaskCleanupOp::LargestComponent:
        state->mask = keep_largest_component(std::move(state->mask), state->region.width, state->region.height);
        break;
    case AnnotationMaskCleanupOp::FillHoles:
        state->mask = fill_holes(std::move(state->mask), state->region.width, state->region.height);
        break;
    case AnnotationMaskCleanupOp::Dilate:
        state->mask = dilate_mask_square(state->mask, state->region.width, state->region.height, radius);
        break;
    case AnnotationMaskCleanupOp::Erode:
        state->mask = erode_mask_square(state->mask, state->region.width, state->region.height, radius);
        break;
    case AnnotationMaskCleanupOp::Open:
        state->mask =
            dilate_mask_square(erode_mask_square(state->mask, state->region.width, state->region.height, radius),
                               state->region.width,
                               state->region.height,
                               radius);
        break;
    case AnnotationMaskCleanupOp::Close:
        state->mask =
            erode_mask_square(dilate_mask_square(state->mask, state->region.width, state->region.height, radius),
                              state->region.width,
                              state->region.height,
                              radius);
        break;
    }

    trim_mask_state(&*state);
    write_mask_state_to_object(object, std::move(*state));
    return true;
}

} // namespace mmltk::gui
