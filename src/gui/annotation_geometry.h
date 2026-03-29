#pragma once

#include "annotation_core.h"

#include <torch/torch.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace fastloader::gui {

enum class AnnotationGeometryToolMode : int {
    Select = 0,
    Direct = 1,
    AddBox = 2,
    Paint = 3,
    Erase = 4,
    Fill = 5,
};

enum class AnnotationGeometryCleanupOp : int {
    LargestComponent = 0,
    FillHoles = 1,
    Dilate = 2,
    Erode = 3,
    Open = 4,
    Close = 5,
};

struct AnnotationGeometryInstance {
    AnnotationMaskRegion region{};
    AnnotationBox bbox{};
    torch::Tensor mask;
};

class AnnotationGeometryDocument {
public:
    AnnotationGeometryDocument(int device_id,
                               std::uint32_t capture_width,
                               std::uint32_t capture_height);

    int device_id() const { return device_id_; }
    std::uint32_t capture_width() const { return capture_width_; }
    std::uint32_t capture_height() const { return capture_height_; }
    const torch::Device& device() const { return device_; }

    void import_instances(const std::vector<AnnotationInstance>& instances);
    void sync_instance_from_annotation(std::size_t index, const AnnotationInstance& instance);
    void export_instance(std::size_t index, AnnotationInstance* instance) const;

    std::size_t size() const { return instances_.size(); }
    const AnnotationGeometryInstance* instance(std::size_t index) const;

    std::optional<AnnotationBox> instance_bbox(std::size_t index) const;
    bool set_instance_box(std::size_t index, const AnnotationBox& box);
    bool resize_instance(std::size_t index, const AnnotationBox& box);
    bool move_instance(std::size_t index, int dx, int dy);
    bool paint_instance(std::size_t index, int capture_x, int capture_y, int radius, bool erase);
    bool fill_instance(std::size_t index, int capture_x, int capture_y);
    bool cleanup_instance(std::size_t index, AnnotationGeometryCleanupOp op, int radius);

private:
    torch::Device device_ = torch::kCPU;
    int device_id_ = -1;
    std::uint32_t capture_width_ = 0;
    std::uint32_t capture_height_ = 0;
    std::vector<AnnotationGeometryInstance> instances_;
};

const char* annotation_geometry_tool_label(AnnotationGeometryToolMode mode);
const char* annotation_geometry_cleanup_label(AnnotationGeometryCleanupOp op);

} // namespace fastloader::gui
