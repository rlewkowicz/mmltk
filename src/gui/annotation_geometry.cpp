#include "annotation_geometry.h"

#include <ATen/TensorIndexing.h>
#include <torch/cuda.h>
#include <torch/nn/functional/pooling.h>
#include <torch/nn/functional/upsampling.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <queue>
#include <stdexcept>

namespace F = torch::nn::functional;

namespace fastloader::gui {

namespace {

using namespace torch::indexing;

bool mask_region_valid(const AnnotationMaskRegion& region) {
    return region.width > 0U && region.height > 0U;
}

bool boxes_equal(const AnnotationBox& lhs, const AnnotationBox& rhs) {
    return lhs.x1 == rhs.x1 &&
           lhs.y1 == rhs.y1 &&
           lhs.x2 == rhs.x2 &&
           lhs.y2 == rhs.y2;
}

AnnotationBox clamp_capture_box(AnnotationBox box, std::uint32_t capture_width, std::uint32_t capture_height) {
    box = normalize_annotation_box(box, capture_width, capture_height);
    box.x1 = std::clamp(box.x1, 0, static_cast<int>(capture_width));
    box.y1 = std::clamp(box.y1, 0, static_cast<int>(capture_height));
    box.x2 = std::clamp(box.x2, 0, static_cast<int>(capture_width));
    box.y2 = std::clamp(box.y2, 0, static_cast<int>(capture_height));
    if (box.x1 > box.x2) {
        std::swap(box.x1, box.x2);
    }
    if (box.y1 > box.y2) {
        std::swap(box.y1, box.y2);
    }
    return box;
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

torch::TensorOptions mask_tensor_options(const torch::Device& device) {
    return torch::TensorOptions().device(device).dtype(torch::kBool);
}

torch::Tensor empty_mask(const torch::Device& device, std::uint32_t width, std::uint32_t height) {
    return torch::zeros({static_cast<int64_t>(height), static_cast<int64_t>(width)}, mask_tensor_options(device))
        .contiguous();
}

std::vector<std::uint8_t> tensor_to_mask_bytes(const torch::Tensor& mask) {
    if (!mask.defined()) {
        return {};
    }
    const torch::Tensor cpu = mask.to(torch::TensorOptions().device(torch::kCPU).dtype(torch::kUInt8)).contiguous();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(cpu.numel()), 0U);
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), cpu.data_ptr<std::uint8_t>(), bytes.size());
    }
    return bytes;
}

torch::Tensor bytes_to_mask_tensor(const std::vector<std::uint8_t>& bytes,
                                   std::uint32_t width,
                                   std::uint32_t height,
                                   const torch::Device& device) {
    const std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (bytes.size() != expected) {
        throw std::runtime_error("annotation geometry mask byte count does not match its region");
    }
    if (expected == 0U) {
        return empty_mask(device, 1U, 1U);
    }
    const torch::Tensor cpu = torch::from_blob(const_cast<std::uint8_t*>(bytes.data()),
                                               {static_cast<int64_t>(height), static_cast<int64_t>(width)},
                                               torch::TensorOptions().device(torch::kCPU).dtype(torch::kUInt8))
                                  .clone();
    return cpu.to(mask_tensor_options(device)).contiguous();
}

std::optional<AnnotationBox> bbox_from_roi_mask(const AnnotationMaskRegion& region, const torch::Tensor& mask) {
    if (!mask_region_valid(region) || !mask.defined()) {
        return std::nullopt;
    }
    const std::vector<std::uint8_t> bytes = tensor_to_mask_bytes(mask);
    const std::optional<AnnotationBox> local_box = annotation_bbox_from_mask(bytes, region.width, region.height);
    if (!local_box.has_value()) {
        return std::nullopt;
    }
    return AnnotationBox{
        static_cast<int>(region.capture_x) + local_box->x1,
        static_cast<int>(region.capture_y) + local_box->y1,
        static_cast<int>(region.capture_x) + local_box->x2,
        static_cast<int>(region.capture_y) + local_box->y2,
    };
}

AnnotationGeometryInstance make_empty_instance(const torch::Device& device) {
    AnnotationGeometryInstance instance;
    instance.region = AnnotationMaskRegion{0U, 0U, 1U, 1U};
    instance.mask = empty_mask(device, 1U, 1U);
    instance.bbox = AnnotationBox{};
    return instance;
}

