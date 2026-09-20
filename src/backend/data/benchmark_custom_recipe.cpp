#include "detail/benchmark_recipe.h"
#include "detail/benchmark_compiler.h"
#include "detail/benchmark_storage.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/common/io/file_digest.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
namespace mmltk::backend::data::benchmark_internal {
namespace common_math = mmltk::common::math;
namespace {
constexpr std::uint64_t kArchiveScratchBytes = 24ULL * 1024U * 1024U * 1024U;
[[nodiscard]] std::string combined_artifact_digest(const std::string& left, const std::string& right) {
    const mmltk::common::io::Sha256Digest left_digest = mmltk::common::io::parse_sha256_hex(left);
    const mmltk::common::io::Sha256Digest right_digest = mmltk::common::io::parse_sha256_hex(right);
    std::array<std::uint8_t, 64> identity{};
    std::ranges::copy(left_digest, identity.begin());
    std::ranges::copy(right_digest, identity.begin() + left_digest.size());
    return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(identity));
}
[[nodiscard]] std::optional<NormalizedAnnotationIndex> discover_cached_index(const std::filesystem::path& path, const BenchmarkDatasetSource source,
                                                                             const std::string_view split,
                                                                             mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                                             const BenchmarkTraceSink& trace) {
    try {
        const nlohmann::json manifest = read_json_file(path.string() + ".complete.json");
        const std::string digest = manifest.at("annotation_sha256").get<std::string>();
        return load_normalized_annotation_index(path, source, split, digest, cancel_requested, trace);
    } catch (const std::exception&) {
        throw_if_benchmark_cancelled(cancel_requested);
        return std::nullopt;
    }
}
[[nodiscard]] NormalizedAnnotationIndex load_or_build_index(const BenchmarkCacheLayout& cache, const std::filesystem::path& path,
                                                            const BenchmarkDatasetSource source, const std::string_view split,
                                                            const std::string_view annotation_sha256,
                                                            mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                            const BenchmarkTraceSink& trace, const std::function<NormalizedAnnotationIndex()>& builder) {
    ArtifactLease lease =
        ArtifactLease::acquire(cache.locks / (std::string(benchmark_source_name(source)) + "-" + std::string(split) + ".index.lock"), cancel_requested);
    if (auto cached = load_normalized_annotation_index(path, source, split, annotation_sha256, cancel_requested, trace)) { return std::move(*cached); }
    NormalizedAnnotationIndex index = builder();
    store_normalized_annotation_index(path, index, cancel_requested, trace);
    return index;
}
// Runs an annotation indexing step with up to three attempts, invoking repair (cache invalidation
// plus artifact re-download) between attempts. Cancellation always rethrows immediately.
void retry_annotation_indexing(mmltk::common::concurrency::CancellationObservation cancel_requested, const std::function<void()>& body,
                               const std::function<void(const std::exception&)>& repair) {
    for (std::uint32_t attempt = 1U; attempt <= 3U; ++attempt) {
        try {
            body();
            break;
        } catch (const std::exception& error) {
            throw_if_benchmark_cancelled(cancel_requested);
            if (attempt == 3U) { throw; }
            repair(error);
        }
    }
}
}
CustomRecipeCatalog custom_recipe_catalog() {
    return {coco_annotations_artifact(), coco_train_images_artifact(), coco_val_images_artifact(), objects365_annotations_artifact(),
        open_images_boxes_artifact(), open_images_classes_artifact(), objects365_train_image_artifacts()};
}
CustomRecipePreparation prepare_custom_recipe(const BenchmarkCompilerConfig& config, const BenchmarkCacheLayout& cache, const CustomRecipeCatalog& catalog,
    ProgressReporter& progress, std::size_t effective_num_workers,
    mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace) {
    const std::filesystem::path coco_train_index_path = cache.source_indexes("coco") / "train2017.normalized.bin";
    const std::filesystem::path coco_val_index_path = cache.source_indexes("coco") / "val2017.normalized.bin";
    const std::filesystem::path objects_index_path = cache.source_indexes("objects365") / "train.normalized.bin";
    const std::filesystem::path open_images_index_path = cache.source_indexes("open-images") / "train.normalized.bin";
    progress.activity("Waiting for annotation cache locks");
    ArtifactLease coco_annotation_lifecycle = ArtifactLease::acquire(cache.locks / "coco-annotations.lifecycle.lock", cancel_requested);
    ArtifactLease objects_annotation_lifecycle = ArtifactLease::acquire(cache.locks / "objects365-annotations.lifecycle.lock", cancel_requested);
    ArtifactLease open_images_annotation_lifecycle = ArtifactLease::acquire(cache.locks / "open-images-annotations.lifecycle.lock", cancel_requested);
    constexpr std::uint64_t kIndexCount = 6U;
    progress.phase(DatasetCompilePhase::Indexing, 0U, kIndexCount);
    progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Validating cached COCO train annotation index");
    std::optional<NormalizedAnnotationIndex> coco_train =
        discover_cached_index(coco_train_index_path, BenchmarkDatasetSource::kCoco2017, "train2017", cancel_requested, trace);
    progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Validating cached COCO validation annotation index");
    std::optional<NormalizedAnnotationIndex> coco_val =
        discover_cached_index(coco_val_index_path, BenchmarkDatasetSource::kCoco2017, "val2017", cancel_requested, trace);
    progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Validating cached Objects365 annotation index");
    std::optional<NormalizedAnnotationIndex> objects =
        discover_cached_index(objects_index_path, BenchmarkDatasetSource::kObjects365V2, "train", cancel_requested, trace);
    progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Validating cached Open Images annotation index");
    std::optional<NormalizedAnnotationIndex> open_images =
        discover_cached_index(open_images_index_path, BenchmarkDatasetSource::kOpenImagesV7, "train", cancel_requested, trace);
    const bool coco_indexes_cache_hit = coco_train && coco_val;
    const bool objects_index_cache_hit = objects.has_value();
    const bool open_images_index_cache_hit = open_images.has_value();
    std::uint64_t completed_indexes = static_cast<std::uint64_t>(coco_train.has_value()) + static_cast<std::uint64_t>(coco_val.has_value()) +
                                      static_cast<std::uint64_t>(objects.has_value()) + static_cast<std::uint64_t>(open_images.has_value());
    progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
    std::vector<DownloadRequest> annotation_requests;
    std::optional<DownloadRequest> coco_annotation_request;
    std::optional<DownloadRequest> objects_annotation_request;
    std::optional<DownloadRequest> open_images_boxes_request;
    std::optional<DownloadRequest> open_images_classes_request;
    const auto annotation_parse_options = [&](const BenchmarkDatasetSource source, std::string split, const std::uint32_t expected_image_count,
                                              const bool keep_images_without_mapped_boxes) {
        return AnnotationParseOptions{
            source, std::move(split), expected_image_count, config.num_workers, keep_images_without_mapped_boxes, cancel_requested, trace,
        };
    };
    if (!coco_train || !coco_val) {
        coco_annotation_request = make_download_request(cache, "coco", catalog.coco_annotations);
        annotation_requests.push_back(*coco_annotation_request);
    }
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
    // The initial annotation fetch and the post-validation repair differ only in their concurrency
    // budget and progress sink; downloading and indexing results by artifact id happens here once.
    const auto fetch_annotation_artifacts = [&](const std::vector<DownloadRequest>& requests, const std::size_t workers, ArtifactProgressTotals* totals) {
        const std::vector<DownloadResult> downloads = download_artifacts(
            requests, workers, cancel_requested,
            progress.transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) {
                                                     totals->update(update, progress);
                                                 }}
                                                 : DownloadProgressSink{},
            trace);
        for (std::size_t index = 0U; index < downloads.size(); ++index) {
            annotation_downloads.insert_or_assign(requests[index].artifact_id, downloads[index]);
        }
    };
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
        if (!coco_train || !coco_val) { progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Downloading COCO annotation metadata"); }
        if (!objects) { progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Downloading Objects365 annotation metadata"); }
        if (!open_images) { progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Downloading Open Images annotation metadata"); }
        fetch_annotation_artifacts(annotation_requests, std::min<std::size_t>(3U, effective_num_workers), &transfer_progress);
        progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
    }
    ArtifactProgressTotals annotation_repair_progress;
    const auto repair_annotation_artifacts = [&](const BenchmarkDatasetSource source, std::vector<DownloadRequest> requests,
                                                 const std::string_view reason) {
        for (DownloadRequest& request : requests) {
            if (std::filesystem::is_regular_file(request.destination)) {
                progress.source_activity(source, "Failure-only SHA-256 diagnosis for " + request.artifact_id);
                const std::string failure_sha256 =
                    mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(request.destination, [&] { return cancel_requested.requested(); }));
                trace_benchmark_event(trace, "benchmark.download.failure_sha256",
                                      [&] { return nlohmann::json{{"artifact", request.artifact_id}, {"sha256", failure_sha256}, {"reason", reason}}; });
            }
            invalidate_download_artifact(request, cancel_requested, trace);
            request.redownload = true;
        }
        progress.source_activity(source,
                                 "Redownloading annotation metadata after structural "
                                 "validation failure");
        fetch_annotation_artifacts(
            requests,
            requests.size() == 1U ? std::min<std::size_t>(8U, effective_num_workers) : std::min<std::size_t>({3U, requests.size(), effective_num_workers}),
            &annotation_repair_progress);
    };
    if (!coco_train || !coco_val) {
        retry_annotation_indexing(
            cancel_requested,
            [&] {
                const DownloadResult& archive = annotation_downloads.at(catalog.coco_annotations.artifact_id);
                const std::filesystem::path extracted_dir = cache.source_indexes("coco") / "source-json";
                std::filesystem::create_directories(extracted_dir);
                if (!coco_train) {
                    const std::filesystem::path json_path = extracted_dir / "instances_train2017.json";
                    progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Extracting COCO train annotations");
                    const std::string digest = extract_archive_member(archive.path, "annotations/instances_train2017.json", json_path, archive.identity,
                                                                      cache.locks / "coco-train-json.extract.lock", cancel_requested, trace);
                    coco_train =
                        load_or_build_index(cache, coco_train_index_path, BenchmarkDatasetSource::kCoco2017, "train2017", digest, cancel_requested, trace, [&] {
                            progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Parsing and indexing COCO train annotations");
                            return parse_coco_style_annotations(json_path, digest, coco_category_mappings(),
                                                                annotation_parse_options(BenchmarkDatasetSource::kCoco2017, "train2017", catalog.coco_train_images_count, false));
                        });
                    ++completed_indexes;
                    progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
                }
                if (!coco_val) {
                    const std::filesystem::path json_path = extracted_dir / "instances_val2017.json";
                    progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Extracting COCO validation annotations");
                    const std::string digest = extract_archive_member(archive.path, "annotations/instances_val2017.json", json_path, archive.identity,
                                                                      cache.locks / "coco-val-json.extract.lock", cancel_requested, trace);
                    coco_val =
                        load_or_build_index(cache, coco_val_index_path, BenchmarkDatasetSource::kCoco2017, "val2017", digest, cancel_requested, trace, [&] {
                            progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Parsing and indexing COCO validation annotations");
                            return parse_coco_style_annotations(json_path, digest, coco_category_mappings(),
                                                                annotation_parse_options(BenchmarkDatasetSource::kCoco2017, "val2017", catalog.coco_validation_images_count, true));
                        });
                    ++completed_indexes;
                    progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
                }
            },
            [&](const std::exception& error) {
                const std::filesystem::path extracted_dir = cache.source_indexes("coco") / "source-json";
                if (!coco_train) {
                    remove_cache_path(extracted_dir / "instances_train2017.json.extract.json");
                    remove_cache_path(extracted_dir / "instances_train2017.json");
                    remove_cache_path(coco_train_index_path.string() + ".complete.json");
                    remove_cache_path(coco_train_index_path);
                }
                if (!coco_val) {
                    remove_cache_path(extracted_dir / "instances_val2017.json.extract.json");
                    remove_cache_path(extracted_dir / "instances_val2017.json");
                    remove_cache_path(coco_val_index_path.string() + ".complete.json");
                    remove_cache_path(coco_val_index_path);
                }
                repair_annotation_artifacts(BenchmarkDatasetSource::kCoco2017, {*coco_annotation_request}, error.what());
            });
    }
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
                repair_annotation_artifacts(BenchmarkDatasetSource::kObjects365V2, {*objects_annotation_request}, error.what());
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
                repair_annotation_artifacts(BenchmarkDatasetSource::kOpenImagesV7, {*open_images_boxes_request, *open_images_classes_request}, error.what());
            });
    }
    if (!coco_train || !coco_val || !objects || !open_images) { throw std::runtime_error("benchmark normalized annotation indexing did not complete"); }
    if (coco_val->images.size() != catalog.coco_validation_images_count) { throw std::runtime_error("COCO val2017 normalized index must contain exactly 5,000 images"); }
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
    coco_annotation_lifecycle = ArtifactLease{};
    return CustomRecipePreparation{std::move(coco_train_index_path), std::move(coco_val_index_path), std::move(objects_index_path), std::move(open_images_index_path), std::move(coco_train), std::move(coco_val), std::move(objects), std::move(open_images), std::move(coco_indexes_cache_hit), std::move(objects_index_cache_hit), std::move(open_images_index_cache_hit), std::move(combined_sampling), std::move(objects_sampling), std::move(open_images_sampling), std::move(sampling_object_artifacts)};
}
}
