#include "annotation_core.h"
#include "gui/annotation/document/edit.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace mmltk::gui {

namespace {

using json = nlohmann::json;

constexpr std::string_view kAnnotationCategoriesSchemaVersion = "3.0";
constexpr std::string_view kAnnotationImageFormat = "png";
constexpr std::string_view kAnnotationBoxFormat = "xyxy_absolute_pixels";
constexpr std::string_view kAnnotationMaskFormat = "rle_row_major_start_length";
constexpr std::string_view kAnnotationBackgroundAnnotationPolicy = "empty_jsonl_file";
constexpr std::string_view kAnnotationMaskRleEncoding = "row_major_start_length";
constexpr std::array<std::string_view, 5> kAnnotationShapeTypes{
    "box",
    "mask",
    "spline",
    "point",
    "skeleton",
};

AnnotationShapeType annotation_shape_type_from_name(const std::string_view shape_type_name) {
    if (shape_type_name == "box") {
        return AnnotationShapeType::Box;
    }
    if (shape_type_name == "mask") {
        return AnnotationShapeType::Mask;
    }
    if (shape_type_name == "spline") {
        return AnnotationShapeType::Spline;
    }
    if (shape_type_name == "point") {
        return AnnotationShapeType::Point;
    }
    if (shape_type_name == "skeleton") {
        return AnnotationShapeType::Skeleton;
    }
    throw std::runtime_error("unsupported `shape_type` value `" + std::string(shape_type_name) + "`");
}

void validate_annotation_categories_meta(const json& meta) {
    const auto require_string = [&](const char* key, const std::string_view expected) {
        const auto field = meta.find(key);
        if (field == meta.end() || !field->is_string() || field->get<std::string>() != expected) {
            throw std::runtime_error("annotation categories meta: unexpected `" + std::string(key) + "`");
        }
    };
    const auto require_shape_types = [&]() {
        const auto field = meta.find("shape_types");
        if (field == meta.end() || !field->is_array() || field->size() != kAnnotationShapeTypes.size()) {
            throw std::runtime_error("annotation categories meta: unexpected `shape_types`");
        }
        for (std::size_t index = 0; index < kAnnotationShapeTypes.size(); ++index) {
            if (!(*field)[index].is_string() ||
                (*field)[index].get<std::string>() != kAnnotationShapeTypes[index]) {
                throw std::runtime_error("annotation categories meta: unexpected `shape_types`");
            }
        }
    };

    require_string("version", kAnnotationCategoriesSchemaVersion);
    require_string("image_format", kAnnotationImageFormat);
    require_string("bbox_format", kAnnotationBoxFormat);
    require_string("mask_format", kAnnotationMaskFormat);
    require_string("background_annotation_policy", kAnnotationBackgroundAnnotationPolicy);
    require_shape_types();
}

bool all_digits(std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](const unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::uint32_t next_scene_index(const std::filesystem::path& split_dir) {
    std::uint32_t maximum = 0;
    if (!std::filesystem::exists(split_dir)) {
        return 1;
    }
    for (const auto& entry : std::filesystem::directory_iterator(split_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".png") {
            continue;
        }
        const std::string stem = entry.path().stem().string();
        if (!all_digits(stem)) {
            continue;
        }
        maximum = std::max(maximum, static_cast<std::uint32_t>(std::stoul(stem)));
    }
    return maximum + 1U;
}

std::string format_scene_name(const std::uint32_t scene_index) {
    std::array<char, 32> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%06u", scene_index);
    if (written <= 0) {
        throw std::runtime_error("failed to format annotation scene index");
    }
    return {buffer.data(), static_cast<std::size_t>(written)};
}

json category_json(const AnnotationCategory& category) {
    json payload{
        {"id", category.id},
        {"name", category.name},
    };
    if (!category.keypoints.empty()) {
        payload["keypoints"] = category.keypoints;
    }
    if (!category.skeleton_edges.empty()) {
        payload["skeleton_edges"] = json::array();
        for (const AnnotationCategorySkeletonEdge& edge : category.skeleton_edges) {
            payload["skeleton_edges"].push_back({edge.source_index, edge.target_index});
        }
    }
    return payload;
}

json category_split_stats(const std::filesystem::path& split_dir) {
    std::uint64_t total = 0;
    if (std::filesystem::exists(split_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(split_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".png") {
                ++total;
            }
        }
    }
    return json{
        {"total", total},
        {"background", 0},
        {"annotated", total},
    };
}

void append_jsonl(const std::filesystem::path& path, const json& entry) {
    std::ofstream stream(path, std::ios::app);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to open JSONL manifest: " + path.string());
    }
    stream << entry.dump() << '\n';
}

json point_to_json(const AnnotationPoint& point) {
    return json::array({point.x, point.y});
}

json box_to_json(const AnnotationBox& box) {
    return json::array({box.x1, box.y1, box.x2, box.y2});
}

json serialize_annotation_shape_payload(const AnnotationFrame& frame, const AnnotationObject& object) {
    return std::visit(
        [&frame](const auto& shape) -> json {
            using T = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<T, AnnotationBoxShape> ||
                          std::is_same_v<T, AnnotationMaskShape>) {
                return json{
                    {"box_xyxy", box_to_json(annotation_box_to_frame(frame, shape.box))},
                };
            } else if constexpr (std::is_same_v<T, AnnotationPointShape>) {
                return json{
                    {"xy", point_to_json(annotation_capture_point_to_frame_unclipped(frame, shape.point))},
                };
            } else if constexpr (std::is_same_v<T, AnnotationSplineShape>) {
                json knots = json::array();
                for (const AnnotationSplineKnot& knot : shape.knots) {
                    json payload{
                        {"xy", point_to_json(annotation_capture_point_to_frame_unclipped(frame, knot.position))},
                        {"handle_mode", annotation_spline_handle_mode_name(knot.handle_mode)},
                    };
                    if (knot.in_handle.enabled) {
                        payload["in_handle_xy"] =
                            point_to_json(annotation_capture_point_to_frame_unclipped(frame, knot.in_handle.position));
                    }
                    if (knot.out_handle.enabled) {
                        payload["out_handle_xy"] =
                            point_to_json(annotation_capture_point_to_frame_unclipped(frame, knot.out_handle.position));
                    }
                    knots.push_back(std::move(payload));
                }
                return json{
                    {"closed", shape.closed},
                    {"knots", std::move(knots)},
                };
            } else if constexpr (std::is_same_v<T, AnnotationSkeletonShape>) {
                json nodes = json::array();
                for (const AnnotationSkeletonNode& node : shape.nodes) {
                    nodes.push_back(json{
                        {"key", node.key},
                        {"visible", node.visible},
                        {"xy", point_to_json(annotation_capture_point_to_frame_unclipped(frame, node.point))},
                    });
                }
                json edges = json::array();
                for (const AnnotationSkeletonEdge& edge : shape.edges) {
                    edges.push_back({edge.source_index, edge.target_index});
                }
                return json{
                    {"nodes", std::move(nodes)},
                    {"edges", std::move(edges)},
                };
            } else {
                return json::object();
            }
        },
        object.shape);
}

