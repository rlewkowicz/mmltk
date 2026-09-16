module;
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>
export module mmltk.backend.imaging.annotation.semantic_scene;
export import mmltk.backend.imaging.annotation.manual_mask_mapping;
export namespace mmltk::backend::imaging::annotation {
struct ContentIdentity final {
    std::uint64_t session_nonce = 0U;
    std::uint64_t sequence = 0U;
    [[nodiscard]] bool valid() const noexcept { return session_nonce != 0U || sequence != 0U; }
    [[nodiscard]] bool operator==(const ContentIdentity&) const noexcept = default;
};
struct Hsv {
    float hue_degrees = 180.0F;
    float saturation = 0.5F;
    float value = 0.5F;
    bool operator==(const Hsv&) const = default;
};
struct ColorTolerance {
    float hue_minus_pct = 0.0F;
    float hue_plus_pct = 0.0F;
    float saturation_minus_pct = 0.0F;
    float saturation_plus_pct = 0.0F;
    float value_minus_pct = 0.0F;
    float value_plus_pct = 0.0F;
    bool operator==(const ColorTolerance&) const = default;
};
struct ColorRange {
    Hsv center{};
    ColorTolerance tolerance{};
    bool sampling = false;
    bool operator==(const ColorRange&) const = default;
};
struct Box {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    bool operator==(const Box&) const = default;
};
struct MaskRegion {
    std::uint32_t capture_x = 0U;
    std::uint32_t capture_y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    bool operator==(const MaskRegion&) const = default;
};
enum class ShapeType : std::uint8_t { Box = 0, Mask = 1, Spline = 2, Point = 3, Skeleton = 4 };
enum class Tool : std::uint8_t {
    Select = 0,
    Box = 1,
    MaskPaint = 2,
    MaskErase = 3,
    MaskFill = 4,
    Spline = 5,
    Point = 6,
    Skeleton = 7,
    ColorSample = 8,
    Count = 9,
};
struct Point {
    float x = 0.0F;
    float y = 0.0F;
    bool operator==(const Point&) const = default;
};
enum class SplineHandleMode : std::uint8_t { Corner = 0, Smooth = 1, Mirrored = 2 };
struct SplineHandle {
    Point position{};
    bool enabled = false;
    bool operator==(const SplineHandle&) const = default;
};
struct SplineKnot {
    Point position{};
    SplineHandle in_handle{};
    SplineHandle out_handle{};
    SplineHandleMode handle_mode = SplineHandleMode::Corner;
    bool operator==(const SplineKnot&) const = default;
};
struct BoxShape {
    Box box{};
    bool operator==(const BoxShape&) const = default;
};
struct MaskRun {
    std::uint32_t offset = 0U;
    std::uint32_t length = 0U;
    bool operator==(const MaskRun&) const = default;
};
struct DeferredMask : ManualMaskMapping {
    std::vector<MaskRun> runs;
    bool operator==(const DeferredMask&) const = default;
};
struct MaskShape {
    Box box{};
    MaskRegion region{};
    std::vector<std::uint8_t> mask;
    std::uint64_t seed_frame_id = 0U;
    std::optional<ContentIdentity> seed_live_frame_id;
    std::shared_ptr<const DeferredMask> deferred;
    std::vector<MaskRun> runs;
    bool operator==(const MaskShape& other) const {
        const bool deferred_equal = deferred == nullptr || other.deferred == nullptr ? deferred == other.deferred : *deferred == *other.deferred;
        return box == other.box && region == other.region && mask == other.mask && seed_frame_id == other.seed_frame_id &&
               seed_live_frame_id == other.seed_live_frame_id && deferred_equal && runs == other.runs;
    }
};
struct SplineShape {
    bool closed = false;
    std::vector<SplineKnot> knots;
    bool operator==(const SplineShape&) const = default;
};
struct PointShape {
    Point point{};
    bool operator==(const PointShape&) const = default;
};
struct SkeletonNode {
    std::string key;
    Point point{};
    bool visible = true;
    bool operator==(const SkeletonNode&) const = default;
};
struct SkeletonEdge {
    std::size_t source_index = 0U;
    std::size_t target_index = 0U;
    bool operator==(const SkeletonEdge&) const = default;
};
struct SkeletonShape {
    std::vector<SkeletonNode> nodes;
    std::vector<SkeletonEdge> edges;
    bool operator==(const SkeletonShape&) const = default;
};
using Shape = std::variant<BoxShape, MaskShape, SplineShape, PointShape, SkeletonShape>;
struct Object {
    std::string object_id;
    bool enabled = true;
    std::size_t category_index = 0U;
    ColorRange sup{};
    ColorRange nosup{};
    Shape shape = BoxShape{};
    bool operator==(const Object&) const = default;
};
struct Scene {
    std::uint64_t document_generation = 0U;
    std::uint64_t interaction_revision = 0U;
    std::vector<std::string> category_names;
    std::vector<Object> objects;
    bool operator==(const Scene&) const = default;
};
}  // namespace mmltk::backend::imaging::annotation