torch::Tensor circle_stamp(const torch::Device& device,
                           int width,
                           int height,
                           int center_x,
                           int center_y,
                           int radius) {
    const auto options = torch::TensorOptions().device(device).dtype(torch::kInt32);
    const torch::Tensor xs = torch::arange(width, options).view({1, width});
    const torch::Tensor ys = torch::arange(height, options).view({height, 1});
    const torch::Tensor dx = xs - center_x;
    const torch::Tensor dy = ys - center_y;
    return (dx * dx + dy * dy) <= (radius * radius);
}

torch::Tensor dilate_mask(const torch::Tensor& mask, int radius) {
    if (!mask.defined() || radius <= 0) {
        return mask.clone();
    }
    const int kernel = radius * 2 + 1;
    auto options = F::MaxPool2dFuncOptions(kernel).stride(1).padding(radius);
    return F::max_pool2d(mask.to(torch::kFloat32).unsqueeze(0).unsqueeze(0), options)
               .squeeze(0)
               .squeeze(0)
               .gt(0.0f)
               .contiguous();
}

torch::Tensor erode_mask(const torch::Tensor& mask, int radius) {
    if (!mask.defined() || radius <= 0) {
        return mask.clone();
    }
    return torch::logical_not(dilate_mask(torch::logical_not(mask), radius)).contiguous();
}

void trim_instance_to_mask(AnnotationGeometryInstance& instance,
                           const torch::Device& device,
                           std::uint32_t capture_width,
                           std::uint32_t capture_height) {
    if (!instance.mask.defined()) {
        instance = make_empty_instance(device);
        return;
    }

    const AnnotationBox capture_box = clamp_capture_box(box_from_mask_region(instance.region), capture_width, capture_height);
    instance.region = mask_region_from_box(capture_box);
    if (!mask_region_valid(instance.region)) {
        instance = make_empty_instance(device);
        return;
    }

    if (const std::optional<AnnotationBox> bbox = bbox_from_roi_mask(instance.region, instance.mask); bbox.has_value()) {
        const AnnotationBox local_box{
            bbox->x1 - static_cast<int>(instance.region.capture_x),
            bbox->y1 - static_cast<int>(instance.region.capture_y),
            bbox->x2 - static_cast<int>(instance.region.capture_x),
            bbox->y2 - static_cast<int>(instance.region.capture_y),
        };
        if (local_box.x1 > 0 || local_box.y1 > 0 ||
            local_box.x2 < static_cast<int>(instance.region.width) ||
            local_box.y2 < static_cast<int>(instance.region.height)) {
            instance.mask = instance.mask.index({
                                                Slice(local_box.y1, local_box.y2),
                                                Slice(local_box.x1, local_box.x2),
                                            }).contiguous();
            instance.region.capture_x += static_cast<std::uint32_t>(local_box.x1);
            instance.region.capture_y += static_cast<std::uint32_t>(local_box.y1);
            instance.region.width = static_cast<std::uint32_t>(local_box.x2 - local_box.x1);
            instance.region.height = static_cast<std::uint32_t>(local_box.y2 - local_box.y1);
        }
        instance.bbox = *bbox_from_roi_mask(instance.region, instance.mask);
        return;
    }

    const std::uint32_t origin_x = std::min(instance.region.capture_x, capture_width == 0U ? 0U : capture_width - 1U);
    const std::uint32_t origin_y = std::min(instance.region.capture_y, capture_height == 0U ? 0U : capture_height - 1U);
    instance.region = AnnotationMaskRegion{origin_x, origin_y, 1U, 1U};
    instance.mask = empty_mask(device, 1U, 1U);
    instance.bbox = AnnotationBox{};
}

void ensure_instance_bounds(AnnotationGeometryInstance& instance,
                            const torch::Device& device,
                            std::uint32_t capture_width,
                            std::uint32_t capture_height,
                            const AnnotationBox& required_box) {
    const AnnotationBox clamped_required = clamp_capture_box(required_box, capture_width, capture_height);
    if (!annotation_box_has_area(clamped_required)) {
        return;
    }

    if (!mask_region_valid(instance.region) || !instance.mask.defined()) {
        instance.region = mask_region_from_box(clamped_required);
        instance.mask = empty_mask(device, instance.region.width, instance.region.height);
        instance.bbox = AnnotationBox{};
        return;
    }

    const AnnotationBox current_box = box_from_mask_region(instance.region);
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
    torch::Tensor expanded = empty_mask(device, expanded_region.width, expanded_region.height);
    const int dx = static_cast<int>(instance.region.capture_x - expanded_region.capture_x);
    const int dy = static_cast<int>(instance.region.capture_y - expanded_region.capture_y);
    expanded.index_put_({
                            Slice(dy, dy + static_cast<int>(instance.region.height)),
                            Slice(dx, dx + static_cast<int>(instance.region.width)),
                        },
                        instance.mask);
    instance.region = expanded_region;
    instance.mask = expanded.contiguous();
}