std::optional<AnnotationPoint> annotation_point_from_json(const json& value) {
    if (!value.is_array() || value.size() != 2U) {
        return std::nullopt;
    }
    return AnnotationPoint{
        value.at(0).get<float>(),
        value.at(1).get<float>(),
    };
}

std::optional<AnnotationBox> annotation_box_from_json(const json& value) {
    if (!value.is_array() || value.size() != 4U) {
        return std::nullopt;
    }
    return AnnotationBox{
        value.at(0).get<int>(),
        value.at(1).get<int>(),
        value.at(2).get<int>(),
        value.at(3).get<int>(),
    };
}

std::optional<AnnotationBox> frame_box_to_capture_box(const AnnotationBox& frame_box,
                                                      const std::uint32_t view_x,
                                                      const std::uint32_t view_y,
                                                      const std::uint32_t capture_width,
                                                      const std::uint32_t capture_height) {
    return normalize_annotation_box(
        AnnotationBox{
            frame_box.x1 + static_cast<int>(view_x),
            frame_box.y1 + static_cast<int>(view_y),
            frame_box.x2 + static_cast<int>(view_x),
            frame_box.y2 + static_cast<int>(view_y),
        },
        capture_width,
        capture_height);
}

std::vector<std::uint8_t> trim_dense_mask_to_bbox(const std::vector<std::uint8_t>& dense_mask,
                                                  const std::uint32_t image_width,
                                                  const AnnotationBox& bbox) {
    const std::uint32_t trimmed_width = static_cast<std::uint32_t>(std::max(0, bbox.x2 - bbox.x1));
    const std::uint32_t trimmed_height = static_cast<std::uint32_t>(std::max(0, bbox.y2 - bbox.y1));
    std::vector<std::uint8_t> trimmed(static_cast<std::size_t>(trimmed_width) *
                                          static_cast<std::size_t>(trimmed_height),
                                      0U);
    for (std::uint32_t row = 0; row < trimmed_height; ++row) {
        const std::size_t src_offset =
            static_cast<std::size_t>(bbox.y1 + static_cast<int>(row)) * static_cast<std::size_t>(image_width) +
            static_cast<std::size_t>(bbox.x1);
        const std::size_t dst_offset =
            static_cast<std::size_t>(row) * static_cast<std::size_t>(trimmed_width);
        std::copy_n(dense_mask.begin() + static_cast<std::ptrdiff_t>(src_offset),
                    trimmed_width,
                    trimmed.begin() + static_cast<std::ptrdiff_t>(dst_offset));
    }
    return trimmed;
}

