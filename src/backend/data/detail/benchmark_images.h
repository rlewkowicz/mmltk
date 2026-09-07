#pragma once  // backend.data private implementation boundary

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmark_cache.h"

namespace mmltk::backend::data::benchmark_internal {

// Parses the "quarantined" manifest array shared by the cached-image manifests: each record must
// carry a reason and reference a requested image id. Returns false when the array is malformed.
// append receives (image_id, reason) per record.
template <typename Records, typename Append>
[[nodiscard]] bool parse_quarantined_manifest_records(const Records& records, const std::span<const std::uint64_t> requested_image_ids,
                                                      Append&& append) {
    if (!records.is_array()) { return false; }
    for (const auto& record : records) {
        const std::uint64_t image_id = record.value("image_id", std::uint64_t{0U});
        std::string reason = record.value("reason", std::string{});
        if (reason.empty() || !std::ranges::binary_search(requested_image_ids, image_id)) { return false; }
        append(image_id, std::move(reason));
    }
    return true;
}

using ArchiveImageIdParser = std::function<std::optional<std::uint64_t>(std::string_view)>;
using CachedImageProgress = std::function<void(std::uint64_t, std::uint64_t)>;
using CachedImageValidator = std::function<void(std::uint64_t, std::span<const std::uint8_t>)>;

struct CachedImageRejection {
    std::uint64_t image_id = 0U;
    std::string reason;
};

struct CachedImageDirectory {
    std::string source;
    std::string shard;
    std::filesystem::path path;
    std::string identity;
    std::string selection_sha256;
    std::uint64_t image_count = 0U;
    std::uint64_t image_bytes = 0U;
    bool cache_hit = false;
    std::vector<CachedImageRejection> quarantined;
};

[[nodiscard]] std::string cached_image_selection_digest(std::span<const std::uint64_t> image_ids);
[[nodiscard]] std::filesystem::path cached_image_path(const std::filesystem::path& root, std::uint64_t image_id);
void prepare_cached_image_directory(const std::filesystem::path& root);
[[nodiscard]] std::size_t format_cached_image_relative_path(std::uint64_t image_id, std::span<char> output);
[[nodiscard]] bool has_complete_jpeg_markers(std::span<const std::uint8_t> encoded) noexcept;

void write_cached_image_atomically(const std::filesystem::path& path, std::span<const std::uint8_t> encoded,
                                   mmltk::common::concurrency::CancellationObservation cancellation);

[[nodiscard]] bool validate_cached_image_group(const std::filesystem::path& root, const std::filesystem::path& completion_path,
                                               std::string_view identity, std::span<const std::uint64_t> expected_image_ids,
                                               std::uint64_t* image_bytes,
                                               mmltk::common::concurrency::CancellationObservation cancel_requested,
                                               const BenchmarkTraceSink& trace = {},
                                               std::vector<CachedImageRejection>* quarantined = nullptr);

void complete_cached_image_group(const std::filesystem::path& root, const std::filesystem::path& completion_path, std::string_view identity,
                                 std::span<const std::uint64_t> expected_image_ids, std::uint64_t image_bytes,
                                 mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace = {},
                                 std::span<const CachedImageRejection> quarantined = {});

// Everything one archive extraction needs. The knobs live here instead of in a positional parameter
// list so the entry point keeps a single signature that callers and the definition cannot drift.
struct ArchiveExtractionRequest {
    std::filesystem::path archive_path;
    std::string source_identity;
    std::filesystem::path output_root;
    std::string source;
    std::string shard;
    std::span<const std::uint64_t> selected_image_ids;
    ArchiveImageIdParser image_id_parser;
    mmltk::common::concurrency::CancellationObservation cancel_requested = {};
    CachedImageProgress progress;
    CachedImageValidator validator;
    BenchmarkTraceSink trace;
    bool quarantine_unavailable = false;
    std::size_t decompression_workers = 1U;
    std::size_t cache_write_workers = 1U;
    std::function<void(std::string_view)> activity;
};

[[nodiscard]] CachedImageDirectory extract_selected_archive_images(ArchiveExtractionRequest request);

}  // namespace mmltk::backend::data::benchmark_internal