AnnotationGeometryInstance import_instance(const AnnotationInstance& instance,
                                           const torch::Device& device,
                                           std::uint32_t capture_width,
                                           std::uint32_t capture_height) {
    AnnotationGeometryInstance geometry = make_empty_instance(device);
    if (instance.seed_kind == AnnotationSeedKind::ModelMask &&
        mask_region_valid(instance.seed_mask_region) &&
        instance.seed_mask.size() ==
            static_cast<std::size_t>(instance.seed_mask_region.width) *
                static_cast<std::size_t>(instance.seed_mask_region.height)) {
        geometry.region = instance.seed_mask_region;
        geometry.mask = bytes_to_mask_tensor(instance.seed_mask,
                                             geometry.region.width,
                                             geometry.region.height,
                                             device);
        trim_instance_to_mask(geometry, device, capture_width, capture_height);
        return geometry;
    }

    const AnnotationBox box = clamp_capture_box(instance.box, capture_width, capture_height);
    if (!annotation_box_has_area(box)) {
        return geometry;
    }

    geometry.region = mask_region_from_box(box);
    geometry.mask = torch::ones({static_cast<int64_t>(geometry.region.height), static_cast<int64_t>(geometry.region.width)},
                                mask_tensor_options(device))
                        .contiguous();
    geometry.bbox = box;
    return geometry;
}

void fill_connected_component(std::vector<std::uint8_t>& mask,
                              std::uint32_t width,
                              std::uint32_t height,
                              int seed_x,
                              int seed_y,
                              std::uint8_t target,
                              std::uint8_t replacement) {
    if (width == 0U || height == 0U) {
        return;
    }
    if (seed_x < 0 || seed_y < 0 || seed_x >= static_cast<int>(width) || seed_y >= static_cast<int>(height)) {
        return;
    }
    const std::size_t seed_index =
        static_cast<std::size_t>(seed_y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(seed_x);
    if (mask[seed_index] != target || target == replacement) {
        return;
    }

    std::queue<std::pair<int, int>> queue;
    queue.emplace(seed_x, seed_y);
    mask[seed_index] = replacement;
    constexpr std::array<std::pair<int, int>, 4> kOffsets{
        std::pair<int, int>{1, 0},
        std::pair<int, int>{-1, 0},
        std::pair<int, int>{0, 1},
        std::pair<int, int>{0, -1},
    };
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
            if (mask[index] != target) {
                continue;
            }
            mask[index] = replacement;
            queue.emplace(nx, ny);
        }
    }
}

