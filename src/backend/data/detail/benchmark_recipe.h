#pragma once
#include "benchmark_annotations.h"
#include "benchmark_sampling.h"
#include "benchmark_progress.h"
#include "benchmark_catalog.h"
#include "benchmark_download.h"
#include "benchmark_images.h"
#include "benchmark_writer.h"
#include <span>
#include "coconut_annotations.h"
#include "coconut_catalog.h"
#include "coconut_inventory.h"
#include "src/backend/data/benchmark_dataset_compiler.h"
#include <optional>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <exception>
#include <fstream>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
namespace mmltk::backend::data::benchmark_internal {
struct CustomRecipeCatalog {
 CatalogArtifact coco_annotations, coco_train_images, coco_val_images, objects_annotations, open_images_boxes, open_images_classes;
 std::vector<CatalogArtifact> objects_images;
 std::uint32_t coco_train_images_count = 118287U;
 std::uint32_t coco_validation_images_count = 5000U;
};
[[nodiscard]] CustomRecipeCatalog custom_recipe_catalog();
struct CustomRecipePreparation {
 std::filesystem::path coco_train_index_path, coco_val_index_path, objects_index_path, open_images_index_path;
 std::optional<NormalizedAnnotationIndex> coco_train, coco_val, objects, open_images;
 bool coco_indexes_cache_hit, objects_index_cache_hit, open_images_index_cache_hit;
 CombinedSupplementalSamplingResult combined_sampling;
 SupplementalSamplingResult objects_sampling, open_images_sampling;
 std::vector<CatalogArtifact> sampling_object_artifacts;
};
[[nodiscard]] CustomRecipePreparation prepare_custom_recipe(const BenchmarkCompilerConfig&, const BenchmarkCacheLayout&, const CustomRecipeCatalog&, ProgressReporter&, std::size_t,
 mmltk::common::concurrency::CancellationObservation, const BenchmarkTraceSink&, std::span<const int> worker_cpus = {});
}  // namespace mmltk::backend::data::benchmark_internal
namespace mmltk::backend::data::benchmark_internal {
struct RecipeImageArchive {
 CoconutImageNamespace source;
 std::uint16_t shard = 0;
 std::string cache_shard;
 CatalogArtifact artifact;
};
struct AdmittedRecipeArchive {
 RecipeImageArchive origin;
 DownloadResult download;
 unsigned structural_attempts = 0;
};
enum class CoconutReleaseBoundary { MetadataConsumed, MasksStarted };
// Private ordinary catalog facts are also used by bounded local release fixtures.
struct CoconutRecipeCatalog {
 std::vector<CoconutReleaseComponent> releases;
 std::vector<RecipeImageArchive> images;
 CatalogArtifact stock_annotations;
 std::uint64_t coco_validation_images = 5000U;
 // Effect-only production-boundary notification, called outside owner/reporter
 // locks. It neither selects work nor supplies readiness or cache decisions.
 std::function<void(CoconutEdition, CoconutReleaseBoundary)> release_observer;
};
struct CoconutRecipeInputs;
[[nodiscard]] std::shared_ptr<CoconutRecipeInputs> acquire_coconut_recipe_inputs(const BenchmarkCacheLayout&, const CoconutRecipeCatalog&, ProgressReporter&, std::size_t,
 mmltk::common::concurrency::CancellationObservation, const BenchmarkTraceSink&, std::size_t download_connections = 0, std::span<const int> cpus = {}, std::function<void(std::exception_ptr)> failed = {});
struct CoconutRecipePreparation {
 std::shared_ptr<CoconutRecipeInputs> inputs;
 std::vector<CoconutComponent> components;
 std::optional<NormalizedAnnotationIndex> stock_validation;
 bool annotation_cache_hit = true;
 std::uint64_t annotation_storage_bytes = 0;
 std::uint64_t duplicate_xl_images = 0;
 std::uint64_t validation_images = 0;
 nlohmann::json manifest;
};
class CoconutFailureReport {
public:
 CoconutFailureReport(const std::filesystem::path& cache_root, ProgressReporter& progress);
 void reject(const CoconutPhysicalImage&, std::uint64_t release_image_id, std::string_view release, std::uint64_t object_id, std::uint64_t category_id, std::string_view reason) noexcept;

private:
 std::filesystem::path path_;
 ProgressReporter& progress_;
 std::ofstream stream_;
 std::mutex mutex_;
 bool attempted_ = false;
 bool warned_ = false;
};
[[nodiscard]] std::span<const CoconutImageNamespace> coconut_release_sources(CoconutEdition);
[[nodiscard]] CoconutRecipeCatalog coconut_recipe_catalog(CoconutValidation);
[[nodiscard]] CoconutRecipePreparation prepare_coconut_recipe(const BenchmarkCompilerConfig&, const BenchmarkCacheLayout&, const CoconutRecipeCatalog&, std::span<const AdmittedRecipeArchive>,
 const CoconutPhysicalMembership&, ProgressReporter&, CoconutFailureReport&, std::size_t, mmltk::common::concurrency::CancellationObservation, const BenchmarkTraceSink&,
 std::span<const CoconutImageNamespace> refreshed_sources = {}, bool metadata_only = false, std::shared_ptr<CoconutRecipeInputs> inputs = {});
[[nodiscard]] bool coconut_validation_component(CoconutEdition) noexcept;
[[nodiscard]] AnnotationSource coconut_annotation_source(CoconutImageNamespace);
// Same compiler entry with explicit private source catalog, not a second execution path.
// Optional effect-only notification outside cache/reporter locks. Membership
// and scheduling remain owned by the production compiler.
void compile_benchmark_recipe(BenchmarkCompilerConfig, const CoconutRecipeCatalog*, const CustomRecipeCatalog* = nullptr,
 const std::function<void(BenchmarkDatasetSource, std::string_view)>& source_labels_started = {}, const BenchmarkImageReadObserver& image_opened = {});
}  // namespace mmltk::backend::data::benchmark_internal
