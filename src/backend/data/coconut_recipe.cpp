#include "detail/benchmark_recipe.h"
#include "detail/benchmark_annotation_cache.h"
#include "detail/benchmark_image_decoder.h"
#include "detail/benchmark_storage.h"
#include "src/common/io/file_digest.h"
#include "src/common/math/checked_arithmetic.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <unordered_set>
#include <map>
#include <tuple>
#include <iterator>
namespace mmltk::backend::data::benchmark_internal {
namespace {
std::string digest_text(std::string_view text) {
    return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
}
std::vector<CoconutImageNamespace> edition_sources(CoconutEdition edition) {
    switch (edition) {
        case CoconutEdition::Base: return {CoconutImageNamespace::CocoTrain, CoconutImageNamespace::CocoUnlabeled};
        case CoconutEdition::RelabeledValidation: return {CoconutImageNamespace::CocoValidation};
        case CoconutEdition::Large:
        case CoconutEdition::XLarge: return {CoconutImageNamespace::Objects365V2};
        case CoconutEdition::ObjectsValidation: return {CoconutImageNamespace::Objects365V1};
    }
    throw std::invalid_argument("invalid COCONut edition");
}
}  // namespace
CoconutFailureReport::CoconutFailureReport(const std::filesystem::path& cache_root, ProgressReporter& progress) : progress_(progress) {
    auto directory = cache_root;
    for (auto parent = cache_root; !parent.empty(); parent = parent.parent_path()) {
        if (parent.filename() == ".cache") {
            directory = parent;
            break;
        }
        if (parent == parent.root_path()) break;
    }
    path_ = directory / "failed.txt";
}
void CoconutFailureReport::reject(const CoconutPhysicalImage& image, const std::uint64_t release_image_id, const std::string_view release,
                                 const std::uint64_t object_id, const std::uint64_t category_id, const std::string_view reason) noexcept {
    try {
        if (!attempted_) {
            attempted_ = true;
            stream_.open(path_, std::ios::app);
        }
        if (stream_) {
            stream_ << nlohmann::json{{"image", image.member},
                                     {"image_id", image.image_id},
                                     {"release_image_id", release_image_id},
                                     {"source", coconut_namespace_name(image.source)},
                                     {"release", release},
                                     {"object_id", object_id},
                                     {"category_id", category_id},
                                     {"reason", reason}}
                           .dump()
                    << '\n';
            stream_.flush();
        }
        if (!warned_) {
            warned_ = true;
            progress_.activity(std::string("Skipping invalid COCONut objects; ") +
                               (stream_ ? "details: " : "cannot write failure report: ") + path_.string());
        }
    } catch (...) {
        // Reporting cannot make rejected object metadata fatal to compilation.
    }
}
std::vector<std::pair<std::uint32_t, std::uint32_t>> coconut_image_dimensions(
    const CoconutComponent& component, const std::string_view release, const std::span<const CachedImageDirectory> directories,
    const std::span<const std::uint16_t> image_sources, CoconutFailureReport& failures, ProgressReporter& progress,
    const mmltk::common::concurrency::CancellationObservation cancellation) {
    const auto& index = component.index;
    if (component.inventory.size() != index.images.size() || image_sources.size() != index.images.size())
        throw std::runtime_error("COCONut image geometry membership is misaligned");
    std::vector<std::pair<std::uint32_t, std::uint32_t>> dimensions;
    dimensions.reserve(index.images.size());
    BenchmarkImageValidator validator;
    for (std::size_t i = 0; i < index.images.size(); ++i) {
        if ((i & 4095U) == 0U) {
            throw_if_benchmark_cancelled(cancellation);
            progress.activity("Checking " + std::string(release) + " image geometry: " + std::to_string(i) + "/" + std::to_string(index.images.size()));
        }
        if (image_sources[i] >= directories.size()) throw std::runtime_error("COCONut image geometry directory is invalid");
        const auto& image = index.images[i];
        auto actual = std::pair{image.width, image.height};
        const auto path = cached_image_path(directories[image_sources[i]].path, image.source_image_id);
        try {
            actual = validator.validate_file(path);
        } catch (const std::bad_alloc&) { throw; } catch (const std::exception&) {
            // Unreadable bytes still go through the writer's bounded physical repair.
        }
        dimensions.push_back(actual);
        if (actual == std::pair{image.width, image.height}) continue;
        const auto& identity = component.inventory[i];
        const std::string reason = "annotation dimensions " + std::to_string(image.width) + "x" + std::to_string(image.height) +
                                   " do not match image dimensions " + std::to_string(actual.first) + "x" + std::to_string(actual.second);
        for (const auto& box : std::span(index.boxes).subspan(static_cast<std::size_t>(image.first_box), image.box_count))
            failures.reject(identity.physical, identity.release_image_id, release, box.annotation_id, box.source_category_id, reason);
    }
    return dimensions;
}
bool coconut_validation_component(CoconutEdition edition) noexcept {
    return edition == CoconutEdition::RelabeledValidation || edition == CoconutEdition::ObjectsValidation;
}
AnnotationSource coconut_annotation_source(CoconutImageNamespace source) {
    switch (source) {
        case CoconutImageNamespace::CocoTrain:
        case CoconutImageNamespace::CocoUnlabeled:
        case CoconutImageNamespace::CocoValidation: return AnnotationSource::CoconutCoco;
        case CoconutImageNamespace::Objects365V1: return AnnotationSource::CoconutObjects365V1;
        case CoconutImageNamespace::Objects365V2: return AnnotationSource::CoconutObjects365V2;
    }
    throw std::invalid_argument("invalid COCONut image namespace");
}
CoconutRecipeCatalog coconut_recipe_catalog(CoconutValidation validation) {
    CoconutRecipeCatalog catalog;
    for (const auto& release : coconut_release_catalog()) {
        if (release.edition == CoconutEdition::RelabeledValidation && validation == CoconutValidation::Stock) continue;
        if (release.edition == CoconutEdition::ObjectsValidation && validation != CoconutValidation::Coconut) continue;
        catalog.releases.push_back(release);
    }
    catalog.stock_annotations = coco_annotations_artifact();
    catalog.images = {{CoconutImageNamespace::CocoTrain, 0, "train2017", coco_train_images_artifact()},
                      {CoconutImageNamespace::CocoUnlabeled, 0, "unlabeled2017", coconut_unlabeled_images_artifact()},
                      {CoconutImageNamespace::CocoValidation, 0, "val2017", coco_val_images_artifact()}};
    const auto objects = objects365_train_image_artifacts();
    std::vector<bool> admitted(objects.size(), false);
    for (const auto edition : {CoconutEdition::Large, CoconutEdition::XLarge}) {
        for (const auto shard : coconut_objects_training_shards(edition)) {
            if (admitted.at(shard)) continue;
            admitted[shard] = true;
            catalog.images.push_back(
                {CoconutImageNamespace::Objects365V2, static_cast<std::uint16_t>(shard), "patch-" + std::to_string(shard), objects.at(shard)});
        }
    }
    if (validation == CoconutValidation::Coconut)
        catalog.images.push_back({CoconutImageNamespace::Objects365V1, 0, "validation", coconut_validation_images_artifact()});
    return catalog;
}
CoconutRecipePreparation prepare_coconut_recipe(const BenchmarkCompilerConfig& config, const BenchmarkCacheLayout& cache, const CoconutRecipeCatalog& catalog,
                                                std::span<const AdmittedRecipeArchive> acquired, const CoconutPhysicalMembership& physical,
                                                ProgressReporter& progress, CoconutFailureReport& failures, std::size_t workers,
                                                mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace,
                                                std::span<const CoconutImageNamespace> refreshed_sources) {
    using namespace mmltk::common::math;
    CoconutRecipePreparation prepared;
    ArtifactProgressTotals totals;
    StorageReservationPool reservations(cache.root, trace);
    const auto acquire = [&](const CatalogArtifact& artifact, std::string_view owner, bool redownload = false) {
        progress.phase(DatasetCompilePhase::Downloading);
        auto request = make_download_request(cache, owner, artifact);
        const auto reservation =
            reservations.reserve(additional_download_bytes(request.destination, artifact.expected_size), "additional COCONut annotation download bytes");
        request.redownload = redownload;
        return download_artifacts({request}, workers, cancellation,
                                  progress.transfer_observer_enabled()
                                      ? DownloadProgressSink{[&](const DownloadProgress& update) { totals.update(update, progress); }}
                                      : DownloadProgressSink{},
                                  trace)
            .front();
    };
    progress.phase(DatasetCompilePhase::Downloading);
    std::map<CoconutImageNamespace, std::string> physical_identities;
    for (const auto& archive : acquired) physical_identities[archive.origin.source] += archive.download.identity;
    prepared.manifest = {
        {"dataset", "coconut"}, {"validation", config.selection.validation}, {"components", nlohmann::json::array()}, {"artifacts", nlohmann::json::array()}};
    for (const auto& release : catalog.releases) {
        const auto sources = edition_sources(release.edition);
        const auto owner = std::string("coconut-") + std::string(release.name);
        ArtifactLease lease = ArtifactLease::acquire(cache.locks / (std::string(release.name) + ".annotations.lifecycle.lock"), cancellation);
        std::vector<DownloadResult> downloads;
        const auto first_artifact = prepared.manifest["artifacts"].size();
        std::string identity = std::string(release.revision) + std::string(kCoconutNormalizationRevision);
        for (const auto source : sources) identity += physical_identities[source];
        for (const auto& artifact : release.annotations) {
            downloads.push_back(acquire(artifact, owner));
            identity += downloads.back().identity;
            prepared.annotation_storage_bytes =
                checked_add(prepared.annotation_storage_bytes, downloads.back().size, "COCONut annotation archive storage overflow");
            prepared.manifest["artifacts"].push_back({{"artifact_id", artifact.artifact_id},
                                                      {"url", artifact.url},
                                                      {"filename", artifact.filename},
                                                      {"expected_size", artifact.expected_size},
                                                      {"expected_sha256", artifact.expected_sha256},
                                                      {"identity", downloads.back().identity}});
        }
        identity = digest_text(identity);
        const auto path_for = [&](CoconutImageNamespace source) {
            return cache.source_indexes(owner) / (std::string(coconut_namespace_name(source)) + ".normalized.bin");
        };
        std::vector<CoconutComponent> components;
        bool complete =
            std::ranges::none_of(sources, [&](const auto source) { return std::ranges::find(refreshed_sources, source) != refreshed_sources.end(); });
        for (auto source : sources) {
            if (!complete) break;
            auto cached = load_coconut_component(path_for(source), release.edition, source, identity, cancellation);
            if (!cached) {
                complete = false;
                break;
            }
            components.push_back(std::move(*cached));
        }
        if (!complete) {
            prepared.annotation_cache_hit = false;
            components.clear();
            progress.source_activity(BenchmarkDatasetSource::kCoconut, "Normalizing required " + std::string(release.name) + " annotations and masks");
            CoconutImportRequest request;
            request.edition = release.edition;
            request.input_identity = identity;
            request.physical_membership = &physical;
            request.expected_rows = release.expected_rows;
            request.cancellation = cancellation;
            request.rejected_object = [&](const CoconutPhysicalImage& physical_image, const CoconutRecord& record, const CoconutSegment& segment,
                                          std::string_view reason) {
                failures.reject(physical_image, record.image_id, release.name, segment.id, segment.category_id, reason);
            };
            if (progress.normalization_observer_enabled())
                request.progress = [&](std::uint64_t rows) { progress.phase(DatasetCompilePhase::Indexing, rows, release.expected_rows); };
            else
                progress.phase(DatasetCompilePhase::Indexing, 0, release.expected_rows);
            for (const auto& artifact : downloads) {
                if (artifact.path.extension() == ".parquet")
                    request.parquet_shards.push_back(artifact.path);
                else if (artifact.path.extension() == ".json")
                    request.annotation_json = artifact.path;
                else
                    request.mask_archive = artifact.path;
            }
            for (unsigned attempt = 1;; ++attempt) {
                try {
                    components = import_coconut_annotations(request);
                    break;
                } catch (const CoconutPhysicalMembershipError&) { throw; } catch (const InsufficientBenchmarkStorage&) {
                    throw;
                } catch (const std::exception& error) {
                    throw_if_benchmark_cancelled(cancellation);
                    if (attempt == 3) throw;
                    bool repaired = false;
                    std::string refreshed_identity = std::string(release.revision) + std::string(kCoconutNormalizationRevision);
                    for (const auto source : sources) refreshed_identity += physical_identities[source];
                    for (std::size_t i = 0; i < release.annotations.size(); ++i) {
                        const auto& artifact = release.annotations[i];
                        auto download_request = make_download_request(cache, owner, artifact);
                        bool matches_expected = false;
                        if (std::filesystem::is_regular_file(download_request.destination)) {
                            const auto sha = mmltk::common::io::sha256_hex(
                                mmltk::common::io::sha256_file(download_request.destination, [&] { return cancellation.requested(); }));
                            matches_expected = !artifact.expected_sha256.empty() && sha == artifact.expected_sha256;
                            trace_benchmark_event(trace, "benchmark.download.failure_sha256", [&] {
                                return nlohmann::json{{"artifact", artifact.artifact_id},
                                                      {"sha256", sha},
                                                      {"matches_expected", matches_expected},
                                                      {"reason", error.what()}};
                            });
                        }
                        if (!matches_expected) {
                            invalidate_download_artifact(download_request, cancellation, trace);
                            downloads[i] = acquire(artifact, owner, true);
                            repaired = true;
                        }
                        refreshed_identity += downloads[i].identity;
                    }
                    if (!repaired) throw;
                    request.input_identity = digest_text(refreshed_identity);
                    progress.source_activity(BenchmarkDatasetSource::kCoconut, "Retrying required COCONut masks after source repair");
                }
            }
            for (const auto& component : components)
                if (std::ranges::find(sources, component.source) == sources.end())
                    throw std::runtime_error("COCONut release references a namespace outside its selected recipe");
            for (const auto& component : components) {
                if (!component.index.images.empty()) store_coconut_component(path_for(component.source), component, cancellation);
            }
        }
        for (std::size_t i = 0; i < downloads.size(); ++i) prepared.manifest["artifacts"][first_artifact + i]["identity"] = downloads[i].identity;
        for (const auto& component : components)
            if (std::ranges::find(sources, component.source) == sources.end())
                throw std::runtime_error("COCONut release references a namespace outside its selected recipe");
        std::uint64_t admitted_rows = 0;
        for (const auto& component : components) admitted_rows = checked_add(admitted_rows, component.index.images.size(), "COCONut release count overflow");
        if (release.expected_rows != 0 && admitted_rows != release.expected_rows) throw std::runtime_error("COCONut cached release membership is incomplete");
        for (auto& component : components) {
            const auto index_path = path_for(component.source);
            for (const auto& path :
                 {index_path, std::filesystem::path(index_path.string() + ".inventory"), std::filesystem::path(index_path.string() + ".complete.json")})
                prepared.annotation_storage_bytes =
                    checked_add(prepared.annotation_storage_bytes, std::filesystem::file_size(path), "COCONut index storage overflow");
            prepared.manifest["components"].push_back({{"edition", release.edition},
                                                       {"revision", release.revision},
                                                       {"physical_source", coconut_namespace_name(component.source)},
                                                       {"input_identity", component.input_identity},
                                                       {"offered_images", component.index.images.size()},
                                                       {"annotation_source", coconut_annotation_source(component.source)},
                                                       {"imported_annotation_sha256", component.index.annotation_sha256},
                                                       {"imported_index", index_path.lexically_relative(cache.root).string()},
                                                       {"imported_index_identity", read_json_file(index_path.string() + ".complete.json").at("identity")}});
            prepared.components.push_back(std::move(component));
        }
    }
    prepared.duplicate_xl_images = reconcile_coconut_extensions(prepared.components, cancellation);
    std::erase_if(prepared.components, [](const CoconutComponent& component) { return component.index.images.empty(); });
    if (config.selection.validation == CoconutValidation::Stock) {
        CocoAnnotationCache stock(cache, catalog.stock_annotations, false, 0,
                                  checked_cast<std::uint32_t>(catalog.coco_validation_images, "COCO validation count overflow"), config.num_workers,
                                  cancellation, trace);
        stock.discover(progress);
        if (stock.pending_download()) {
            const auto& request = *stock.pending_download();
            const auto reservation =
                reservations.reserve(additional_download_bytes(request.destination, request.expected_size), "additional stock annotation download bytes");
            progress.phase(DatasetCompilePhase::Downloading);
            auto archive = download_artifacts({request}, workers, cancellation,
                                              progress.transfer_observer_enabled()
                                                  ? DownloadProgressSink{[&](const DownloadProgress& update) { totals.update(update, progress); }}
                                                  : DownloadProgressSink{},
                                              trace)
                               .front();
            auto completed = stock.completed_indexes();
            stock.settle(std::move(archive), progress, workers, completed, 1);
        }
        auto indexes = stock.take_indexes();
        prepared.annotation_cache_hit = prepared.annotation_cache_hit && indexes.cache_hit;
        prepared.annotation_storage_bytes = checked_add(prepared.annotation_storage_bytes, indexes.retained_storage_bytes, "stock annotation storage overflow");
        prepared.stock_validation = std::move(indexes.validation);
        prepared.validation_images = prepared.stock_validation->images.size();
    }
    // COCO shares one physical ID domain across its archive subsets; Objects365 editions remain distinct.
    const auto split_key = [](const CoconutPhysicalImage& image) {
        const auto source = image.source == CoconutImageNamespace::CocoUnlabeled || image.source == CoconutImageNamespace::CocoValidation
                                ? CoconutImageNamespace::CocoTrain
                                : image.source;
        return std::pair{source, image.image_id};
    };
    constexpr auto namespace_count = static_cast<std::size_t>(CoconutImageNamespace::Objects365V2) + 1;
    std::array<std::unordered_set<std::uint64_t>, namespace_count> train_members;
    const auto contains_training = [&](const CoconutPhysicalImage& image) {
        const auto [source, id] = split_key(image);
        return train_members[static_cast<std::size_t>(source)].contains(id);
    };
    std::uint64_t coco_val_count = prepared.validation_images;
    for (const auto& component : prepared.components) {
        if (!coconut_validation_component(component.edition)) {
            const auto source = split_key(component.inventory.front().physical).first;
            auto& membership = train_members[static_cast<std::size_t>(source)];
            membership.reserve(checked_add(membership.size(), component.inventory.size(), "COCONut split membership overflow"));
            for (const auto& image : component.inventory) {
                throw_if_benchmark_cancelled(cancellation);
                if (!membership.insert(image.physical.image_id).second) throw std::runtime_error("COCONut training contains an unreconciled physical member");
            }
        } else {
            prepared.validation_images = checked_add(prepared.validation_images, component.index.images.size(), "COCONut validation count overflow");
            if (component.source == CoconutImageNamespace::CocoValidation) coco_val_count = component.index.images.size();
        }
    }
    if (prepared.stock_validation)
        for (const auto& image : prepared.stock_validation->images)
            if (train_members[static_cast<std::size_t>(CoconutImageNamespace::CocoTrain)].contains(image.source_image_id))
                throw std::runtime_error("COCONut training reuses a stock validation physical ID");
    if (coco_val_count != catalog.coco_validation_images) throw std::runtime_error("COCONut COCO validation membership is incomplete");
    for (const auto& component : prepared.components) {
        if (coconut_validation_component(component.edition))
            for (const auto& image : component.inventory)
                if (contains_training(image.physical)) throw std::runtime_error("COCONut train and validation reuse a physical member");
    }
    for (auto& facts : prepared.manifest["components"]) {
        const auto found = std::ranges::find_if(prepared.components, [&](const CoconutComponent& component) {
            return facts["edition"] == component.edition && facts["physical_source"] == coconut_namespace_name(component.source);
        });
        const auto admitted = found == prepared.components.end() ? 0U : found->index.images.size();
        facts["admitted_images"] = admitted;
        facts["covered_by_large_images"] = facts["offered_images"].get<std::uint64_t>() - admitted;
        facts["selected_annotation_sha256"] = found == prepared.components.end() ? nlohmann::json(nullptr) : nlohmann::json(found->index.annotation_sha256);
    }
    for (const auto& admitted : acquired) {
        const auto& archive = admitted.origin;
        prepared.manifest["artifacts"].push_back({{"artifact_id", archive.artifact.artifact_id},
                                                  {"url", archive.artifact.url},
                                                  {"filename", archive.artifact.filename},
                                                  {"expected_size", archive.artifact.expected_size},
                                                  {"expected_sha256", archive.artifact.expected_sha256},
                                                  {"physical_source", coconut_namespace_name(archive.source)},
                                                  {"identity", admitted.download.identity},
                                                  {"bytes", admitted.download.size}});
    }
    if (config.selection.validation == CoconutValidation::Stock)
        prepared.manifest["artifacts"].push_back({{"artifact_id", catalog.stock_annotations.artifact_id},
                                                  {"url", catalog.stock_annotations.url},
                                                  {"expected_size", catalog.stock_annotations.expected_size},
                                                  {"expected_sha256", catalog.stock_annotations.expected_sha256}});
    prepared.manifest["duplicate_xl_images"] = prepared.duplicate_xl_images;
    prepared.manifest["validation_images"] = prepared.validation_images;
    return prepared;
}
}  // namespace mmltk::backend::data::benchmark_internal
