#pragma once
#include "src/frameworks/reflection/reflected_declarations.h"
#include "src/backend/data/catalog/keypoint_schema.h"
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "mmltk/frameworks/reflection/materializer.h"
namespace mmltk::backend::data::catalog {
inline constexpr std::size_t kClassCatalogCapacity = 256U;
inline constexpr std::size_t kClassNameCapacity = 256U;
using ForegroundIndex = std::uint8_t;
[[nodiscard]] ForegroundIndex checked_foreground_index(std::size_t index);
struct ClassName final {
 MMLTK_MAX_BYTES(kClassNameCapacity) std::string value;
 [[nodiscard]] bool valid() const noexcept { return !value.empty() && value.size() <= kClassNameCapacity && value.find('\0') == std::string::npos; }
 auto operator<=>(const ClassName&) const = default;
};
MMLTK_REFLECT_FIELDS(ClassName)
struct OrderedClassCatalog final {
 MMLTK_MAX_ITEMS(kClassCatalogCapacity) std::vector<ClassName> names;
 // Absent means no joint schemas, including persisted catalogs predating keypoints.
 // A present schema has exactly one entry per foreground class, including empty classes.
 MMLTK_MAX_ITEMS(kClassCatalogCapacity) std::optional<std::vector<ClassKeypointSchema>> keypoints;
 auto operator<=>(const OrderedClassCatalog&) const = default;
};
MMLTK_REFLECT_FIELDS(OrderedClassCatalog)
enum class ClassReferenceDomain : std::uint8_t { Foreground, RawOutputSlot };
MMLTK_REFLECT_ENUM(ClassReferenceDomain)
// Zero is a valid foreground index. Source IDs and physical output slots are
// admitted by their respective adapters, never inferred from this inventory.
class ClassCatalog final {
public:
 explicit ClassCatalog(std::vector<std::string> names = {}, std::size_t name_capacity = kClassNameCapacity);
 explicit ClassCatalog(const OrderedClassCatalog& record);
 ClassCatalog(const ClassCatalog&) = delete;
 ClassCatalog& operator=(const ClassCatalog&) = delete;
 ClassCatalog(ClassCatalog&&) noexcept = default;
 ClassCatalog& operator=(ClassCatalog&&) noexcept = default;
 [[nodiscard]] std::span<const std::string> names() const noexcept { return names_; }
 [[nodiscard]] std::size_t size() const noexcept { return names_.size(); }
 [[nodiscard]] bool empty() const noexcept { return names_.empty(); }
 [[nodiscard]] std::optional<std::uint32_t> resolve(std::string_view name) const;
 [[nodiscard]] bool ordered_equal(const ClassCatalog& other) const noexcept;
 [[nodiscard]] const ClassKeypointSchema& keypoints(std::size_t foreground_index) const;
 [[nodiscard]] bool has_keypoints() const noexcept;
 // Maps each index in this catalog to the identical name in destination.
 // A different set or joint meaning is an admission error, including a missing class.
 [[nodiscard]] std::vector<std::uint32_t> permutation_to(const ClassCatalog& destination) const;
 [[nodiscard]] OrderedClassCatalog record() const;

private:
 struct NameHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
 };
 std::vector<std::string> names_;
 std::vector<ClassKeypointSchema> keypoints_;
 std::unordered_map<std::string_view, std::uint32_t, NameHash, std::equal_to<>> lookup_;
};
}  // namespace mmltk::backend::data::catalog