std::vector<std::uint8_t> keep_largest_component(std::vector<std::uint8_t> mask,
                                                 std::uint32_t width,
                                                 std::uint32_t height) {
    if (width == 0U || height == 0U || mask.empty()) {
        return mask;
    }

    std::vector<std::uint8_t> labels(mask.size(), 0U);
    std::vector<std::size_t> sizes;
    sizes.push_back(0U);
    std::uint8_t next_label = 1U;
    constexpr std::array<std::pair<int, int>, 4> kOffsets{
        std::pair<int, int>{1, 0},
        std::pair<int, int>{-1, 0},
        std::pair<int, int>{0, 1},
        std::pair<int, int>{0, -1},
    };
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t start = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x;
            if (mask[start] == 0U || labels[start] != 0U) {
                continue;
            }
            if (next_label == std::numeric_limits<std::uint8_t>::max()) {
                throw std::runtime_error("annotation geometry largest-component label overflow");
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

std::vector<std::uint8_t> fill_holes(std::vector<std::uint8_t> mask, std::uint32_t width, std::uint32_t height) {
    if (width == 0U || height == 0U || mask.empty()) {
        return mask;
    }

    std::vector<std::uint8_t> outside(mask.size(), 0U);
    std::queue<std::pair<int, int>> queue;
    auto push_background = [&](int x, int y) {
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

    constexpr std::array<std::pair<int, int>, 4> kOffsets{
        std::pair<int, int>{1, 0},
        std::pair<int, int>{-1, 0},
        std::pair<int, int>{0, 1},
        std::pair<int, int>{0, -1},
    };
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

} // namespace

AnnotationGeometryDocument::AnnotationGeometryDocument(int device_id,
                                                       std::uint32_t capture_width,
                                                       std::uint32_t capture_height)
    : device_id_(device_id),
      capture_width_(capture_width),
      capture_height_(capture_height) {
    if (capture_width_ == 0U || capture_height_ == 0U) {
        throw std::runtime_error("annotation geometry capture dimensions must be positive");
    }
    if (device_id_ < 0) {
        device_ = torch::Device(torch::kCPU);
        return;
    }
    if (!torch::cuda::is_available()) {
        throw std::runtime_error("annotation geometry requested CUDA but CUDA is unavailable");
    }
    const auto device_count = torch::cuda::device_count();
    if (device_id_ >= device_count) {
        throw std::runtime_error("annotation geometry CUDA device index is out of range");
    }
    device_ = torch::Device(torch::kCUDA, static_cast<c10::DeviceIndex>(device_id_));
}

void AnnotationGeometryDocument::import_instances(const std::vector<AnnotationInstance>& instances) {
    instances_.clear();
    instances_.reserve(instances.size());
    for (const AnnotationInstance& instance : instances) {
        instances_.push_back(import_instance(instance, device_, capture_width_, capture_height_));
    }
}

void AnnotationGeometryDocument::sync_instance_from_annotation(std::size_t index, const AnnotationInstance& instance) {
    if (index > instances_.size()) {
        throw std::runtime_error("annotation geometry sync index is out of range");
    }
    AnnotationGeometryInstance geometry = import_instance(instance, device_, capture_width_, capture_height_);
    if (index == instances_.size()) {
        instances_.push_back(std::move(geometry));
    } else {
        instances_[index] = std::move(geometry);
    }
}

void AnnotationGeometryDocument::export_instance(std::size_t index, AnnotationInstance* instance) const {
    if (instance == nullptr) {
        throw std::runtime_error("annotation geometry export target must not be null");
    }
    const AnnotationGeometryInstance* geometry = this->instance(index);
    if (geometry == nullptr) {
        throw std::runtime_error("annotation geometry export index is out of range");
    }

    instance->box = geometry->bbox;
    instance->seed_kind = AnnotationSeedKind::ModelMask;
    instance->seed_mask_region = geometry->region;
    instance->seed_mask = tensor_to_mask_bytes(geometry->mask);
    instance->seed_frame_id = 0U;
}

const AnnotationGeometryInstance* AnnotationGeometryDocument::instance(std::size_t index) const {
    if (index >= instances_.size()) {
        return nullptr;
    }
    return &instances_[index];
}

std::optional<AnnotationBox> AnnotationGeometryDocument::instance_bbox(std::size_t index) const {
    const AnnotationGeometryInstance* geometry = instance(index);
    if (geometry == nullptr || !annotation_box_has_area(geometry->bbox)) {
        return std::nullopt;
    }
    return geometry->bbox;
}

bool AnnotationGeometryDocument::set_instance_box(std::size_t index, const AnnotationBox& box) {
    AnnotationGeometryInstance* geometry =
        index < instances_.size() ? &instances_[index] : nullptr;
    if (geometry == nullptr) {
        return false;
    }

    const AnnotationBox clamped = clamp_capture_box(box, capture_width_, capture_height_);
    if (!annotation_box_has_area(clamped)) {
        *geometry = make_empty_instance(device_);
        return true;
    }

    geometry->region = mask_region_from_box(clamped);
    geometry->mask = torch::ones({static_cast<int64_t>(geometry->region.height), static_cast<int64_t>(geometry->region.width)},
                                 mask_tensor_options(device_))
                         .contiguous();
    geometry->bbox = clamped;
    return true;
}

bool AnnotationGeometryDocument::resize_instance(std::size_t index, const AnnotationBox& box) {
    AnnotationGeometryInstance* geometry =
        index < instances_.size() ? &instances_[index] : nullptr;
    if (geometry == nullptr) {
        return false;
    }

    const AnnotationBox clamped = clamp_capture_box(box, capture_width_, capture_height_);
    if (!annotation_box_has_area(clamped)) {
        *geometry = make_empty_instance(device_);
        return true;
    }
    if (!geometry->mask.defined() || !mask_region_valid(geometry->region) || !annotation_box_has_area(geometry->bbox)) {
        return set_instance_box(index, clamped);
    }

    const AnnotationMaskRegion next_region = mask_region_from_box(clamped);
    auto options = F::InterpolateFuncOptions();
    options.size(std::vector<int64_t>{static_cast<int64_t>(next_region.height), static_cast<int64_t>(next_region.width)});
    options.mode(torch::kNearest);
    geometry->mask = F::interpolate(geometry->mask.to(torch::kFloat32).unsqueeze(0).unsqueeze(0), options)
                         .squeeze(0)
                         .squeeze(0)
                         .gt(0.0f)
                         .contiguous();
    geometry->region = next_region;
    trim_instance_to_mask(*geometry, device_, capture_width_, capture_height_);
    return true;
}

bool AnnotationGeometryDocument::move_instance(std::size_t index, int dx, int dy) {
    AnnotationGeometryInstance* geometry =
        index < instances_.size() ? &instances_[index] : nullptr;
    if (geometry == nullptr || !mask_region_valid(geometry->region) || !geometry->mask.defined()) {
        return false;
    }

    const AnnotationBox current_box = box_from_mask_region(geometry->region);
    const int width = current_box.x2 - current_box.x1;
    const int height = current_box.y2 - current_box.y1;
    const int new_x1 = std::clamp(current_box.x1 + dx, 0, static_cast<int>(capture_width_) - width);
    const int new_y1 = std::clamp(current_box.y1 + dy, 0, static_cast<int>(capture_height_) - height);
    const int offset_x = new_x1 - current_box.x1;
    const int offset_y = new_y1 - current_box.y1;
    if (offset_x == 0 && offset_y == 0) {
        return false;
    }
    geometry->region.capture_x = static_cast<std::uint32_t>(new_x1);
    geometry->region.capture_y = static_cast<std::uint32_t>(new_y1);
    if (annotation_box_has_area(geometry->bbox)) {
        geometry->bbox.x1 += offset_x;
        geometry->bbox.y1 += offset_y;
        geometry->bbox.x2 += offset_x;
        geometry->bbox.y2 += offset_y;
    }
    return true;
}

bool AnnotationGeometryDocument::paint_instance(std::size_t index,
                                                int capture_x,
                                                int capture_y,
                                                int radius,
                                                bool erase) {
    AnnotationGeometryInstance* geometry =
        index < instances_.size() ? &instances_[index] : nullptr;
    if (geometry == nullptr) {
        return false;
    }
    radius = std::max(radius, 1);
    const AnnotationBox stamp_box = clamp_capture_box(
        AnnotationBox{capture_x - radius, capture_y - radius, capture_x + radius + 1, capture_y + radius + 1},
        capture_width_,
        capture_height_);
    if (!annotation_box_has_area(stamp_box)) {
        return false;
    }
    if (!erase) {
        ensure_instance_bounds(*geometry, device_, capture_width_, capture_height_, stamp_box);
    } else if (!mask_region_valid(geometry->region) || !geometry->mask.defined()) {
        return false;
    }

    const int local_x1 = std::max(0, stamp_box.x1 - static_cast<int>(geometry->region.capture_x));
    const int local_y1 = std::max(0, stamp_box.y1 - static_cast<int>(geometry->region.capture_y));
    const int local_x2 = std::min(static_cast<int>(geometry->region.width),
                                  stamp_box.x2 - static_cast<int>(geometry->region.capture_x));
    const int local_y2 = std::min(static_cast<int>(geometry->region.height),
                                  stamp_box.y2 - static_cast<int>(geometry->region.capture_y));
    if (local_x1 >= local_x2 || local_y1 >= local_y2) {
        return false;
    }

    const torch::Tensor stamp = circle_stamp(device_,
                                             local_x2 - local_x1,
                                             local_y2 - local_y1,
                                             capture_x - static_cast<int>(geometry->region.capture_x) - local_x1,
                                             capture_y - static_cast<int>(geometry->region.capture_y) - local_y1,
                                             radius);
    torch::Tensor slice = geometry->mask.index({Slice(local_y1, local_y2), Slice(local_x1, local_x2)});
    if (erase) {
        slice = torch::logical_and(slice, torch::logical_not(stamp));
    } else {
        slice = torch::logical_or(slice, stamp);
    }
    geometry->mask.index_put_({Slice(local_y1, local_y2), Slice(local_x1, local_x2)}, slice);
    trim_instance_to_mask(*geometry, device_, capture_width_, capture_height_);
    return true;
}

bool AnnotationGeometryDocument::fill_instance(std::size_t index, int capture_x, int capture_y) {
    AnnotationGeometryInstance* geometry =
        index < instances_.size() ? &instances_[index] : nullptr;
    if (geometry == nullptr || !mask_region_valid(geometry->region) || !geometry->mask.defined()) {
        return false;
    }
    const int local_x = capture_x - static_cast<int>(geometry->region.capture_x);
    const int local_y = capture_y - static_cast<int>(geometry->region.capture_y);
    if (local_x < 0 || local_y < 0 ||
        local_x >= static_cast<int>(geometry->region.width) ||
        local_y >= static_cast<int>(geometry->region.height)) {
        return false;
    }

    std::vector<std::uint8_t> bytes = tensor_to_mask_bytes(geometry->mask);
    const std::size_t seed_index =
        static_cast<std::size_t>(local_y) * static_cast<std::size_t>(geometry->region.width) + static_cast<std::size_t>(local_x);
    if (bytes[seed_index] != 0U) {
        return false;
    }
    fill_connected_component(bytes, geometry->region.width, geometry->region.height, local_x, local_y, 0U, 1U);
    geometry->mask = bytes_to_mask_tensor(bytes, geometry->region.width, geometry->region.height, device_);
    trim_instance_to_mask(*geometry, device_, capture_width_, capture_height_);
    return true;
}

bool AnnotationGeometryDocument::cleanup_instance(std::size_t index, AnnotationGeometryCleanupOp op, int radius) {
    AnnotationGeometryInstance* geometry =
        index < instances_.size() ? &instances_[index] : nullptr;
    if (geometry == nullptr || !mask_region_valid(geometry->region) || !geometry->mask.defined()) {
        return false;
    }
    radius = std::max(radius, 1);

    switch (op) {
    case AnnotationGeometryCleanupOp::LargestComponent: {
        std::vector<std::uint8_t> bytes = keep_largest_component(
            tensor_to_mask_bytes(geometry->mask),
            geometry->region.width,
            geometry->region.height);
        geometry->mask = bytes_to_mask_tensor(bytes, geometry->region.width, geometry->region.height, device_);
        break;
    }
    case AnnotationGeometryCleanupOp::FillHoles: {
        std::vector<std::uint8_t> bytes = fill_holes(
            tensor_to_mask_bytes(geometry->mask),
            geometry->region.width,
            geometry->region.height);
        geometry->mask = bytes_to_mask_tensor(bytes, geometry->region.width, geometry->region.height, device_);
        break;
    }
    case AnnotationGeometryCleanupOp::Dilate:
        geometry->mask = dilate_mask(geometry->mask, radius);
        break;
    case AnnotationGeometryCleanupOp::Erode:
        geometry->mask = erode_mask(geometry->mask, radius);
        break;
    case AnnotationGeometryCleanupOp::Open:
        geometry->mask = dilate_mask(erode_mask(geometry->mask, radius), radius);
        break;
    case AnnotationGeometryCleanupOp::Close:
        geometry->mask = erode_mask(dilate_mask(geometry->mask, radius), radius);
        break;
    }
    trim_instance_to_mask(*geometry, device_, capture_width_, capture_height_);
    return true;
}

const char* annotation_geometry_tool_label(AnnotationGeometryToolMode mode) {
    switch (mode) {
    case AnnotationGeometryToolMode::Select:
        return "Select";
    case AnnotationGeometryToolMode::Direct:
        return "Direct";
    case AnnotationGeometryToolMode::AddBox:
        return "Add Box";
    case AnnotationGeometryToolMode::Paint:
        return "Paint";
    case AnnotationGeometryToolMode::Erase:
        return "Erase";
    case AnnotationGeometryToolMode::Fill:
        return "Fill";
    }
    return "Unknown";
}

const char* annotation_geometry_cleanup_label(AnnotationGeometryCleanupOp op) {
    switch (op) {
    case AnnotationGeometryCleanupOp::LargestComponent:
        return "Largest";
    case AnnotationGeometryCleanupOp::FillHoles:
        return "Fill Holes";
    case AnnotationGeometryCleanupOp::Dilate:
        return "Dilate";
    case AnnotationGeometryCleanupOp::Erode:
        return "Erode";
    case AnnotationGeometryCleanupOp::Open:
        return "Open";
    case AnnotationGeometryCleanupOp::Close:
        return "Close";
    }
    return "Unknown";
}

} // namespace fastloader::gui