AnnotationObject annotation_object_from_scene_record(const json& record,
                                                     AnnotationCategories* categories) {
    const auto fail = [](const std::string& message) -> void {
        throw std::runtime_error("annotation scene record: " + message);
    };
    const auto required_array_field =
        [&](const char* key, const std::size_t expected_size) -> const json& {
        const auto it = record.find(key);
        if (it == record.end() || !it->is_array() || it->size() != expected_size) {
            fail("missing required array field `" + std::string(key) + "`");
        }
        return *it;
    };
    const auto required_shape_object = [&]() -> const json& {
        const auto it = record.find("shape");
        if (it == record.end() || !it->is_object()) {
            fail("missing required object field `shape`");
        }
        return *it;
    };

    const std::string class_name = record.value("class", std::string{});
    const std::size_t category_index =
        categories != nullptr ? ensure_annotation_category(*categories, class_name) : 0U;

    const auto shape_type_it = record.find("shape_type");
    if (shape_type_it == record.end() || !shape_type_it->is_string()) {
        fail("missing required string field `shape_type`");
    }
    const std::string shape_type_name = shape_type_it->get<std::string>();
    const AnnotationShapeType shape_type = annotation_shape_type_from_name(shape_type_name);

    const json& image_size = required_array_field("image_size_wh", 2U);
    const std::uint32_t image_width = image_size.at(0).get<std::uint32_t>();
    const std::uint32_t image_height = image_size.at(1).get<std::uint32_t>();
    const json& view_origin = required_array_field("view_origin_xy", 2U);
    const std::uint32_t view_x = view_origin.at(0).get<std::uint32_t>();
    const std::uint32_t view_y = view_origin.at(1).get<std::uint32_t>();
    const json& capture_size = required_array_field("capture_size_wh", 2U);
    const std::uint32_t capture_width = capture_size.at(0).get<std::uint32_t>();
    const std::uint32_t capture_height = capture_size.at(1).get<std::uint32_t>();

    AnnotationObject object;
    object.object_id.clear();
    object.category_index = category_index;

    switch (shape_type) {
    case AnnotationShapeType::Point: {
        const json& shape_payload = required_shape_object();
        const auto xy = shape_payload.find("xy");
        if (xy == shape_payload.end()) {
            fail("point record is missing `shape.xy`");
        }
        const std::optional<AnnotationPoint> point = annotation_point_from_json(*xy);
        if (!point.has_value()) {
            fail("point record has invalid `shape.xy`");
        }
        const AnnotationPoint point_value = point.value_or(AnnotationPoint{});
        object.shape = AnnotationPointShape{
            annotation_frame_point_to_capture_unclipped(point_value, view_x, view_y),
        };
        break;
    }
    case AnnotationShapeType::Spline: {
        const json& shape_payload = required_shape_object();
        const auto knots = shape_payload.find("knots");
        if (knots == shape_payload.end() || !knots->is_array()) {
            fail("spline record is missing array `shape.knots`");
        }
        AnnotationSplineShape spline;
        spline.closed = shape_payload.value("closed", false);
        spline.knots.reserve(knots->size());
        for (const auto& knot_entry : *knots) {
            if (!knot_entry.is_object()) {
                fail("spline record contains a non-object knot");
            }
            const auto knot_xy = knot_entry.find("xy");
            if (knot_xy == knot_entry.end()) {
                fail("spline knot is missing `xy`");
            }
            const std::optional<AnnotationPoint> knot_point = annotation_point_from_json(*knot_xy);
            if (!knot_point.has_value()) {
                fail("spline knot has invalid `xy`");
            }
            const AnnotationPoint knot_point_value =
                knot_point.value_or(AnnotationPoint{});

            AnnotationSplineKnot knot;
            knot.position = annotation_frame_point_to_capture_unclipped(knot_point_value, view_x, view_y);
            knot.handle_mode = annotation_spline_handle_mode_from_name(
                knot_entry.value("handle_mode", std::string("corner")));
            if (const auto in_handle = knot_entry.find("in_handle_xy");
                in_handle != knot_entry.end()) {
                const std::optional<AnnotationPoint> handle_point = annotation_point_from_json(*in_handle);
                if (!handle_point.has_value()) {
                    fail("spline knot has invalid `in_handle_xy`");
                }
                const AnnotationPoint handle_point_value =
                    handle_point.value_or(AnnotationPoint{});
                knot.in_handle.position =
                    annotation_frame_point_to_capture_unclipped(handle_point_value, view_x, view_y);
                knot.in_handle.enabled = true;
            }
            if (const auto out_handle = knot_entry.find("out_handle_xy");
                out_handle != knot_entry.end()) {
                const std::optional<AnnotationPoint> handle_point = annotation_point_from_json(*out_handle);
                if (!handle_point.has_value()) {
                    fail("spline knot has invalid `out_handle_xy`");
                }
                const AnnotationPoint handle_point_value =
                    handle_point.value_or(AnnotationPoint{});
                knot.out_handle.position =
                    annotation_frame_point_to_capture_unclipped(handle_point_value, view_x, view_y);
                knot.out_handle.enabled = true;
            }
            spline.knots.push_back(knot);
        }
        object.shape = std::move(spline);
        break;
    }
    case AnnotationShapeType::Skeleton: {
        const json& shape_payload = required_shape_object();
        const auto nodes = shape_payload.find("nodes");
        if (nodes == shape_payload.end() || !nodes->is_array()) {
            fail("skeleton record is missing array `shape.nodes`");
        }
        const auto edges = shape_payload.find("edges");
        if (edges == shape_payload.end() || !edges->is_array()) {
            fail("skeleton record is missing array `shape.edges`");
        }
        AnnotationSkeletonShape skeleton;
        skeleton.nodes.reserve(nodes->size());
        for (const auto& node_entry : *nodes) {
            if (!node_entry.is_object()) {
                fail("skeleton record contains a non-object node");
            }
            const auto node_xy = node_entry.find("xy");
            if (node_xy == node_entry.end()) {
                fail("skeleton node is missing `xy`");
            }
            const std::optional<AnnotationPoint> node_point = annotation_point_from_json(*node_xy);
            if (!node_point.has_value()) {
                fail("skeleton node has invalid `xy`");
            }
            const AnnotationPoint node_point_value =
                node_point.value_or(AnnotationPoint{});
            const auto key = node_entry.find("key");
            if (key == node_entry.end() || !key->is_string()) {
                fail("skeleton node is missing string `key`");
            }
            skeleton.nodes.push_back(AnnotationSkeletonNode{
                key->get<std::string>(),
                annotation_frame_point_to_capture_unclipped(node_point_value, view_x, view_y),
                node_entry.value("visible", true),
            });
        }
        skeleton.edges.reserve(edges->size());
        for (const auto& edge_entry : *edges) {
            if (!edge_entry.is_array() || edge_entry.size() != 2U) {
                fail("skeleton edge must contain exactly two indices");
            }
            skeleton.edges.push_back(AnnotationSkeletonEdge{
                edge_entry.at(0).get<std::size_t>(),
                edge_entry.at(1).get<std::size_t>(),
            });
        }
        object.shape = std::move(skeleton);
        if (categories != nullptr && object.category_index < categories->items.size()) {
            AnnotationCategory& category = categories->items[object.category_index];
            if (category.keypoints.empty()) {
                category.keypoints.reserve(std::get<AnnotationSkeletonShape>(object.shape).nodes.size());
                for (const AnnotationSkeletonNode& node : std::get<AnnotationSkeletonShape>(object.shape).nodes) {
                    category.keypoints.push_back(node.key);
                }
            }
            if (category.skeleton_edges.empty()) {
                category.skeleton_edges.reserve(std::get<AnnotationSkeletonShape>(object.shape).edges.size());
                for (const AnnotationSkeletonEdge& edge : std::get<AnnotationSkeletonShape>(object.shape).edges) {
                    category.skeleton_edges.push_back(AnnotationCategorySkeletonEdge{
                        edge.source_index,
                        edge.target_index,
                    });
                }
            }
        }
        break;
    }
    case AnnotationShapeType::Mask: {
        const json& shape_payload = required_shape_object();
        const auto shape_box_xyxy = shape_payload.find("box_xyxy");
        if (shape_box_xyxy == shape_payload.end()) {
            fail("mask record is missing `shape.box_xyxy`");
        }
        const std::optional<AnnotationBox> shape_frame_box =
            annotation_box_from_json(*shape_box_xyxy);
        if (!shape_frame_box.has_value()) {
            fail("mask record has invalid `shape.box_xyxy`");
        }
        const AnnotationBox shape_frame_box_value =
            shape_frame_box.value_or(AnnotationBox{});
        const std::optional<AnnotationBox> shape_capture_box =
            frame_box_to_capture_box(shape_frame_box_value, view_x, view_y, capture_width, capture_height);
        if (!shape_capture_box.has_value()) {
            fail("mask shape box falls outside the declared capture bounds");
        }
        const AnnotationBox shape_capture_box_value =
            shape_capture_box.value_or(AnnotationBox{});
        const auto mask_rle = record.find("mask_rle");
        if (mask_rle == record.end() || !mask_rle->is_string()) {
            fail("mask record is missing string `mask_rle`");
        }
        const auto mask_rle_encoding = record.find("mask_rle_encoding");
        if (mask_rle_encoding == record.end() || !mask_rle_encoding->is_string() ||
            mask_rle_encoding->get<std::string>() != kAnnotationMaskRleEncoding) {
            fail("mask record is missing `mask_rle_encoding` value `" + std::string(kAnnotationMaskRleEncoding) + "`");
        }
        const std::vector<std::uint8_t> dense_mask =
            decode_annotation_mask_rle(mask_rle->get<std::string>(), image_width, image_height);
        object.shape = AnnotationMaskShape{
            shape_capture_box_value,
            AnnotationMaskRegion{
                static_cast<std::uint32_t>(shape_capture_box_value.x1),
                static_cast<std::uint32_t>(shape_capture_box_value.y1),
                static_cast<std::uint32_t>(std::max(0, shape_capture_box_value.x2 - shape_capture_box_value.x1)),
                static_cast<std::uint32_t>(std::max(0, shape_capture_box_value.y2 - shape_capture_box_value.y1)),
            },
            trim_dense_mask_to_bbox(dense_mask, image_width, shape_frame_box_value),
            0U,
            std::nullopt,
        };
        break;
    }
    case AnnotationShapeType::Box: {
        const json& shape_payload = required_shape_object();
        const auto shape_box_xyxy = shape_payload.find("box_xyxy");
        if (shape_box_xyxy == shape_payload.end()) {
            fail("box record is missing `shape.box_xyxy`");
        }
        const std::optional<AnnotationBox> shape_frame_box =
            annotation_box_from_json(*shape_box_xyxy);
        if (!shape_frame_box.has_value()) {
            fail("box record has invalid `shape.box_xyxy`");
        }
        const AnnotationBox shape_frame_box_value =
            shape_frame_box.value_or(AnnotationBox{});
        const std::optional<AnnotationBox> shape_capture_box =
            frame_box_to_capture_box(shape_frame_box_value, view_x, view_y, capture_width, capture_height);
        if (!shape_capture_box.has_value()) {
            fail("box shape box falls outside the declared capture bounds");
        }
        const AnnotationBox shape_capture_box_value =
            shape_capture_box.value_or(AnnotationBox{});
        object.shape = AnnotationBoxShape{shape_capture_box_value};
        break;
    }
    }

    return object;
}

