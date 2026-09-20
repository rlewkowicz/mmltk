#include "detail/benchmark_annotation_cache.h"
#include "detail/benchmark_storage.h"
#include "detail/benchmark_progress.h"
#include "detail/staging_file_cleanup.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/common/io/staging_directory.h"
#include "src/common/math/checked_arithmetic.h"
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <memory>
#include <span>
#include <utility>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
namespace common_io = mmltk::common::io;
namespace common_math = mmltk::common::math;
namespace {
constexpr std::size_t kArchiveReadBufferBytes = std::size_t{1024U} * 1024U;
struct ArchiveDestroy {
    void operator()(archive* reader) const noexcept {
        if (reader != nullptr) { (void)archive_read_free(reader); }
    }
};
using ArchiveReader = std::unique_ptr<archive, ArchiveDestroy>;
[[nodiscard]] std::string archive_error(const archive* reader) {
    const char* message = archive_error_string(const_cast<archive*>(reader));
    return message != nullptr ? message : "unknown libarchive error";
}
[[nodiscard]] std::filesystem::path extract_manifest_path(const std::filesystem::path& path) { return path.string() + ".extract.json"; }
}
[[nodiscard]] std::string extract_archive_member(const std::filesystem::path& archive_path, const std::string_view member_suffix,
                                                 const std::filesystem::path& output_path, const std::string_view archive_identity,
                                                 const std::filesystem::path& lock_path, mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                 const BenchmarkTraceSink& trace) {
    ArtifactLease lease = ArtifactLease::acquire(lock_path, cancel_requested);
    const std::filesystem::path completion = extract_manifest_path(output_path);
    if (std::filesystem::is_regular_file(output_path) && std::filesystem::is_regular_file(completion)) {
        try {
            const nlohmann::json metadata = read_json_file(completion);
            if (metadata.value("schema_version", 0U) == kBenchmarkCacheSchemaVersion && metadata.value("complete", false) &&
                metadata.value("archive_identity", std::string{}) == archive_identity && metadata.value("member", std::string{}) == member_suffix &&
                metadata.value("size", 0ULL) == std::filesystem::file_size(output_path)) {
                const std::string identity = metadata.value("identity", std::string{});
                if (!identity.empty()) {
                    trace_benchmark_event(trace, "benchmark.archive.extract_cache_hit",
                                          [&] { return nlohmann::json{{"member", member_suffix}, {"path", output_path.string()}}; });
                    return identity;
                }
            }
        } catch (const std::exception& error) {
            trace_benchmark_event(trace, "benchmark.archive.extract_cache_invalid",
                                  [&] { return nlohmann::json{{"member", member_suffix}, {"path", output_path.string()}, {"reason", error.what()}}; });
        }
    }
    std::error_code remove_error;
    std::filesystem::remove(output_path, remove_error);
    if (remove_error) { throw std::filesystem::filesystem_error("cannot remove invalid extracted annotation", output_path, remove_error); }
    remove_error.clear();
    std::filesystem::remove(completion, remove_error);
    if (remove_error) { throw std::filesystem::filesystem_error("cannot remove invalid extraction metadata", completion, remove_error); }
    ArchiveReader reader(archive_read_new());
    if (!reader) { throw std::runtime_error("cannot allocate benchmark annotation archive reader"); }
    if (archive_read_support_filter_all(reader.get()) < ARCHIVE_WARN || archive_read_support_format_tar(reader.get()) < ARCHIVE_WARN ||
        archive_read_support_format_zip(reader.get()) < ARCHIVE_WARN ||
        archive_read_open_filename(reader.get(), archive_path.c_str(), kArchiveReadBufferBytes) < ARCHIVE_WARN) {
        throw std::runtime_error("cannot open benchmark annotation archive: " + archive_error(reader.get()));
    }
    archive_entry* entry = nullptr;
    bool found = false;
    std::string staging_text = output_path.string() + ".tmp.XXXXXX";
    common_io::FileHandle staging;
    std::filesystem::path staging_path;
    std::optional<StagingFileCleanup> staging_cleanup;
    std::uint64_t expected_size = 0U;
    while (true) {
        throw_if_benchmark_cancelled(cancel_requested);
        int status = ARCHIVE_RETRY;
        for (std::uint32_t retry = 0U; status == ARCHIVE_RETRY && retry < 8U; ++retry) { status = archive_read_next_header(reader.get(), &entry); }
        if (status == ARCHIVE_EOF) { break; }
        if (status != ARCHIVE_OK && status != ARCHIVE_WARN) {
            throw std::runtime_error("cannot read benchmark annotation archive: " + archive_error(reader.get()));
        }
        const char* pathname = archive_entry_pathname(entry);
        const std::string_view name = pathname != nullptr ? std::string_view(pathname) : std::string_view{};
        if (!name.ends_with(member_suffix)) {
            int skip_status = ARCHIVE_RETRY;
            for (std::uint32_t retry = 0U; skip_status == ARCHIVE_RETRY && retry < 8U; ++retry) { skip_status = archive_read_data_skip(reader.get()); }
            if (skip_status < ARCHIVE_WARN) { throw std::runtime_error("cannot skip benchmark annotation archive member"); }
            continue;
        }
        if (found || archive_entry_filetype(entry) != AE_IFREG || archive_entry_size(entry) <= 0) {
            throw std::runtime_error("benchmark annotation archive member is missing or ambiguous");
        }
        found = true;
        expected_size = common_math::checked_cast<std::uint64_t>(archive_entry_size(entry), "extracted annotation size overflow");
        require_storage(output_path, expected_size, "extracted annotation staging", trace);
        staging = common_io::FileHandle::create_unique_output(staging_text,
                                                              common_math::checked_cast<std::size_t>(expected_size, "extracted annotation size overflow"));
        staging_path = staging_text;
        staging_cleanup.emplace(staging_path);
        std::vector<std::uint8_t> buffer(kArchiveReadBufferBytes);
        std::uint64_t offset = 0U;
        while (offset < expected_size) {
            throw_if_benchmark_cancelled(cancel_requested);
            const std::size_t remaining = common_math::checked_cast<std::size_t>(expected_size - offset, "extraction remaining size overflow");
            la_ssize_t count = ARCHIVE_RETRY;
            for (std::uint32_t retry = 0U; count == ARCHIVE_RETRY && retry < 8U; ++retry) {
                count = archive_read_data(reader.get(), buffer.data(), std::min(buffer.size(), remaining));
            }
            if (count <= 0) { throw std::runtime_error("cannot read complete benchmark annotation archive member"); }
            staging.pwrite_all(buffer.data(), common_math::checked_cast<std::size_t>(count, "archive read overflow"),
                               common_math::checked_cast<std::size_t>(offset, "archive write offset overflow"));
            offset += common_math::checked_cast<std::uint64_t>(count, "archive read overflow");
        }
    }
    if (!found) { throw std::runtime_error("benchmark annotation archive does not contain " + std::string(member_suffix)); }
    staging.sync_data();
    staging = common_io::FileHandle{};
    std::string identity_material;
    identity_material.reserve(archive_identity.size() + member_suffix.size() + 64U);
    identity_material.append(archive_identity);
    identity_material.push_back('\n');
    identity_material.append(member_suffix);
    identity_material.push_back('\n');
    identity_material.append(std::to_string(expected_size));
    const std::string identity = mmltk::common::io::sha256_hex(
        mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(identity_material.data()), identity_material.size())));
    throw_if_benchmark_cancelled(cancel_requested);
    common_io::publish_staged_path_atomically(staging_path, output_path, true);
    write_json_atomically(completion,
                          nlohmann::json{{"schema_version", kBenchmarkCacheSchemaVersion},
                                         {"complete", true},
                                         {"archive_identity", archive_identity},
                                         {"member", member_suffix},
                                         {"size", expected_size},
                                         {"identity", identity},
                                         {"integrity_mode", "archive_structure_size"}},
                          cancel_requested);
    trace_benchmark_event(trace, "benchmark.archive.extracted",
                          [&] { return nlohmann::json{{"member", member_suffix}, {"bytes", expected_size}, {"identity", identity}}; });
    return identity;
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
        } catch (const InsufficientBenchmarkStorage&) {
            throw;
        } catch (const std::exception& error) {
            throw_if_benchmark_cancelled(cancel_requested);
            if (attempt == 3U) { throw; }
            repair(error);
        }
    }
}

