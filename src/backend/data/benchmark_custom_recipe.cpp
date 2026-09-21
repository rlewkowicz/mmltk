#include "detail/benchmark_recipe.h"
#include "detail/benchmark_annotation_cache.h"
#include "detail/benchmark_storage.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/common/io/file_digest.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
namespace mmltk::backend::data::benchmark_internal {
namespace common_math = mmltk::common::math;
namespace {
[[nodiscard]] std::string combined_artifact_digest(const std::string& left, const std::string& right) {
    const mmltk::common::io::Sha256Digest left_digest = mmltk::common::io::parse_sha256_hex(left);
    const mmltk::common::io::Sha256Digest right_digest = mmltk::common::io::parse_sha256_hex(right);
    std::array<std::uint8_t, 64> identity{};
    std::ranges::copy(left_digest, identity.begin());
    std::ranges::copy(right_digest, identity.begin() + left_digest.size());
    return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(identity));
}
}  // namespace
CustomRecipeCatalog custom_recipe_catalog() {
    return {coco_annotations_artifact(),  coco_train_images_artifact(),   coco_val_images_artifact(),        objects365_annotations_artifact(),
            open_images_boxes_artifact(), open_images_classes_artifact(), objects365_train_image_artifacts()};
}
CustomRecipePreparation prepare_custom_recipe(const BenchmarkCompilerConfig& config, const BenchmarkCacheLayout& cache, const CustomRecipeCatalog& catalog,
                                              ProgressReporter& progress, std::size_t effective_num_workers,
                                              mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace) {
    const std::filesystem::path objects_index_path = cache.source_indexes("objects365") / "train.normalized.bin";
    const std::filesystem::path open_images_index_path = cache.source_indexes("open-images") / "train.normalized.bin";
    progress.activity("Waiting for annotation cache locks");
    CocoAnnotationCache coco_cache(cache, catalog.coco_annotations, true, catalog.coco_train_images_count, catalog.coco_validation_images_count,
                                   config.num_workers, cancel_requested, trace);
    ArtifactLease objects_annotation_lifecycle = ArtifactLease::acquire(cache.locks / "objects365-annotations.lifecycle.lock", cancel_requested);
    ArtifactLease open_images_annotation_lifecycle = ArtifactLease::acquire(cache.locks / "open-images-annotations.lifecycle.lock", cancel_requested);
    constexpr std::uint64_t kIndexCount = 6U;
    progress.phase(DatasetCompilePhase::Indexing, 0U, kIndexCount);
    coco_cache.discover(progress);
    progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Validating cached Objects365 annotation index");
    std::optional<NormalizedAnnotationIndex> objects =
        discover_cached_index(objects_index_path, BenchmarkDatasetSource::kObjects365V2, "train", cancel_requested, trace);
    progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Validating cached Open Images annotation index");
    std::optional<NormalizedAnnotationIndex> open_images =
        discover_cached_index(open_images_index_path, BenchmarkDatasetSource::kOpenImagesV7, "train", cancel_requested, trace);
    const bool objects_index_cache_hit = objects.has_value();
    const bool open_images_index_cache_hit = open_images.has_value();
    std::uint64_t completed_indexes =
        coco_cache.completed_indexes() + static_cast<std::uint64_t>(objects.has_value()) + static_cast<std::uint64_t>(open_images.has_value());
    progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
    std::vector<DownloadRequest> annotation_requests;
    std::optional<DownloadRequest> objects_annotation_request;
    std::optional<DownloadRequest> open_images_boxes_request;
    std::optional<DownloadRequest> open_images_classes_request;
    const auto annotation_parse_options = [&](const BenchmarkDatasetSource source, std::string split, const std::uint32_t expected_image_count,
                                              const bool keep_images_without_mapped_boxes) {
        return AnnotationParseOptions{
            source, std::move(split), expected_image_count, config.num_workers, keep_images_without_mapped_boxes, cancel_requested, trace,
        };
    };
    if (coco_cache.pending_download()) annotation_requests.push_back(*coco_cache.pending_download());
    if (!objects) {
        objects_annotation_request = make_download_request(cache, "objects365", catalog.objects_annotations);
        annotation_requests.push_back(*objects_annotation_request);
    }
    if (!open_images) {
        open_images_boxes_request = make_download_request(cache, "open-images", catalog.open_images_boxes);
        open_images_classes_request = make_download_request(cache, "open-images", catalog.open_images_classes);
        annotation_requests.push_back(*open_images_boxes_request);
        annotation_requests.push_back(*open_images_classes_request);
    }
    std::unordered_map<std::string, DownloadResult> annotation_downloads;
    if (!annotation_requests.empty()) {
        ArtifactProgressTotals transfer_progress;
        std::uint64_t annotation_storage = 0U;
        bool needs_archive_scratch = false;
        for (const DownloadRequest& request : annotation_requests) {
            annotation_storage = common_math::checked_add(annotation_storage, request.expected_size, "benchmark annotation storage estimate overflow");
            needs_archive_scratch = needs_archive_scratch || request.artifact_id == catalog.coco_annotations.artifact_id ||
                                    request.artifact_id == catalog.objects_annotations.artifact_id;
        }
        if (needs_archive_scratch) {
            annotation_storage = common_math::checked_add(annotation_storage, kArchiveScratchBytes, "benchmark annotation storage estimate overflow");
        }
        require_storage(cache.root, annotation_storage, "benchmark annotation acquisition and indexing", trace);
        progress.phase(DatasetCompilePhase::Downloading);
        if (coco_cache.pending_download()) { progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Downloading COCO annotation metadata"); }
        if (!objects) { progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Downloading Objects365 annotation metadata"); }
        if (!open_images) { progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Downloading Open Images annotation metadata"); }
        auto downloads = download_artifacts(annotation_requests, std::min<std::size_t>(3U, effective_num_workers), cancel_requested,
                                            progress.transfer_observer_enabled()
                                                ? DownloadProgressSink{[&](const DownloadProgress& update) { transfer_progress.update(update, progress); }}
                                                : DownloadProgressSink{},
                                            trace);
        for (std::size_t i = 0; i < downloads.size(); ++i) annotation_downloads.emplace(annotation_requests[i].artifact_id, std::move(downloads[i]));
        progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
    }
    ArtifactProgressTotals annotation_repair_progress;
    const auto repair_annotations = [&](const BenchmarkDatasetSource source, const std::vector<DownloadRequest>& requests, const std::string_view reason) {
        auto repaired =
            repair_annotation_artifacts(requests, source, reason, progress, annotation_repair_progress, effective_num_workers, cancel_requested, trace);
        for (std::size_t i = 0; i < requests.size(); ++i) annotation_downloads.insert_or_assign(requests[i].artifact_id, std::move(repaired[i]));
    };
    if (coco_cache.pending_download()) {
        coco_cache.settle(std::move(annotation_downloads.at(catalog.coco_annotations.artifact_id)), progress, effective_num_workers, completed_indexes,
                          kIndexCount);
    }
    auto coco_indexes = coco_cache.take_indexes();
    auto coco_train_index_path = std::move(coco_indexes.train_path);
    auto coco_val_index_path = std::move(coco_indexes.validation_path);
    auto coco_train = std::move(coco_indexes.train);
    auto coco_val = std::move(coco_indexes.validation);
    const bool coco_indexes_cache_hit = coco_indexes.cache_hit;
    if (!objects) {
        retry_annotation_indexing(
            cancel_requested,
            [&] {
                const DownloadResult& annotation_archive = annotation_downloads.at(catalog.objects_annotations.artifact_id);
                const std::filesystem::path extracted_dir = cache.source_indexes("objects365") / "source-json";
                std::filesystem::create_directories(extracted_dir);
                const std::filesystem::path json_path = extracted_dir / "zhiyuan_objv2_train.json";
                progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Extracting Objects365 train annotations");
                const std::string annotation_digest =
                    extract_archive_member(annotation_archive.path, "zhiyuan_objv2_train.json", json_path, annotation_archive.identity,
                                           cache.locks / "objects365-train-json.extract.lock", cancel_requested, trace);
                objects = load_or_build_index(
                    cache, objects_index_path, BenchmarkDatasetSource::kObjects365V2, "train", annotation_digest, cancel_requested, trace, [&] {
                        progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Parsing and indexing Objects365 annotations");
                        return parse_coco_style_annotations(json_path, annotation_digest, objects365_category_mappings(),
                                                            annotation_parse_options(BenchmarkDatasetSource::kObjects365V2, "train", 0U, false));
                    });
                ++completed_indexes;
                progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
            },
            [&](const std::exception& error) {
                const std::filesystem::path json_path = cache.source_indexes("objects365") / "source-json" / "zhiyuan_objv2_train.json";
                remove_cache_path(json_path.string() + ".extract.json");
                remove_cache_path(json_path);
                remove_cache_path(objects_index_path.string() + ".complete.json");
                remove_cache_path(objects_index_path);
                repair_annotations(BenchmarkDatasetSource::kObjects365V2, {*objects_annotation_request}, error.what());
            });
    }
    if (!open_images) {
        retry_annotation_indexing(
            cancel_requested,
            [&] {
                const DownloadResult& boxes = annotation_downloads.at(catalog.open_images_boxes.artifact_id);
                const DownloadResult& classes = annotation_downloads.at(catalog.open_images_classes.artifact_id);
                const std::string annotation_identity = combined_artifact_digest(boxes.identity, classes.identity);
                open_images = load_or_build_index(
                    cache, open_images_index_path, BenchmarkDatasetSource::kOpenImagesV7, "train", annotation_identity, cancel_requested, trace, [&] {
                        progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Parsing and indexing Open Images annotations");
                        return parse_open_images_annotations(boxes.path, classes.path, annotation_identity, open_images_category_mappings(),
                                                             annotation_parse_options(BenchmarkDatasetSource::kOpenImagesV7, "train", 0U, false));
                    });
                ++completed_indexes;
                progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
            },
            [&](const std::exception& error) {
                remove_cache_path(open_images_index_path.string() + ".complete.json");
                remove_cache_path(open_images_index_path);
                repair_annotations(BenchmarkDatasetSource::kOpenImagesV7, {*open_images_boxes_request, *open_images_classes_request}, error.what());
            });
    }
    if (!coco_train || !coco_val || !objects || !open_images) { throw std::runtime_error("benchmark normalized annotation indexing did not complete"); }
    if (coco_val->images.size() != catalog.coco_validation_images_count) {
        throw std::runtime_error("COCO val2017 normalized index must contain exactly 5,000 images");
    }
    {
        progress.activity("Checking COCO train and validation metadata reuse");
        std::unordered_set<std::uint64_t> validation_ids;
        validation_ids.reserve(coco_val->images.size());
        for (const NormalizedImage& image : coco_val->images) { validation_ids.emplace(image.source_image_id); }
        for (const NormalizedImage& image : coco_train->images) {
            if (validation_ids.contains(image.source_image_id)) { throw std::runtime_error("COCO train and validation metadata reuse an image ID"); }
        }
        trace_benchmark_event(trace, "benchmark.split_reuse.metadata_check", [&] {
            return nlohmann::json{{"coco_train_images", coco_train->images.size()}, {"coco_val_images", coco_val->images.size()}, {"overlap", 0}};
        });
    }
    progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Selecting byte-efficient Objects365 shards and balanced images");
    const std::vector<CatalogArtifact> sampling_object_artifacts = catalog.objects_images;
    std::vector<std::uint64_t> object_shard_bytes;
    object_shard_bytes.reserve(sampling_object_artifacts.size());
    for (const CatalogArtifact& artifact : sampling_object_artifacts) { object_shard_bytes.push_back(artifact.expected_size); }
    CombinedSupplementalSamplingResult combined_sampling =
        sample_combined_supplemental_indices(*coco_train, *objects, *open_images, object_shard_bytes, cancel_requested);
    SupplementalSamplingResult objects_sampling = std::move(combined_sampling.objects365);
    SupplementalSamplingResult open_images_sampling = std::move(combined_sampling.open_images);
    *objects = std::move(objects_sampling.index);
    progress.phase(DatasetCompilePhase::Indexing, ++completed_indexes, kIndexCount);
    progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Selecting Open Images class-deficit and diversity sample");
    *open_images = std::move(open_images_sampling.index);
    progress.phase(DatasetCompilePhase::Indexing, ++completed_indexes, kIndexCount);
    const auto trace_sampling = [&](const SupplementalSamplingStats& stats, const BenchmarkDatasetSource source) {
        trace_benchmark_event(trace, "benchmark.sampling.complete", [&] {
            return nlohmann::json{
                {"source", benchmark_source_name(source)},
                {"revision", kSupplementalSamplingRevision},
                {"full_images", stats.full_images},
                {"full_boxes", stats.full_boxes},
                {"selected_images", stats.selected_images},
                {"selected_boxes", stats.selected_boxes},
                {"available_class_images", stats.available_class_images},
                {"selected_class_images", stats.selected_class_images},
            };
        });
    };
    trace_sampling(objects_sampling.stats, BenchmarkDatasetSource::kObjects365V2);
    trace_sampling(open_images_sampling.stats, BenchmarkDatasetSource::kOpenImagesV7);
    trace_benchmark_event(trace, "benchmark.sampling.source_mix", [&] {
        return nlohmann::json{{"target_images", combined_sampling.target_images},
                              {"objects365_images", objects_sampling.stats.selected_images},
                              {"open_images_images", open_images_sampling.stats.selected_images},
                              {"open_images_floor", combined_sampling.open_images_floor},
                              {"open_images_ceiling", combined_sampling.open_images_ceiling},
                              {"objects365_shards", combined_sampling.objects365_shards},
                              {"objects365_archive_bytes", combined_sampling.objects365_archive_bytes}};
    });
    progress.activity("Finalizing normalized annotation cache");
    open_images_annotation_lifecycle = ArtifactLease{};
    objects_annotation_lifecycle = ArtifactLease{};
    return CustomRecipePreparation{std::move(coco_train_index_path),
                                   std::move(coco_val_index_path),
                                   std::move(objects_index_path),
                                   std::move(open_images_index_path),
                                   std::move(coco_train),
                                   std::move(coco_val),
                                   std::move(objects),
                                   std::move(open_images),
                                   std::move(coco_indexes_cache_hit),
                                   std::move(objects_index_cache_hit),
                                   std::move(open_images_index_cache_hit),
                                   std::move(combined_sampling),
                                   std::move(objects_sampling),
                                   std::move(open_images_sampling),
                                   std::move(sampling_object_artifacts)};
}
}  // namespace mmltk::backend::data::benchmark_internal