std::string normalized_path_string(const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }
    try {
        return std::filesystem::weakly_canonical(path).lexically_normal().string();
    } catch (const std::exception&) {
        return path.lexically_normal().string();
    }
}

} // namespace

AnnotationCategories load_annotation_categories(const std::filesystem::path& output_root) {
    AnnotationCategories categories;
    if (!output_root.empty() && output_root.filename() != ".") {
        categories.dataset_name = output_root.filename().string();
    }
    const std::filesystem::path categories_path = output_root / "categories.json";
    if (!std::filesystem::exists(categories_path)) {
        return categories;
    }
    std::ifstream stream(categories_path);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to open annotation categories: " + categories_path.string());
    }
    const json parsed = json::parse(stream);
    const auto meta = parsed.find("meta");
    if (meta == parsed.end() || !meta->is_object()) {
        throw std::runtime_error("annotation categories file is missing object `meta`");
    }
    validate_annotation_categories_meta(*meta);
    if (const auto dataset_name = meta->find("dataset_name");
        dataset_name != meta->end() && dataset_name->is_string()) {
        categories.dataset_name = dataset_name->get<std::string>();
    }

    const auto classes = parsed.find("classes");
    if (classes == parsed.end() || !classes->is_array()) {
        throw std::runtime_error("annotation categories file is missing array `classes`");
    }
    categories.items.clear();
    for (const auto& entry : *classes) {
        AnnotationCategory category;
        category.id = entry.value("id", static_cast<int>(categories.items.size()) + 1);
        category.name = entry.value("name", std::string{});
        if (const auto keypoints = entry.find("keypoints"); keypoints != entry.end() && keypoints->is_array()) {
            category.keypoints = keypoints->get<std::vector<std::string>>();
        }
        if (const auto edges = entry.find("skeleton_edges"); edges != entry.end() && edges->is_array()) {
            for (const auto& edge : *edges) {
                if (!edge.is_array() || edge.size() != 2U) {
                    continue;
                }
                category.skeleton_edges.push_back(AnnotationCategorySkeletonEdge{
                    edge.at(0).get<std::size_t>(),
                    edge.at(1).get<std::size_t>(),
                });
            }
        }
        categories.items.push_back(std::move(category));
    }
    return categories;
}

