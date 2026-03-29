#include "gui/annotation_geometry.h"

#include <cassert>
#include <vector>

namespace {

using namespace fastloader::gui;

AnnotationInstance make_box_instance(AnnotationBox box) {
    AnnotationInstance instance;
    instance.instance_id = "box";
    instance.box = box;
    instance.category_index = 0U;
    return instance;
}

void test_box_import_and_export_round_trip() {
    AnnotationGeometryDocument doc(-1, 32, 32);
    doc.import_instances({make_box_instance(AnnotationBox{4, 5, 9, 11})});

    const std::optional<AnnotationBox> bbox = doc.instance_bbox(0);
    assert(bbox.has_value());
    assert(bbox->x1 == 4);
    assert(bbox->y1 == 5);
    assert(bbox->x2 == 9);
    assert(bbox->y2 == 11);

    AnnotationInstance exported = make_box_instance({});
    doc.export_instance(0, &exported);
    assert(exported.seed_kind == AnnotationSeedKind::ModelMask);
    assert(exported.seed_frame_id == 0U);
    assert(exported.box.x1 == 4);
    assert(exported.box.y1 == 5);
    assert(exported.box.x2 == 9);
    assert(exported.box.y2 == 11);
    assert(exported.seed_mask_region.capture_x == 4U);
    assert(exported.seed_mask_region.capture_y == 5U);
    assert(exported.seed_mask_region.width == 5U);
    assert(exported.seed_mask_region.height == 6U);
    assert(exported.seed_mask.size() == 30U);
    for (std::uint8_t value : exported.seed_mask) {
        assert(value == 1U);
    }
}

void test_paint_erase_and_fill_update_bbox() {
    AnnotationGeometryDocument doc(-1, 32, 32);
    doc.import_instances({make_box_instance(AnnotationBox{10, 10, 14, 14})});

    assert(doc.paint_instance(0, 16, 16, 1, false));
    std::optional<AnnotationBox> bbox = doc.instance_bbox(0);
    assert(bbox.has_value());
    assert(bbox->x1 == 10);
    assert(bbox->y1 == 10);
    assert(bbox->x2 == 18);
    assert(bbox->y2 == 18);

    assert(doc.paint_instance(0, 12, 12, 2, true));
    bbox = doc.instance_bbox(0);
    assert(bbox.has_value());
    assert(bbox->x1 == 10);
    assert(bbox->y1 == 10);
    assert(bbox->x2 >= 17);
    assert(bbox->y2 >= 17);

    AnnotationInstance exported = make_box_instance({});
    doc.export_instance(0, &exported);
    const std::size_t center_index =
        2U * static_cast<std::size_t>(exported.seed_mask_region.width) + 2U;
    assert(center_index < exported.seed_mask.size());
    assert(exported.seed_mask[center_index] == 0U);

    assert(doc.fill_instance(0, 12, 12));
    doc.export_instance(0, &exported);
    assert(exported.seed_mask[center_index] == 1U);
}

void test_move_resize_and_cleanup_ops() {
    AnnotationGeometryDocument doc(-1, 32, 32);
    AnnotationInstance masked = make_box_instance({});
    masked.seed_kind = AnnotationSeedKind::ModelMask;
    masked.seed_mask_region = AnnotationMaskRegion{4U, 4U, 5U, 5U};
    masked.seed_mask = {
        0, 1, 0, 0, 1,
        1, 1, 1, 0, 1,
        0, 1, 0, 0, 1,
        0, 0, 0, 1, 1,
        0, 0, 0, 0, 1,
    };
    doc.import_instances({masked});

    std::optional<AnnotationBox> bbox = doc.instance_bbox(0);
    assert(bbox.has_value());
    assert(bbox->x1 == 4);
    assert(bbox->y1 == 4);
    assert(bbox->x2 == 9);
    assert(bbox->y2 == 9);

    assert(doc.cleanup_instance(0, AnnotationGeometryCleanupOp::LargestComponent, 1));
    bbox = doc.instance_bbox(0);
    assert(bbox.has_value());
    assert(bbox->x1 == 7);
    assert(bbox->y1 == 4);
    assert(bbox->x2 == 9);
    assert(bbox->y2 == 9);

    assert(doc.move_instance(0, 3, 2));
    bbox = doc.instance_bbox(0);
    assert(bbox.has_value());
    assert(bbox->x1 == 10);
    assert(bbox->y1 == 6);
    assert(bbox->x2 == 12);
    assert(bbox->y2 == 11);

    assert(doc.resize_instance(0, AnnotationBox{10, 6, 15, 14}));
    bbox = doc.instance_bbox(0);
    assert(bbox.has_value());
    assert(bbox->x1 == 10);
    assert(bbox->y1 == 6);
    assert(bbox->x2 == 15);
    assert(bbox->y2 == 14);

    assert(doc.cleanup_instance(0, AnnotationGeometryCleanupOp::Dilate, 1));
    bbox = doc.instance_bbox(0);
    assert(bbox.has_value());
    assert(bbox->x1 <= 10);
    assert(bbox->y1 <= 6);
    assert(bbox->x2 >= 15);
    assert(bbox->y2 >= 14);
}

} // namespace

int main() {
    test_box_import_and_export_round_trip();
    test_paint_erase_and_fill_update_bbox();
    test_move_resize_and_cleanup_ops();
    return 0;
}