std::vector<DownloadResult> repair_annotation_artifacts(std::vector<DownloadRequest> requests, BenchmarkDatasetSource source,
    std::string_view reason, ProgressReporter& progress, ArtifactProgressTotals& totals, std::size_t workers,
    mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace) {
    for (auto& request : requests) {
        if (std::filesystem::is_regular_file(request.destination)) {
            progress.source_activity(source, "Failure-only SHA-256 diagnosis for " + request.artifact_id);
            const auto sha = common_io::sha256_hex(common_io::sha256_file(request.destination, [&] { return cancellation.requested(); }));
            trace_benchmark_event(trace, "benchmark.download.failure_sha256", [&] {
                return nlohmann::json{{"artifact", request.artifact_id}, {"sha256", sha}, {"reason", reason}};
            });
        }
        invalidate_download_artifact(request, cancellation, trace);
        request.redownload = true;
    }
    progress.source_activity(source, "Redownloading annotation metadata after structural validation failure");
    return download_artifacts(requests,
        requests.size() == 1U ? std::min<std::size_t>(8U, workers) : std::min<std::size_t>({3U, requests.size(), workers}), cancellation,
        progress.transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { totals.update(update, progress); }}
                                            : DownloadProgressSink{}, trace);
}
CocoAnnotationCache::CocoAnnotationCache(const BenchmarkCacheLayout& cache, const CatalogArtifact& artifact, bool training,
    std::uint32_t train_count, std::uint32_t validation_count, int parse_workers,
    mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace)
    : cache_(cache), request_(make_download_request(cache, "coco", artifact)), training_(training), train_count_(train_count),
      validation_count_(validation_count), parse_workers_(parse_workers), cancellation_(cancellation), trace_(trace),
      lease_(ArtifactLease::acquire(cache.locks / "coco-annotations.lifecycle.lock", cancellation)) {
    indexes_.train_path = cache.source_indexes("coco") / "train2017.normalized.bin";
    indexes_.validation_path = cache.source_indexes("coco") / "val2017.normalized.bin";
}
void CocoAnnotationCache::discover(ProgressReporter& progress) {
    if (training_) {
        progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Validating cached COCO train annotation index");
        indexes_.train = discover_cached_index(indexes_.train_path, BenchmarkDatasetSource::kCoco2017, "train2017", cancellation_, trace_);
    }
    progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Validating cached COCO validation annotation index");
    indexes_.validation = discover_cached_index(indexes_.validation_path, BenchmarkDatasetSource::kCoco2017, "val2017", cancellation_, trace_);
    indexes_.cache_hit = (!training_ || indexes_.train.has_value()) && indexes_.validation.has_value();
    if (!indexes_.cache_hit) pending_ = request_;
}
std::uint64_t CocoAnnotationCache::completed_indexes() const noexcept {
    return static_cast<std::uint64_t>(indexes_.train.has_value()) + static_cast<std::uint64_t>(indexes_.validation.has_value());
}
std::filesystem::path CocoAnnotationCache::source_json(bool training) const {
    return cache_.source_indexes("coco") / "source-json" / (training ? "instances_train2017.json" : "instances_val2017.json");
}
void CocoAnnotationCache::build_split(bool training, const DownloadResult& archive, ProgressReporter& progress,
    std::uint64_t& completed, std::uint64_t total) {
    auto& index = training ? indexes_.train : indexes_.validation;
    if (index) return;
    const std::string split = training ? "train2017" : "val2017";
    const std::string label = training ? "train" : "validation";
    const auto json_path = source_json(training);
    std::filesystem::create_directories(json_path.parent_path());
    progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Extracting COCO " + label + " annotations");
    const auto digest = extract_archive_member(archive.path, "annotations/instances_" + split + ".json", json_path, archive.identity,
        cache_.locks / (training ? "coco-train-json.extract.lock" : "coco-val-json.extract.lock"), cancellation_, trace_);
    index = load_or_build_index(cache_, training ? indexes_.train_path : indexes_.validation_path, BenchmarkDatasetSource::kCoco2017,
        split, digest, cancellation_, trace_, [&] {
            progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Parsing and indexing COCO " + label + " annotations");
            return parse_coco_style_annotations(json_path, digest, coco_category_mappings(),
                AnnotationParseOptions{BenchmarkDatasetSource::kCoco2017, split, training ? train_count_ : validation_count_,
                    parse_workers_, !training, cancellation_, trace_});
        });
    progress.phase(DatasetCompilePhase::Indexing, ++completed, total);
}
void CocoAnnotationCache::invalidate_missing() {
    for (const bool training : {true, false}) {
        if ((training && !training_) || (training ? indexes_.train.has_value() : indexes_.validation.has_value())) continue;
        const auto json = source_json(training);
        const auto& path = training ? indexes_.train_path : indexes_.validation_path;
        remove_cache_path(json.string() + ".extract.json");
        remove_cache_path(json);
        remove_cache_path(path.string() + ".complete.json");
        remove_cache_path(path);
    }
}
void CocoAnnotationCache::settle(DownloadResult archive, ProgressReporter& progress, std::size_t workers,
    std::uint64_t& completed, std::uint64_t total) {
    if (!pending_) throw std::logic_error("COCO annotation cache has no pending download");
    ArtifactProgressTotals repair_progress;
    retry_annotation_indexing(cancellation_, [&] {
        if (training_) build_split(true, archive, progress, completed, total);
        build_split(false, archive, progress, completed, total);
    }, [&](const std::exception& error) {
        invalidate_missing();
        archive = repair_annotation_artifacts({request_}, BenchmarkDatasetSource::kCoco2017, error.what(), progress,
            repair_progress, workers, cancellation_, trace_).front();
    });
    pending_.reset();
}
CocoAnnotationIndexes CocoAnnotationCache::take_indexes() {
    if (pending_ || !indexes_.validation || (training_ && !indexes_.train)) throw std::logic_error("COCO annotation indexes are not settled");
    std::uint64_t storage = 0;
    const auto account = [&](const std::filesystem::path& path) {
        if (std::filesystem::is_regular_file(path)) storage = common_math::checked_add(storage, std::filesystem::file_size(path), "stock annotation storage overflow");
    };
    account(request_.destination);
    account(request_.destination.string() + ".download.json");
    for (const bool training : {true, false}) {
        if (training && !training_) continue;
        const auto& path = training ? indexes_.train_path : indexes_.validation_path;
        const auto json = source_json(training);
        account(path); account(path.string() + ".complete.json"); account(json); account(json.string() + ".extract.json");
    }
    indexes_.retained_storage_bytes = storage;
    return std::move(indexes_);
}
}