std::size_t ensure_annotation_category(AnnotationCategories& categories, const std::string& class_name) {
    const auto found = std::find_if(categories.items.begin(), categories.items.end(), [&](const AnnotationCategory& item) {
        return item.name == class_name;
    });
    if (found != categories.items.end()) {
        return static_cast<std::size_t>(std::distance(categories.items.begin(), found));
    }
    const int next_id = categories.items.empty() ? 1 : categories.items.back().id + 1;
    categories.items.push_back(AnnotationCategory{next_id, class_name});
    return categories.items.size() - 1U;
}

void write_annotation_categories(const std::filesystem::path& output_root,
                                 const AnnotationCategories& categories) {
    std::filesystem::create_directories(output_root);
    json payload;
    payload["meta"] = {
        {"dataset_name", categories.dataset_name.empty() ? output_root.filename().string() : categories.dataset_name},
        {"version", kAnnotationCategoriesSchemaVersion},
        {"image_format", kAnnotationImageFormat},
        {"bbox_format", kAnnotationBoxFormat},
        {"mask_format", kAnnotationMaskFormat},
        {"shape_types", kAnnotationShapeTypes},
        {"background_annotation_policy", kAnnotationBackgroundAnnotationPolicy},
    };
    payload["classes"] = json::array();
    for (const AnnotationCategory& category : categories.items) {
        payload["classes"].push_back(category_json(category));
    }

    json splits = json::object();
    for (const char* split_name : {"train", "val", "test"}) {
        const std::filesystem::path split_dir = output_root / split_name;
        if (!std::filesystem::exists(split_dir)) {
            continue;
        }
        splits[split_name] = category_split_stats(split_dir);
    }
    if (!splits.empty()) {
        payload["splits"] = std::move(splits);
    }

    const std::filesystem::path categories_path = output_root / "categories.json";
    std::ofstream stream(categories_path, std::ios::trunc);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to write annotation categories: " + categories_path.string());
    }
    stream << payload.dump(2) << '\n';
}

