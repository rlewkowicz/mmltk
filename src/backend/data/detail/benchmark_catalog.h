#pragma once  // backend.data private implementation boundary
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include "src/backend/data/benchmark_dataset_compiler.h"
namespace mmltk::backend::data::benchmark_internal {
inline constexpr std::string_view kBenchmarkCatalogRevision = "benchmark-sources-v1";
inline constexpr std::string_view kBenchmarkMappingRevision = "coco80-faithful-annotations-v3";
struct CatalogArtifact {
    std::string artifact_id;
    std::string url;
    std::string filename;
    std::uint64_t expected_size = 0U;
    std::string expected_sha256;
    BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
};
struct NumericCategoryMapping {
    std::uint32_t source_id = 0U;
    std::uint8_t target_id = 0U;
    std::string_view expected_name;
};
struct CategoryLookup {
    std::vector<std::int16_t> target_by_id;
    std::unordered_map<std::uint32_t, std::string_view> expected_names;
};
[[nodiscard]] CategoryLookup make_numeric_lookup(std::span<const NumericCategoryMapping> mappings);
class NumericCategoryAdmission final {
   public:
    explicit NumericCategoryAdmission(const CategoryLookup& lookup) : lookup_(lookup) {}
    void observe(std::optional<std::uint32_t> id, std::optional<std::string_view> name);
    void complete() const;

   private:
    const CategoryLookup& lookup_;
    std::unordered_set<std::uint32_t> matched_;
};
struct StringCategoryMapping {
    std::string_view source_id;
    std::uint8_t target_id = 0U;
    std::string_view expected_name;
};
[[nodiscard]] const std::array<std::string_view, 80>& coco80_class_names() noexcept;
[[nodiscard]] std::span<const NumericCategoryMapping> coco_category_mappings() noexcept;
[[nodiscard]] std::span<const NumericCategoryMapping> objects365_category_mappings() noexcept;
[[nodiscard]] std::span<const StringCategoryMapping> open_images_category_mappings() noexcept;
[[nodiscard]] const CatalogArtifact& coco_annotations_artifact() noexcept;
[[nodiscard]] const CatalogArtifact& coco_train_images_artifact() noexcept;
[[nodiscard]] const CatalogArtifact& coco_val_images_artifact() noexcept;
[[nodiscard]] const CatalogArtifact& objects365_annotations_artifact() noexcept;
[[nodiscard]] std::vector<CatalogArtifact> objects365_train_image_artifacts();
[[nodiscard]] const CatalogArtifact& open_images_boxes_artifact() noexcept;
[[nodiscard]] const CatalogArtifact& open_images_classes_artifact() noexcept;
[[nodiscard]] std::string_view open_images_train_image_url_template() noexcept;
[[nodiscard]] std::string open_images_train_image_url(std::uint64_t image_id);
[[nodiscard]] std::string_view benchmark_source_name(BenchmarkDatasetSource source) noexcept;
[[nodiscard]] std::string_view benchmark_source_version(BenchmarkDatasetSource source) noexcept;
}  // namespace mmltk::backend::data::benchmark_internal