std::vector<AnnotationObject> load_annotation_scene_objects(const std::filesystem::path& scene_jsonl_path,
                                                            AnnotationCategories* categories) {
    std::ifstream stream(scene_jsonl_path);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to open annotation scene JSONL: " + scene_jsonl_path.string());
    }

    std::vector<AnnotationObject> objects;
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        const json record = json::parse(line);
        AnnotationObject object = annotation_object_from_scene_record(record, categories);
        object.object_id = next_annotation_object_id(objects.size());
        objects.push_back(std::move(object));
    }
    return objects;
}

std::optional<std::vector<AnnotationObject>> load_saved_annotation_scene_for_frame(
    const std::filesystem::path& output_root,
    const AnnotationFrame& frame,
    AnnotationCategories* categories) {
    const std::filesystem::path manifest_path = output_root / "manifests" / "scenes.jsonl";
    if (!std::filesystem::exists(manifest_path)) {
        return std::nullopt;
    }

    std::ifstream stream(manifest_path);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to open annotation scenes manifest: " + manifest_path.string());
    }

    const std::string target_source_path = normalized_path_string(frame.source_path);
    std::uint32_t best_scene_index = 0U;
    std::filesystem::path best_scene_jsonl_path;
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        const json entry = json::parse(line);
        const std::filesystem::path source_path = entry.value("source_path", std::string{});
        const std::uint64_t frame_id = entry.value("frame_id", 0ULL);
        if (frame_id != frame.frame_id) {
            continue;
        }
        if (!target_source_path.empty() &&
            normalized_path_string(source_path) != target_source_path) {
            continue;
        }
        const std::uint32_t scene_index = entry.value("scene_index", 0U);
        if (scene_index < best_scene_index) {
            continue;
        }
        const std::filesystem::path relative_scene_jsonl =
            entry.value("scene_jsonl", std::string{});
        if (relative_scene_jsonl.empty()) {
            continue;
        }
        best_scene_index = scene_index;
        best_scene_jsonl_path = output_root / relative_scene_jsonl;
    }

    if (best_scene_jsonl_path.empty()) {
        return std::nullopt;
    }
    return load_annotation_scene_objects(best_scene_jsonl_path, categories);
}

AnnotationSaveResult save_annotation_scene(const AnnotationSaveConfig& config,
                                           const AnnotationFrame& frame,
                                           AnnotationCategories& categories,
                                           const std::vector<AnnotationObject>& objects,
                                           const bool live_mode,
                                           const AnnotationProjectedScene* projected_scene) {
    if (config.output_root.empty()) {
        throw std::runtime_error("annotation output root must not be empty");
    }
    const std::vector<AnnotationResolvedObject> resolved_objects =
        resolve_annotation_objects(frame, categories, objects, live_mode, projected_scene);
    std::filesystem::create_directories(config.output_root);
    const std::filesystem::path split_dir = config.output_root / config.split;
    const std::filesystem::path entity_dir = config.output_root / "entities";
    const std::filesystem::path manifest_dir = config.output_root / "manifests";
    std::filesystem::create_directories(split_dir);
    std::filesystem::create_directories(entity_dir);
    std::filesystem::create_directories(manifest_dir);

    const std::uint32_t scene_index = next_scene_index(split_dir);
    const std::string scene_stem = format_scene_name(scene_index);
    const std::filesystem::path scene_image_path = split_dir / (scene_stem + ".png");
    const std::filesystem::path scene_jsonl_path = split_dir / (scene_stem + ".jsonl");

    write_annotation_frame_png(scene_image_path, frame);

    std::ofstream scene_stream(scene_jsonl_path, std::ios::trunc);
    if (!scene_stream.is_open()) {
        throw std::runtime_error("failed to write annotation JSONL: " + scene_jsonl_path.string());
    }

    AnnotationSaveResult result;
    result.scene_image_path = scene_image_path;
    result.scene_jsonl_path = scene_jsonl_path;
    result.scene_index = scene_index;
    result.entity_paths.reserve(resolved_objects.size());

    for (std::size_t index = 0; index < resolved_objects.size(); ++index) {
        const AnnotationResolvedObject& resolved = resolved_objects[index];
        if (resolved.object_index >= objects.size()) {
            throw std::runtime_error("resolved annotation object index is out of range");
        }
        const AnnotationObject& object = objects[resolved.object_index];
        if (resolved.category_index >= categories.items.size()) {
            throw std::runtime_error("resolved annotation object category index is out of range");
        }
        const AnnotationCategory& category = categories.items[resolved.category_index];

        json record{
            {"class", category.name},
            {"shape_type", annotation_shape_type_name(resolved.shape_type)},
            {"image_size_wh", {frame.width, frame.height}},
            {"view_origin_xy", {frame.view_x, frame.view_y}},
            {"capture_size_wh", {annotation_frame_capture_width(frame), annotation_frame_capture_height(frame)}},
        };
        if (annotation_box_has_area(resolved.bbox)) {
            record["bbox_xyxy"] = {resolved.bbox.x1, resolved.bbox.y1, resolved.bbox.x2, resolved.bbox.y2};
        }
        if (!resolved.mask_rle.empty()) {
            record["mask_rle_encoding"] = kAnnotationMaskRleEncoding;
            record["mask_rle"] = resolved.mask_rle;
        }
        const json shape_payload = serialize_annotation_shape_payload(frame, object);
        if (!shape_payload.empty()) {
            record["shape"] = shape_payload;
        }
        scene_stream << record.dump() << '\n';

        if (resolved.crop_width > 0U && resolved.crop_height > 0U && !resolved.crop_rgba.empty()) {
            const std::filesystem::path class_dir = entity_dir / category.name;
            std::filesystem::create_directories(class_dir);
            const std::array<char, 64> suffix = [] (const std::uint32_t scene_id, const std::size_t object_id) {
                std::array<char, 64> buffer{};
                std::snprintf(buffer.data(), buffer.size(), "%06u_%03zu.png", scene_id, object_id + 1U);
                return buffer;
            }(scene_index, index);
            const std::filesystem::path entity_path = class_dir / (config.split + "_" + std::string(suffix.data()));
            write_annotation_png(entity_path,
                                 static_cast<int>(resolved.crop_width),
                                 static_cast<int>(resolved.crop_height),
                                 4,
                                 resolved.crop_rgba.data(),
                                 static_cast<int>(resolved.crop_width * 4U));
            result.entity_paths.push_back(entity_path);

            append_jsonl(manifest_dir / "entities.jsonl",
                         json{
                             {"split", config.split},
                             {"scene_index", scene_index},
                             {"source_name", frame.source_name},
                             {"source_path", frame.source_path.string()},
                             {"frame_id", frame.frame_id},
                             {"class", category.name},
                             {"shape_type", annotation_shape_type_name(resolved.shape_type)},
                             {"bbox_xyxy", {resolved.bbox.x1, resolved.bbox.y1, resolved.bbox.x2, resolved.bbox.y2}},
                             {"entity_png", std::filesystem::relative(entity_path, config.output_root).string()},
                             {"scene_png", std::filesystem::relative(scene_image_path, config.output_root).string()},
                         });
        }
    }
    scene_stream.close();

    append_jsonl(manifest_dir / "scenes.jsonl",
                 json{
                     {"split", config.split},
                     {"scene_index", scene_index},
                     {"source_name", frame.source_name},
                     {"source_path", frame.source_path.string()},
                     {"frame_id", frame.frame_id},
                     {"scene_png", std::filesystem::relative(scene_image_path, config.output_root).string()},
                     {"scene_jsonl", std::filesystem::relative(scene_jsonl_path, config.output_root).string()},
                     {"object_count", resolved_objects.size()},
                 });

    write_annotation_categories(config.output_root, categories);
    return result;
}

} // namespace mmltk::gui
