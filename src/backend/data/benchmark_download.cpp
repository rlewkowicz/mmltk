#include <curl/curl.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <list>
#include <mutex>
#include <ranges>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "src/backend/data/benchmark_hash.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/common/types/string_utils.h"
// CLEANUP-IGNORE: This download unit names its boundary-specific imports and private headers.
#include "benchmark_curl.h"
#include "detail/benchmark_download.h"
namespace mmltk::backend::data::benchmark_internal {
[[nodiscard]] DownloadRequest make_download_request(const BenchmarkCacheLayout& cache, const std::string_view source, const CatalogArtifact& artifact) {
    DownloadRequest request;
    request.artifact_id = artifact.artifact_id;
    request.source = artifact.source;
    request.url = artifact.url;
    request.destination = cache.source_downloads(source) / artifact.filename;
    request.lock_path = cache.locks / (artifact.artifact_id + ".lock");
    request.expected_size = artifact.expected_size;
    if (!artifact.expected_sha256.empty()) { request.expected_sha256 = artifact.expected_sha256; }
    request.maximum_attempts = kMaximumAttempts;
    return request;
}
using mmltk::common::io::errno_error;
using mmltk::common::io::ScopedFd;
using mmltk::common::io::sync_parent_directory;
using mmltk::common::math::checked_add;
using mmltk::common::math::checked_cast;
using mmltk::common::types::trim_http_field_value;
namespace {
using Clock = std::chrono::steady_clock;
void trace_transfer_progress(const BenchmarkTraceSink& trace, const DownloadRequest& request, const std::uint64_t completed,
                             const std::uint64_t total, const std::uint32_t attempt, const bool resumed,
                             const std::uint64_t retained, const std::uint64_t durable, const bool redownload) {
    trace_benchmark_event(trace, "benchmark.download.progress", [&] {
        return nlohmann::json{{"artifact", request.artifact_id}, {"completed_bytes", completed}, {"total_bytes", total},
                              {"attempt", attempt}, {"resumed", resumed}, {"cache_hit", false},
                              {"retained_bytes", retained}, {"durable_bytes", durable}, {"redownload", redownload}};
    });
}

class CurlGlobal {
   public:
    static void initialize() { ensure_curl_global_initialized("cannot initialize libcurl: "); }
};
[[nodiscard]] std::filesystem::path complete_metadata_path(const DownloadRequest& request) { return request.destination.string() + ".download.json"; }
[[nodiscard]] std::filesystem::path partial_path(const DownloadRequest& request) { return request.destination.string() + ".part"; }
[[nodiscard]] std::filesystem::path partial_metadata_path(const DownloadRequest& request) { return request.destination.string() + ".part.json"; }
[[nodiscard]] std::uint64_t regular_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status)) { return 0U; }
    return std::filesystem::file_size(path);
}
[[nodiscard]] std::string artifact_identity(const DownloadRequest& request, const std::uint64_t size, const std::string_view etag,
                                            const std::string_view last_modified) {
    std::string material;
    material.reserve(request.url.size() + etag.size() + last_modified.size() + 64U);
    material.append(request.url);
    material.push_back('\n');
    material.append(std::to_string(size));
    material.push_back('\n');
    material.append(etag);
    material.push_back('\n');
    material.append(last_modified);
    return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(material.data()), material.size())));
}
[[nodiscard]] std::uint64_t checked_u64_add(const std::uint64_t left, const std::uint64_t right, const char* context) {
    return checked_add(left, right, context);
}
// Writes a whole response chunk at an absolute file offset, absorbing EINTR and short writes. Every download path
// lands its bytes this way, so the retry loop lives here instead of inside each libcurl write callback.
void write_all_at(const int descriptor, const std::uint8_t* source, const std::size_t bytes, const std::uint64_t offset, const char* offset_context,
                  const char* write_context, const char* progress_context) {
    std::size_t written = 0U;
    while (written < bytes) {
        const std::uint64_t write_at = checked_u64_add(offset, written, offset_context);
        const ssize_t result = ::pwrite(descriptor, source + written, bytes - written, checked_cast<off_t>(write_at, offset_context));
        if (result < 0) {
            if (errno == EINTR) { continue; }
            throw errno_error(write_context);
        }
        if (result == 0) { throw std::runtime_error(progress_context); }
        written += static_cast<std::size_t>(result);
    }
}
// Diagnostic contexts for one download shape's body writes. Each transfer names its own so failures
// stay attributable while the offset accounting itself stays shared.
struct DownloadWriteContext {
    const char* offset_overflow = nullptr;
    const char* write_failure = nullptr;
    const char* no_progress = nullptr;
    const char* size_overflow = nullptr;
};
// Lands a response chunk at the transfer's current write offset and advances it. Every download
// shape accounts for its body bytes here, so write-offset bookkeeping has a single owner.
void append_transfer_bytes(std::uint64_t* write_offset, const DownloadWriteContext& context, const int descriptor, const char* data, const std::size_t bytes) {
    write_all_at(descriptor, reinterpret_cast<const std::uint8_t*>(data), bytes, *write_offset, context.offset_overflow, context.write_failure,
                 context.no_progress);
    *write_offset = checked_u64_add(*write_offset, bytes, context.size_overflow);
}
[[nodiscard]] std::optional<DownloadResult> validate_complete_artifact(const DownloadRequest& request,
                                                                       mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                                       const DownloadProgressSink& progress, const BenchmarkTraceSink& trace) {
    (void)progress;
    const std::uint64_t size = regular_file_size(request.destination);
    if (size == 0U || (request.expected_size != 0U && size != request.expected_size)) { return std::nullopt; }
    bool has_completion_metadata = false;
    std::string metadata_identity;
    std::string etag;
    std::string last_modified;
    std::uint32_t attempts = 0U;
    const std::filesystem::path metadata_path = complete_metadata_path(request);
    if (std::filesystem::exists(metadata_path)) {
        has_completion_metadata = true;
        try {
            const nlohmann::json metadata = read_json_file(metadata_path);
            if (metadata.value("schema_version", 0U) != kBenchmarkCacheSchemaVersion || metadata.value("complete", false) == false ||
                metadata.value("url", std::string{}) != request.url || metadata.value("size", 0ULL) != size) {
                return std::nullopt;
            }
            metadata_identity = metadata.value("identity", std::string{});
            etag = metadata.value("etag", std::string{});
            last_modified = metadata.value("last_modified", std::string{});
            attempts = metadata.value("attempts", 0U);
        } catch (const std::exception&) { return std::nullopt; }
    }
    throw_if_benchmark_cancelled(cancel_requested);
    const std::string identity = artifact_identity(request, size, etag, last_modified);
    if (has_completion_metadata && (metadata_identity.empty() || metadata_identity != identity)) { return std::nullopt; }
    trace_benchmark_event(trace, has_completion_metadata ? "benchmark.download.cache_hit" : "benchmark.download.preseeded", [&] {
        return nlohmann::json{{"artifact", request.artifact_id},
                              {"bytes", size},
                              {"integrity", has_completion_metadata ? "atomic_http_identity" : "pending_structural_validation"}};
    });
    return DownloadResult{
        request.destination, size, identity, {}, etag, last_modified, attempts, false, true,
    };
}
void remove_invalid_complete_artifact(const DownloadRequest& request) {
    std::error_code error;
    std::filesystem::remove(request.destination, error);
    if (error) { throw std::filesystem::filesystem_error("cannot remove invalid benchmark download", request.destination, error); }
    error.clear();
    std::filesystem::remove(complete_metadata_path(request), error);
    if (error) { throw std::filesystem::filesystem_error("cannot remove invalid benchmark download metadata", complete_metadata_path(request), error); }
}
[[nodiscard]] bool starts_with_case_insensitive(const std::string_view value, const std::string_view prefix) noexcept {
    if (value.size() < prefix.size()) { return false; }
    for (std::size_t index = 0U; index < prefix.size(); ++index) {
        const char left = static_cast<char>(std::tolower(static_cast<unsigned char>(value[index])));
        const char right = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[index])));
        if (left != right) { return false; }
    }
    return true;
}
struct ParsedContentRange {
    std::uint64_t start = 0U;
    std::uint64_t end = 0U;
    std::uint64_t total = 0U;
    bool unsatisfied = false;
};
[[nodiscard]] std::optional<ParsedContentRange> parse_content_range(std::string_view header) {
    constexpr std::string_view kPrefix = "bytes ";
    header = trim_http_field_value(header);
    if (!starts_with_case_insensitive(header, kPrefix)) { return std::nullopt; }
    header.remove_prefix(kPrefix.size());
    const std::size_t slash = header.find('/');
    if (slash == std::string_view::npos) { return std::nullopt; }
    const std::string_view range = header.substr(0U, slash);
    const std::string_view total_text = header.substr(slash + 1U);
    ParsedContentRange parsed;
    const auto total = std::from_chars(total_text.data(), total_text.data() + total_text.size(), parsed.total);
    if (total.ec != std::errc{} || total.ptr != total_text.data() + total_text.size() || parsed.total == 0U) { return std::nullopt; }
    if (range == "*") {
        parsed.unsatisfied = true;
        return parsed;
    }
    const std::size_t dash = range.find('-');
    if (dash == std::string_view::npos) { return std::nullopt; }
    const std::string_view start_text = range.substr(0U, dash);
    const std::string_view end_text = range.substr(dash + 1U);
    const auto start = std::from_chars(start_text.data(), start_text.data() + start_text.size(), parsed.start);
    const auto end = std::from_chars(end_text.data(), end_text.data() + end_text.size(), parsed.end);
    if (start.ec != std::errc{} || start.ptr != start_text.data() + start_text.size() || end.ec != std::errc{} ||
        end.ptr != end_text.data() + end_text.size() || parsed.end < parsed.start || parsed.end >= parsed.total) {
        return std::nullopt;
    }
    return parsed;
}
enum class HttpHeaderKind : std::uint8_t {
    kStatusLine,
    kETag,
    kLastModified,
    kContentRange,
    kOther,
};
// The response-header fields every benchmark HTTP transfer tracks. apply() consumes one header
// line, resetting all fields on a status line and capturing the identity and range headers.
struct HttpResponseHeaderFields {
    long response_code = 0L;
    std::optional<ParsedContentRange> range;
    std::string etag;
    std::string last_modified;
    HttpHeaderKind apply(const std::string_view header) {
        if (starts_with_case_insensitive(header, "HTTP/")) {
            const std::size_t separator = header.find(' ');
            response_code = 0L;
            if (separator != std::string_view::npos) {
                const char* begin = header.data() + separator + 1U;
                (void)std::from_chars(begin, header.data() + header.size(), response_code);
            }
            range.reset();
            etag.clear();
            last_modified.clear();
            return HttpHeaderKind::kStatusLine;
        }
        if (starts_with_case_insensitive(header, "ETag:")) {
            etag = trim_http_field_value(header.substr(5U));
            return HttpHeaderKind::kETag;
        }
        if (starts_with_case_insensitive(header, "Last-Modified:")) {
            last_modified = trim_http_field_value(header.substr(14U));
            return HttpHeaderKind::kLastModified;
        }
        if (starts_with_case_insensitive(header, "Content-Range:")) {
            range = parse_content_range(header.substr(14U));
            return HttpHeaderKind::kContentRange;
        }
        return HttpHeaderKind::kOther;
    }
};
[[nodiscard]] bool is_header_block_terminator(const std::string_view header) noexcept { return header == "\r\n" || header == "\n"; }
struct Transfer {
    static constexpr DownloadWriteContext kWriteContext{
        .offset_overflow = "partial write offset overflow",
        .write_failure = "cannot write benchmark partial download",
        .no_progress = "benchmark partial download write made no progress",
        .size_overflow = "partial download size overflow",
    };
    const DownloadRequest& request;
    DownloadProgressSink progress;
    BenchmarkTraceSink trace;
    mmltk::common::concurrency::CancellationObservation cancel_requested = {};
    CurlEasy easy;
    CurlHeaders headers;
    ScopedFd partial;
    std::uint64_t requested_resume_offset = 0U;
    std::uint64_t resume_offset = 0U;
    std::uint64_t write_offset = 0U;
    std::uint64_t response_total = 0U;
    std::uint32_t attempt = 0U;
    bool response_restarted = false;
    bool resumed = false;
    bool response_headers_valid = false;
    bool callback_retryable = false;
    std::exception_ptr callback_error;
    HttpResponseHeaderFields http;
    std::string resume_etag;
    std::string resume_last_modified;
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    bool redownload = false;
    Clock::time_point last_progress{};
    curl_off_t last_reported_download_now = -1;
    Transfer(const DownloadRequest& request_value, DownloadProgressSink progress_value, BenchmarkTraceSink trace_value,
             mmltk::common::concurrency::CancellationObservation cancel, const std::uint32_t attempt_value, const bool redownload_value)
        : request(request_value), progress(std::move(progress_value)), trace(std::move(trace_value)), cancel_requested(cancel), attempt(attempt_value),
          redownload(request_value.redownload || redownload_value) {
        prepare_partial();
        prepare_easy();
    }
    void prepare_partial() {
        (void)mmltk::common::io::ensure_parent_directory(request.destination);
        const std::filesystem::path part_path = partial_path(request);
        bool metadata_matches = false;
        const std::filesystem::path metadata_path = partial_metadata_path(request);
        if (std::filesystem::exists(metadata_path)) {
            try {
                const nlohmann::json metadata = read_json_file(metadata_path);
                metadata_matches = metadata.value("schema_version", 0U) == kBenchmarkCacheSchemaVersion && metadata.value("url", std::string{}) == request.url;
                resume_etag = metadata.value("etag", std::string{});
                resume_last_modified = metadata.value("last_modified", std::string{});
            } catch (const std::exception&) { metadata_matches = false; }
        }
        if (!metadata_matches) {
            std::error_code ignored;
            std::filesystem::remove(part_path, ignored);
            std::filesystem::remove(metadata_path, ignored);
            resume_etag.clear();
            resume_last_modified.clear();
        }
        const int descriptor = ::open(part_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (descriptor < 0) { throw errno_error("cannot open benchmark partial download", part_path.string()); }
        partial = ScopedFd(descriptor);
        struct stat status{};
        if (::fstat(descriptor, &status) != 0) { throw errno_error("cannot inspect benchmark partial download", part_path.string()); }
        resume_offset = checked_cast<std::uint64_t>(status.st_size, "partial download size overflow");
        if (request.expected_size != 0U && resume_offset > request.expected_size) {
            if (::ftruncate(descriptor, 0) != 0) { throw errno_error("cannot reset oversized benchmark partial download", part_path.string()); }
            resume_offset = 0U;
        }
        if (resume_offset != 0U && resume_etag.empty() && resume_last_modified.empty()) {
            if (::ftruncate(descriptor, 0) != 0) { throw errno_error("cannot reset unvalidated benchmark partial download", part_path.string()); }
            resume_offset = 0U;
        }
        requested_resume_offset = resume_offset;
        write_offset = resume_offset;
        resumed = resume_offset != 0U;
        write_json_atomically(metadata_path,
                              nlohmann::json{{"schema_version", kBenchmarkCacheSchemaVersion},
                                             {"url", request.url},
                                             {"etag", resume_etag},
                                             {"last_modified", resume_last_modified},
                                             {"bytes", resume_offset}},
                              cancel_requested);
    }
    void prepare_easy() {
        easy.reset(curl_easy_init());
        if (!easy) { throw std::runtime_error("cannot allocate benchmark libcurl handle"); }
        configure_curl_transfer(easy.get(),
                                CurlTransferSetup{
                                    .url = request.url.c_str(),
                                    .maximum_redirects = 10L,
                                    .error_buffer = error_buffer.data(),
                                    .owner = this,
                                    .write_callback = &Transfer::write_callback,
                                    .header_callback = &Transfer::header_callback,
                                    .progress_callback = &Transfer::progress_callback,
                                },
                                "cannot configure benchmark transfer ");
        set_curl_option_with_prefix(easy.get(), CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L, "cannot configure benchmark transfer ",
                                    "proxy CONNECT header suppression");
        if (resume_offset != 0U) {
            set_curl_option_with_prefix(easy.get(), CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(resume_offset), "cannot configure benchmark transfer ",
                                        "resume offset");
            const std::string validator = !resume_etag.empty()            ? "If-Range: " + resume_etag
                                          : !resume_last_modified.empty() ? "If-Range: " + resume_last_modified
                                                                          : std::string{};
            if (!validator.empty()) {
                curl_slist* appended = curl_slist_append(nullptr, validator.c_str());
                if (appended == nullptr) { throw std::runtime_error("cannot allocate benchmark resume header"); }
                headers.reset(appended);
                set_curl_option_with_prefix(easy.get(), CURLOPT_HTTPHEADER, headers.get(), "cannot configure benchmark transfer ", "resume headers");
            }
        }
    }
    static std::size_t write_callback(char* data, const std::size_t size, const std::size_t count, void* opaque) {
        return curl_run_data_callback<Transfer>(size, count, opaque, [data](Transfer& transfer, const std::size_t bytes) {
            if (transfer.http.response_code != 200L && transfer.http.response_code != 206L) { return bytes; }
            if (!transfer.response_headers_valid) {
                transfer.callback_retryable = true;
                throw std::runtime_error("benchmark response body arrived before a valid final header block");
            }
            const std::uint64_t bytes_u64 = checked_cast<std::uint64_t>(bytes, "partial download response size overflow");
            const std::uint64_t expected_total = transfer.request.expected_size != 0U ? transfer.request.expected_size : transfer.response_total;
            if (expected_total != 0U && (transfer.write_offset > expected_total || bytes_u64 > expected_total - transfer.write_offset)) {
                transfer.callback_retryable = true;
                throw std::runtime_error("benchmark response body exceeds its declared artifact size");
            }
            append_transfer_bytes(&transfer.write_offset, Transfer::kWriteContext, transfer.partial.get(), data, bytes);
            return bytes;
        });
    }
    static std::size_t header_callback(char* data, const std::size_t size, const std::size_t count, void* opaque) {
        return curl_run_header_callback<Transfer>(data, size, count, opaque);
    }
    void on_header(const HttpHeaderKind kind, const std::string_view header) {
        switch (kind) {
            case HttpHeaderKind::kStatusLine:
                response_total = 0U;
                response_headers_valid = false;
                response_restarted = false;
                break;
            case HttpHeaderKind::kContentRange:
                if (http.range) { response_total = http.range->total; }
                break;
            case HttpHeaderKind::kETag:
            case HttpHeaderKind::kLastModified: break;
            case HttpHeaderKind::kOther:
                if (is_header_block_terminator(header)) { finish_header_block(); }
                break;
        }
    }
    void finish_header_block() {
        bool valid = true;
        if (http.response_code == 206L) {
            valid = requested_resume_offset != 0U && http.range && !http.range->unsatisfied && http.range->start == requested_resume_offset &&
                    http.range->end == http.range->total - 1U;
        } else if (http.response_code == 416L) {
            valid = requested_resume_offset != 0U && http.range && http.range->unsatisfied && http.range->total == requested_resume_offset;
        } else if (http.response_code == 200L) {
            valid = true;
        }
        if (valid && request.expected_size != 0U && (http.response_code == 206L || http.response_code == 416L)) {
            valid = response_total == request.expected_size;
        }
        if (!valid) {
            callback_retryable = true;
            throw std::runtime_error("benchmark server returned an invalid HTTP byte range");
        }
        if (http.response_code == 200L && requested_resume_offset != 0U) {
            if (::ftruncate(partial.get(), 0) != 0) { throw errno_error("cannot restart benchmark partial download"); }
            write_offset = 0U;
            resume_offset = 0U;
            response_restarted = true;
            redownload = true;
            last_progress = {};
        }
        response_headers_valid = http.response_code == 200L || http.response_code == 206L || http.response_code == 416L;
    }
    static int progress_callback(void* opaque, const curl_off_t download_total, const curl_off_t download_now, curl_off_t, curl_off_t) {
        Transfer& transfer = *static_cast<Transfer*>(opaque);
        try {
            if (transfer.cancel_requested.requested()) { return 1; }
            if (!transfer.progress && !transfer.trace) { return 0; }
            const Clock::time_point now = Clock::now();
            if (transfer.last_progress.time_since_epoch().count() == 0 || now - transfer.last_progress >= std::chrono::milliseconds{100} ||
                (download_total > 0 && download_now >= download_total && download_now != transfer.last_reported_download_now)) {
                const std::uint64_t base = transfer.response_restarted ? 0U : transfer.resume_offset;
                const std::uint64_t completed = transfer.write_offset;
                std::uint64_t total = transfer.request.expected_size;
                if (total == 0U && transfer.response_headers_valid) {
                    total = transfer.response_total;
                    if (total == 0U && download_total > 0) {
                        total = checked_u64_add(base, checked_cast<std::uint64_t>(download_total, "download total overflow"), "download total overflow");
                    }
                }
                transfer.emit_progress(completed, total, base);
                transfer.last_progress = now;
                transfer.last_reported_download_now = download_now;
            }
            return 0;
        } catch (...) {
            transfer.callback_error = std::current_exception();
            return 1;
        }
    }
    void emit_progress(const std::uint64_t completed, const std::uint64_t total, const std::uint64_t durable) const {
        const std::uint64_t retained = response_restarted ? 0U : resume_offset;
        const bool retained_resume = resumed && !response_restarted;
        if (progress) {
            progress(DownloadProgress{request.artifact_id, completed, total, attempt, retained_resume, false,
                                      DownloadProgressPhase::kDownloading, retained, redownload, request.source});
        }
        trace_transfer_progress(trace, request, completed, total, attempt, retained_resume, retained, durable, redownload);
    }
    [[nodiscard]] const std::string& effective_etag() const noexcept { return !response_headers_valid || http.etag.empty() ? resume_etag : http.etag; }
    [[nodiscard]] const std::string& effective_last_modified() const noexcept {
        return !response_headers_valid || http.last_modified.empty() ? resume_last_modified : http.last_modified;
    }
    void persist_partial_metadata() const {
        write_json_atomically(partial_metadata_path(request),
                              nlohmann::json{{"schema_version", kBenchmarkCacheSchemaVersion},
                                             {"url", request.url},
                                             {"etag", effective_etag()},
                                             {"last_modified", effective_last_modified()},
                                             {"bytes", write_offset}},
                              {});
        trace_benchmark_event(trace, "benchmark.download.partial_checkpoint", [&] {
            return nlohmann::json{{"artifact", request.artifact_id},
                                  {"bytes", write_offset},
                                  {"resume_eligible", !effective_etag().empty() || !effective_last_modified().empty()}};
        });
    }
};
struct PendingTransfer {
    std::size_t request_index = 0U;
    std::uint32_t attempt = 1U;
    Clock::time_point ready_at{};
    bool redownload = false;
};
class DownloadVerificationError final : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};
class SegmentedDownloadUnsupported final : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};
constexpr std::uint64_t kSegmentedDownloadThreshold = 512ULL * 1024U * 1024U;
constexpr std::uint64_t kMinimumSegmentBytes = 64ULL * 1024U * 1024U;
struct RemoteArtifactIdentity {
    std::string etag;
    std::string last_modified;
    [[nodiscard]] const std::string& if_range_value() const noexcept { return !etag.empty() ? etag : last_modified; }
};
struct IdentityProbe {
    const DownloadRequest& request;
    mmltk::common::concurrency::CancellationObservation cancel_requested = {};
    CurlEasy easy;
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    HttpResponseHeaderFields http;
    std::exception_ptr callback_error;
    IdentityProbe(const DownloadRequest& request_value, mmltk::common::concurrency::CancellationObservation cancel)
        : request(request_value), cancel_requested(cancel), easy(curl_easy_init()) {
        if (!easy) { throw std::runtime_error("cannot allocate segmented download identity probe"); }
        configure_curl_transfer(easy.get(),
                                CurlTransferSetup{
                                    .url = request.url.c_str(),
                                    .maximum_redirects = 10L,
                                    .error_buffer = error_buffer.data(),
                                    .owner = this,
                                    .write_callback = &IdentityProbe::write_callback,
                                    .header_callback = &IdentityProbe::header_callback,
                                    .progress_callback = &curl_cancel_progress_callback<IdentityProbe>,
                                },
                                "cannot configure segmented probe ");
        set_curl_option_with_prefix(easy.get(), CURLOPT_RANGE, "0-0", "cannot configure benchmark transfer ", "segmented probe range");
    }
    static std::size_t write_callback(char*, const std::size_t size, const std::size_t count, void* opaque) {
        const IdentityProbe& probe = *static_cast<IdentityProbe*>(opaque);
        std::size_t bytes = 0U;
        if (!curl_callback_byte_count(size, count, bytes)) { return 0U; }
        return probe.http.response_code == 206L && bytes <= 1U ? bytes : 0U;
    }
    static std::size_t header_callback(char* data, const std::size_t size, const std::size_t count, void* opaque) {
        return curl_run_header_callback<IdentityProbe>(data, size, count, opaque);
    }
    // The probe only needs the parsed response state; individual header kinds carry no policy.
    void on_header(HttpHeaderKind, std::string_view) const noexcept {}
};
[[nodiscard]] RemoteArtifactIdentity probe_remote_identity(const DownloadRequest& request, mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                           const BenchmarkTraceSink& trace) {
    for (std::uint32_t attempt = 1U; attempt <= request.maximum_attempts; ++attempt) {
        throw_if_benchmark_cancelled(cancel_requested);
        IdentityProbe probe(request, cancel_requested);
        const CURLcode result = curl_easy_perform(probe.easy.get());
        (void)curl_easy_getinfo(probe.easy.get(), CURLINFO_RESPONSE_CODE, &probe.http.response_code);
        if (probe.callback_error) { std::rethrow_exception(probe.callback_error); }
        const bool valid_range = result == CURLE_OK && probe.http.response_code == 206L && probe.http.range && !probe.http.range->unsatisfied &&
                                 probe.http.range->start == 0U && probe.http.range->end == 0U && probe.http.range->total == request.expected_size;
        const bool strong_etag = !probe.http.etag.empty() && !starts_with_case_insensitive(probe.http.etag, "W/");
        if (valid_range && (strong_etag || !probe.http.last_modified.empty())) {
            RemoteArtifactIdentity identity{
                strong_etag ? probe.http.etag : std::string{},
                probe.http.last_modified,
            };
            trace_benchmark_event(trace, "benchmark.download.segmented_probe", [&] {
                return nlohmann::json{{"artifact", request.artifact_id},
                                      {"attempt", attempt},
                                      {"bytes", request.expected_size},
                                      {"etag", identity.etag},
                                      {"last_modified", identity.last_modified}};
            });
            return identity;
        }
        if (probe.http.response_code == 200L || (valid_range && !strong_etag && probe.http.last_modified.empty())) {
            throw SegmentedDownloadUnsupported("server does not provide stable ranged downloads");
        }
        trace_benchmark_event(trace, "benchmark.download.segmented_probe_retry", [&] {
            return nlohmann::json{{"artifact", request.artifact_id},
                                  {"attempt", attempt},
                                  {"curl_code", static_cast<int>(result)},
                                  {"http_status", probe.http.response_code},
                                  {"detail", probe.error_buffer[0] != '\0' ? probe.error_buffer.data() : curl_easy_strerror(result)}};
        });
        if (attempt == request.maximum_attempts) { throw std::runtime_error("cannot establish a stable ranged download identity for " + request.artifact_id); }
        // The remote probe retry is intentionally deadline-based HTTP backoff, not local status polling.
        throw_if_benchmark_cancelled(cancel_requested);
        std::this_thread::sleep_for(std::chrono::milliseconds{std::min<std::uint64_t>(4000U, 250U << std::min<std::uint32_t>(attempt - 1U, 4U))});
        throw_if_benchmark_cancelled(cancel_requested);
    }
    throw std::runtime_error("segmented identity probe did not run");
}
struct DownloadSegment {
    std::uint64_t begin = 0U;
    std::uint64_t end = 0U;
    std::uint64_t completed = 0U;
    std::uint32_t attempts = 0U;
};
class SegmentedDownloadState {
   public:
    SegmentedDownloadState(const DownloadRequest& request, const RemoteArtifactIdentity& identity, const std::size_t segment_count,
                           const DownloadProgressSink& progress, const BenchmarkTraceSink& trace, const int descriptor,
                           const mmltk::common::concurrency::CancellationObservation cancellation)
        : request_(request), identity_(identity), progress_(progress), trace_(trace), descriptor_(descriptor), cancellation_(cancellation) {
        segments_.reserve(segment_count);
        const std::uint64_t segment_count_u64 = checked_cast<std::uint64_t>(segment_count, "segmented download count overflow");
        const std::uint64_t base_size = request.expected_size / segment_count_u64;
        const std::uint64_t remainder = request.expected_size % segment_count_u64;
        std::uint64_t next_begin = 0U;
        for (std::size_t index = 0U; index < segment_count; ++index) {
            const std::uint64_t begin = next_begin;
            const std::uint64_t index_u64 = checked_cast<std::uint64_t>(index, "segmented download index overflow");
            const std::uint64_t segment_size = base_size + (index_u64 < remainder ? 1U : 0U);
            const std::uint64_t next = checked_u64_add(begin, segment_size, "segmented download range overflow");
            segments_.push_back(DownloadSegment{begin, next - 1U});
            next_begin = next;
        }
        load_resume_state();
    }
    [[nodiscard]] DownloadSegment segment(const std::size_t index) {
        const std::lock_guard lock(mutex_);
        return segments_.at(index);
    }
    void report_in_flight(const std::size_t index, const std::uint64_t bytes) {
        if (!progress_ && !trace_) return;
        const std::lock_guard lock(mutex_);
        in_flight_.at(index) = bytes;
        const Clock::time_point now = Clock::now();
        if (last_progress_.time_since_epoch().count() != 0 && now - last_progress_ < std::chrono::milliseconds{100}) { return; }
        emit_progress_locked(false);
        last_progress_ = now;
    }
    void begin_attempt(const std::uint32_t attempt) {
        if (!progress_ && !trace_) { return; }
        const std::lock_guard lock(mutex_);
        active_attempt_ = std::max(active_attempt_, attempt);
        if (attempt > 1U) {
            retained_bytes_ = 0U;
            for (const auto& segment : segments_) {
                retained_bytes_ = checked_u64_add(retained_bytes_, segment.completed, "segmented retained byte overflow");
            }
        }
        emit_progress_locked(true);
    }
    void commit_attempt(const std::size_t index, const std::uint64_t attempt_begin, const std::uint64_t transferred, const std::uint32_t attempt) {
        const std::lock_guard lock(mutex_);
        DownloadSegment& segment = segments_.at(index);
        const std::uint64_t expected_begin = checked_u64_add(segment.begin, segment.completed, "segmented download attempt offset overflow");
        if (attempt_begin != expected_begin || transferred > segment.end + 1U - expected_begin) {
            throw std::runtime_error("segmented download attempt is outside its assigned range");
        }
        segment.completed = checked_u64_add(segment.completed, transferred, "segmented download completion overflow");
        segment.attempts = std::max(segment.attempts, attempt);
        in_flight_.at(index) = 0U;
        persist_locked();
        emit_progress_locked(segment.completed == segment.end + 1U - segment.begin);
    }
    void abandon_attempt(const std::size_t index, const std::uint32_t attempt) {
        const std::lock_guard lock(mutex_);
        DownloadSegment& segment = segments_.at(index);
        segment.attempts = std::max(segment.attempts, attempt);
        in_flight_.at(index) = 0U;
        persist_locked();
    }
    [[nodiscard]] bool resumed() const noexcept { return resumed_; }
    [[nodiscard]] std::uint64_t retained_bytes() const {
        const std::lock_guard lock(mutex_);
        return retained_bytes_;
    }
    [[nodiscard]] std::uint32_t maximum_attempts() const {
        const std::lock_guard lock(mutex_);
        std::uint32_t maximum = 0U;
        for (const DownloadSegment& segment : segments_) { maximum = std::max(maximum, segment.attempts); }
        return maximum;
    }

   private:
    [[nodiscard]] nlohmann::json metadata_locked() const {
        nlohmann::json segments = nlohmann::json::array();
        for (const DownloadSegment& segment : segments_) {
            segments.push_back({{"begin", segment.begin}, {"end", segment.end}, {"completed", segment.completed}, {"attempts", segment.attempts}});
        }
        return nlohmann::json{{"schema_version", kBenchmarkCacheSchemaVersion},
                              {"mode", "segmented"},
                              {"url", request_.url},
                              {"size", request_.expected_size},
                              {"etag", identity_.etag},
                              {"last_modified", identity_.last_modified},
                              {"segments", std::move(segments)}};
    }
    void load_resume_state() {
        bool valid = false;
        const std::filesystem::path metadata = partial_metadata_path(request_);
        if (std::filesystem::is_regular_file(metadata) && regular_file_size(partial_path(request_)) == request_.expected_size) {
            try {
                const nlohmann::json cached = read_json_file(metadata);
                const nlohmann::json& cached_segments = cached.at("segments");
                valid = cached.value("schema_version", 0U) == kBenchmarkCacheSchemaVersion && cached.value("mode", std::string{}) == "segmented" &&
                        cached.value("url", std::string{}) == request_.url && cached.value("size", 0ULL) == request_.expected_size &&
                        cached.value("etag", std::string{}) == identity_.etag && cached.value("last_modified", std::string{}) == identity_.last_modified &&
                        cached_segments.is_array() && cached_segments.size() == segments_.size();
                if (valid) {
                    for (std::size_t index = 0U; index < segments_.size(); ++index) {
                        DownloadSegment& segment = segments_[index];
                        const nlohmann::json& cached_segment = cached_segments[index];
                        const std::uint64_t completed = cached_segment.value("completed", 0ULL);
                        valid = cached_segment.value("begin", std::numeric_limits<std::uint64_t>::max()) == segment.begin &&
                                cached_segment.value("end", std::numeric_limits<std::uint64_t>::max()) == segment.end &&
                                completed <= segment.end + 1U - segment.begin;
                        if (!valid) { break; }
                        segment.completed = completed;
                        segment.attempts = 0U;
                    }
                }
            } catch (const std::exception&) { valid = false; }
        }
        if (!valid) {
            for (DownloadSegment& segment : segments_) {
                segment.completed = 0U;
                segment.attempts = 0U;
            }
            if (::ftruncate(descriptor_, 0) != 0 ||
                ::ftruncate(descriptor_, checked_cast<off_t>(request_.expected_size, "segmented download allocation overflow")) != 0) {
                throw errno_error("cannot allocate segmented benchmark download", partial_path(request_).string());
            }
        }
        resumed_ = std::ranges::any_of(segments_, [](const DownloadSegment& segment) { return segment.completed != 0U; });
        in_flight_.assign(segments_.size(), 0U);
        if (progress_ || trace_) {
            for (const auto& segment : segments_) { retained_bytes_ += segment.completed; }
        }
        persist_locked();
        emit_progress_locked(false);
        trace_benchmark_event(trace_, "benchmark.download.segmented_resume_state",
                              [&] { return nlohmann::json{{"artifact", request_.artifact_id}, {"segments", segments_.size()}, {"resumed", resumed_}}; });
    }
    void persist_locked() const { write_json_atomically(partial_metadata_path(request_), metadata_locked(), cancellation_); }
    void emit_progress_locked(const bool force) const {
        if (!progress_ && !trace_) { return; }
        std::uint64_t durable = 0U;
        std::uint64_t completed = 0U;
        std::uint32_t attempts = 0U;
        for (std::size_t index = 0U; index < segments_.size(); ++index) {
            const std::uint64_t segment_completed = segments_[index].completed + in_flight_[index];
            if (completed > request_.expected_size || segment_completed > request_.expected_size - completed) {
                throw std::runtime_error("segmented download progress exceeds artifact size");
            }
            completed += segment_completed;
            durable += segments_[index].completed;
            attempts = std::max({attempts, segments_[index].attempts, active_attempt_});
        }
        if (force || completed <= request_.expected_size) {
            if (progress_) {
                progress_(DownloadProgress{request_.artifact_id, completed, request_.expected_size, attempts, retained_bytes_ != 0U, false,
                                           DownloadProgressPhase::kDownloading, retained_bytes_, request_.redownload, request_.source});
            }
            trace_transfer_progress(trace_, request_, completed, request_.expected_size, attempts, retained_bytes_ != 0U, retained_bytes_, durable, request_.redownload);
        }
    }
    const DownloadRequest& request_;
    const RemoteArtifactIdentity& identity_;
    DownloadProgressSink progress_;
    BenchmarkTraceSink trace_;
    int descriptor_ = -1;
    mmltk::common::concurrency::CancellationObservation cancellation_;
    mutable std::mutex mutex_;
    std::vector<DownloadSegment> segments_;
    std::vector<std::uint64_t> in_flight_;
    std::uint32_t active_attempt_ = 0U;
    std::uint64_t retained_bytes_ = 0U;
    Clock::time_point last_progress_{};
    bool resumed_ = false;
};
struct SegmentTransfer {
    static constexpr DownloadWriteContext kWriteContext{
        .offset_overflow = "segmented download write offset overflow",
        .write_failure = "cannot write segmented benchmark download",
        .no_progress = "segmented benchmark write made no progress",
        .size_overflow = "segmented download size overflow",
    };
    const DownloadRequest& request;
    const RemoteArtifactIdentity& identity;
    SegmentedDownloadState& state;
    std::size_t segment_index = 0U;
    std::uint64_t range_begin = 0U;
    std::uint64_t range_end = 0U;
    std::uint64_t write_offset = 0U;
    std::uint32_t attempt = 0U;
    int descriptor = -1;
    mmltk::common::concurrency::CancellationObservation cancel_requested = {};
    CurlEasy easy;
    CurlHeaders headers;
    std::string range;
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    HttpResponseHeaderFields http;
    bool response_headers_valid = false;
    std::exception_ptr callback_error;
    SegmentTransfer(const DownloadRequest& request_value, const RemoteArtifactIdentity& identity_value, SegmentedDownloadState& state_value,
                    const std::size_t index, const std::uint64_t begin, const std::uint64_t end, const std::uint32_t attempt_value, const int descriptor_value,
                    mmltk::common::concurrency::CancellationObservation cancel)
        : request(request_value),
          identity(identity_value),
          state(state_value),
          segment_index(index),
          range_begin(begin),
          range_end(end),
          write_offset(begin),
          attempt(attempt_value),
          descriptor(descriptor_value),
          cancel_requested(cancel),
          easy(curl_easy_init()),
          range(std::to_string(begin) + "-" + std::to_string(end)) {
        if (!easy || begin > end || descriptor < 0) { throw std::runtime_error("segmented benchmark transfer is invalid"); }
        curl_slist* appended = curl_slist_append(nullptr, ("If-Range: " + identity.if_range_value()).c_str());
        if (appended == nullptr) { throw std::runtime_error("cannot allocate segmented If-Range header"); }
        headers.reset(appended);
        configure_curl_transfer(easy.get(),
                                CurlTransferSetup{
                                    .url = request.url.c_str(),
                                    .maximum_redirects = 10L,
                                    .error_buffer = error_buffer.data(),
                                    .owner = this,
                                    .write_callback = &SegmentTransfer::write_callback,
                                    .header_callback = &SegmentTransfer::header_callback,
                                    .progress_callback = &curl_cancel_progress_callback<SegmentTransfer>,
                                },
                                "cannot configure segmented benchmark transfer ");
        set_curl_option_with_prefix(easy.get(), CURLOPT_RANGE, range.c_str(), "cannot configure benchmark transfer ", "segmented transfer range");
        set_curl_option_with_prefix(easy.get(), CURLOPT_HTTPHEADER, headers.get(), "cannot configure benchmark transfer ", "segmented transfer headers");
    }
    [[nodiscard]] std::uint64_t transferred() const noexcept { return write_offset - range_begin; }
    [[nodiscard]] bool response_identity_matches() const noexcept {
        if (!identity.etag.empty()) { return http.etag == identity.etag; }
        return http.last_modified == identity.last_modified;
    }
    static std::size_t write_callback(char* data, const std::size_t size, const std::size_t count, void* opaque) {
        return curl_run_data_callback<SegmentTransfer>(size, count, opaque, [data](SegmentTransfer& transfer, const std::size_t bytes) -> std::size_t {
            if (!transfer.response_headers_valid) { return 0U; }
            if (bytes > transfer.range_end + 1U - transfer.write_offset) { throw std::runtime_error("segmented response exceeds its validated byte range"); }
            append_transfer_bytes(&transfer.write_offset, SegmentTransfer::kWriteContext, transfer.descriptor, data, bytes);
            transfer.state.report_in_flight(transfer.segment_index, transfer.transferred());
            return bytes;
        });
    }
    static std::size_t header_callback(char* data, const std::size_t size, const std::size_t count, void* opaque) {
        return curl_run_header_callback<SegmentTransfer>(data, size, count, opaque);
    }
    void on_header(const HttpHeaderKind kind, const std::string_view header) {
        if (kind == HttpHeaderKind::kStatusLine) {
            response_headers_valid = false;
        } else if (kind == HttpHeaderKind::kOther && is_header_block_terminator(header)) {
            response_headers_valid = http.response_code == 206L && http.range && !http.range->unsatisfied && http.range->start == range_begin &&
                                     http.range->end == range_end && http.range->total == request.expected_size && response_identity_matches();
        }
    }
};
[[nodiscard]] DownloadResult download_segmented_artifact(const DownloadRequest& request, const std::size_t maximum_concurrency,
                                                         mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                         const DownloadProgressSink& progress, const BenchmarkTraceSink& trace) {
    CurlGlobal::initialize();
    const RemoteArtifactIdentity identity = probe_remote_identity(request, cancel_requested, trace);
    const std::uint64_t segments_for_size =
        request.expected_size / kMinimumSegmentBytes + static_cast<std::uint64_t>(request.expected_size % kMinimumSegmentBytes != 0U);
    const std::size_t segment_count = checked_cast<std::size_t>(
        std::max<std::uint64_t>(1U, std::min(checked_cast<std::uint64_t>(maximum_concurrency, "segmented concurrency overflow"), segments_for_size)),
        "segmented download count overflow");
    (void)mmltk::common::io::ensure_parent_directory(request.destination);
    const int descriptor = ::open(partial_path(request).c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (descriptor < 0) { throw errno_error("cannot open segmented benchmark download", partial_path(request).string()); }
    ScopedFd partial(descriptor);
    SegmentedDownloadState state(request, identity, segment_count, progress, trace, descriptor, cancel_requested);
    trace_benchmark_event(trace, "benchmark.download.segmented_start", [&] {
        return nlohmann::json{{"artifact", request.artifact_id}, {"segments", segment_count}, {"bytes", request.expected_size}, {"resumed", state.resumed()}};
    });
    const auto download_segment = [&](const std::size_t segment_index) {
        while (true) {
            throw_if_benchmark_cancelled(cancel_requested);
            const DownloadSegment segment = state.segment(segment_index);
            const std::uint64_t segment_size = segment.end + 1U - segment.begin;
            if (segment.completed == segment_size) { break; }
            const std::uint32_t attempt = segment.attempts + 1U;
            if (attempt > request.maximum_attempts) { throw std::runtime_error("segmented benchmark download exhausted retries for " + request.artifact_id); }
            const std::uint64_t attempt_begin = segment.begin + segment.completed;
            state.begin_attempt(attempt);
            SegmentTransfer transfer(request, identity, state, segment_index, attempt_begin, segment.end, attempt, descriptor, cancel_requested);
            const CURLcode result = curl_easy_perform(transfer.easy.get());
            (void)curl_easy_getinfo(transfer.easy.get(), CURLINFO_RESPONSE_CODE, &transfer.http.response_code);
            if (transfer.callback_error) {
                try {
                    std::rethrow_exception(transfer.callback_error);
                } catch (const std::runtime_error&) {
                    if (!transfer.response_headers_valid) {
                        state.abandon_attempt(segment_index, attempt);
                    } else {
                        state.commit_attempt(segment_index, attempt_begin, transfer.transferred(), attempt);
                    }
                    throw;
                }
            }
            if (transfer.http.response_code == 200L) {
                state.abandon_attempt(segment_index, attempt);
                throw SegmentedDownloadUnsupported("server ignored a segmented byte range");
            }
            const bool complete = result == CURLE_OK && transfer.response_headers_valid && transfer.write_offset == segment.end + 1U;
            if (transfer.response_headers_valid) {
                state.commit_attempt(segment_index, attempt_begin, transfer.transferred(), attempt);
            } else {
                state.abandon_attempt(segment_index, attempt);
            }
            if (complete) { break; }
            trace_benchmark_event(trace, "benchmark.download.segment_retry", [&] {
                return nlohmann::json{{"artifact", request.artifact_id},
                                      {"segment", segment_index},
                                      {"attempt", attempt},
                                      {"curl_code", static_cast<int>(result)},
                                      {"http_status", transfer.http.response_code},
                                      {"completed_bytes", transfer.transferred()},
                                      {"detail", transfer.error_buffer[0] != '\0' ? transfer.error_buffer.data() : curl_easy_strerror(result)}};
            });
            if (attempt == request.maximum_attempts) {
                throw std::runtime_error("segmented benchmark download failed after retries for " + request.artifact_id);
            }
            // The remote segment retry is intentionally deadline-based HTTP backoff.
            throw_if_benchmark_cancelled(cancel_requested);
            std::this_thread::sleep_for(std::chrono::milliseconds{std::min<std::uint64_t>(4000U, 250U << std::min<std::uint32_t>(attempt - 1U, 4U))});
            throw_if_benchmark_cancelled(cancel_requested);
        }
    };
    std::exception_ptr transfer_error;
    std::mutex transfer_error_mutex;
    std::vector<std::thread> transfer_threads;
    transfer_threads.reserve(segment_count);
    for (std::size_t segment_index = 0U; segment_index < segment_count; ++segment_index) {
        transfer_threads.emplace_back([&, segment_index] {
            try {
                download_segment(segment_index);
            } catch (...) {
                const std::lock_guard lock(transfer_error_mutex);
                if (!transfer_error) { transfer_error = std::current_exception(); }
            }
        });
    }
    for (std::thread& thread : transfer_threads) { thread.join(); }
    if (transfer_error) { std::rethrow_exception(transfer_error); }
    if (::fdatasync(descriptor) != 0) { throw errno_error("cannot flush segmented benchmark download", partial_path(request).string()); }
    partial = ScopedFd{};
    throw_if_benchmark_cancelled(cancel_requested);
    const std::string identity_digest = artifact_identity(request, request.expected_size, identity.etag, identity.last_modified);
    throw_if_benchmark_cancelled(cancel_requested);
    std::filesystem::rename(partial_path(request), request.destination);
    sync_parent_directory(request.destination);
    const std::uint32_t attempts = state.maximum_attempts();
    write_json_atomically(complete_metadata_path(request),
                          nlohmann::json{{"schema_version", kBenchmarkCacheSchemaVersion},
                                         {"complete", true},
                                         {"url", request.url},
                                         {"size", request.expected_size},
                                         {"identity", identity_digest},
                                         {"integrity_mode", "http_identity_size"},
                                         {"etag", identity.etag},
                                         {"last_modified", identity.last_modified},
                                         {"attempts", attempts},
                                         {"segments", segment_count}},
                          cancel_requested);
    std::error_code remove_error;
    std::filesystem::remove(partial_metadata_path(request), remove_error);
    if (remove_error) { throw std::filesystem::filesystem_error("cannot remove segmented download metadata", partial_metadata_path(request), remove_error); }
    trace_benchmark_event(trace, "benchmark.download.segmented_complete", [&] {
        return nlohmann::json{{"artifact", request.artifact_id}, {"bytes", request.expected_size}, {"identity", identity_digest},
                              {"segments", segment_count}, {"attempts", attempts}, {"resumed", state.retained_bytes() != 0U},
                              {"retained_bytes", state.retained_bytes()}, {"redownload", request.redownload}};
    });
    return DownloadResult{
        request.destination, request.expected_size, identity_digest, {}, identity.etag, identity.last_modified, attempts, state.resumed(), false,
    };
}
[[nodiscard]] DownloadResult publish_completed_transfer(Transfer& transfer) {
    const DownloadRequest& request = transfer.request;
    if (::fdatasync(transfer.partial.get()) != 0) { throw errno_error("cannot flush benchmark partial download", partial_path(request).string()); }
    transfer.partial = ScopedFd{};
    const std::uint64_t size = regular_file_size(partial_path(request));
    if (size == 0U || (request.expected_size != 0U && size != request.expected_size)) {
        throw DownloadVerificationError("benchmark download size does not match its catalog");
    }
    throw_if_benchmark_cancelled(transfer.cancel_requested);
    const std::string identity = artifact_identity(request, size, transfer.effective_etag(), transfer.effective_last_modified());
    throw_if_benchmark_cancelled(transfer.cancel_requested);
    std::filesystem::rename(partial_path(request), request.destination);
    sync_parent_directory(request.destination);
    write_json_atomically(complete_metadata_path(request),
                          nlohmann::json{{"schema_version", kBenchmarkCacheSchemaVersion},
                                         {"complete", true},
                                         {"url", request.url},
                                         {"size", size},
                                         {"identity", identity},
                                         {"integrity_mode", "http_identity_size"},
                                         {"etag", transfer.effective_etag()},
                                         {"last_modified", transfer.effective_last_modified()},
                                         {"attempts", transfer.attempt}},
                          transfer.cancel_requested);
    std::error_code ignored;
    std::filesystem::remove(partial_metadata_path(request), ignored);
    transfer.emit_progress(size, size, size);
    trace_benchmark_event(transfer.trace, "benchmark.download.complete", [&] {
        return nlohmann::json{
            {"artifact", request.artifact_id}, {"bytes", size}, {"attempt", transfer.attempt}, {"resumed", transfer.resumed && !transfer.response_restarted},
            {"redownload", transfer.redownload}};
    });
    return DownloadResult{
        request.destination,
        size,
        identity,
        {},
        transfer.effective_etag(),
        transfer.effective_last_modified(),
        transfer.attempt,
        transfer.resumed && !transfer.response_restarted,
        false,
    };
}
}  // namespace
std::vector<DownloadResult> download_artifacts(const std::vector<DownloadRequest>& requests, const std::size_t maximum_concurrency,
                                               mmltk::common::concurrency::CancellationObservation cancel_requested, const DownloadProgressSink& progress,
                                               const BenchmarkTraceSink& trace) {
    if (requests.empty()) { return {}; }
    if (maximum_concurrency == 0U) { throw std::runtime_error("benchmark download concurrency must be positive"); }
    for (const DownloadRequest& request : requests) {
        if (request.artifact_id.empty() || request.url.empty() || request.destination.empty() || request.lock_path.empty() || request.maximum_attempts == 0U) {
            throw std::runtime_error("benchmark download request is incomplete");
        }
        if (request.expected_sha256) { (void)mmltk::common::io::parse_sha256_hex(*request.expected_sha256); }
    }
    std::unordered_set<std::string> unique_lock_paths;
    std::vector<std::size_t> lock_order(requests.size());
    for (std::size_t index = 0U; index < lock_order.size(); ++index) {
        if (!unique_lock_paths.emplace(requests[index].lock_path.string()).second) {
            throw std::runtime_error("benchmark download batch contains a duplicate cache lock");
        }
        lock_order[index] = index;
    }
    std::ranges::sort(lock_order,
                      [&](const std::size_t left, const std::size_t right) { return requests[left].lock_path.string() < requests[right].lock_path.string(); });
    std::vector<ArtifactLease> leases;
    leases.reserve(requests.size());
    for (const std::size_t index : lock_order) { leases.push_back(ArtifactLease::acquire(requests[index].lock_path, cancel_requested)); }
    std::vector<std::optional<DownloadResult>> results(requests.size());
    std::list<PendingTransfer> pending;
    for (std::size_t index = 0U; index < requests.size(); ++index) {
        throw_if_benchmark_cancelled(cancel_requested);
        std::optional<DownloadResult>& result = results[index];
        result = validate_complete_artifact(requests[index], cancel_requested, progress, trace);
        if (result.has_value()) {
            const DownloadResult& completed_result = *result;
            if (progress) {
                progress(DownloadProgress{
                    requests[index].artifact_id,
                    completed_result.size,
                    completed_result.size,
                    completed_result.attempts,
                    false,
                    true,
                    DownloadProgressPhase::kDownloading,
                    0U,
                    requests[index].redownload,
                    requests[index].source,
                });
            }
        } else {
            remove_invalid_complete_artifact(requests[index]);
            pending.push_back(PendingTransfer{index, 1U, Clock::now()});
        }
    }
    if (pending.empty()) {
        std::vector<DownloadResult> complete;
        complete.reserve(results.size());
        for (std::optional<DownloadResult>& result : results) {
            if (!result.has_value()) { throw std::logic_error("completed benchmark download batch is missing a result"); }
            complete.push_back(std::move(*result));
        }
        return complete;
    }
    if (requests.size() == 1U && maximum_concurrency > 1U && requests.front().expected_size >= kSegmentedDownloadThreshold) {
        try {
            return {download_segmented_artifact(requests.front(), maximum_concurrency, cancel_requested, progress, trace)};
        } catch (const SegmentedDownloadUnsupported& error) {
            std::error_code cleanup_error;
            pending.front().redownload = std::filesystem::remove(partial_path(requests.front()), cleanup_error);
            if (cleanup_error) {
                throw std::filesystem::filesystem_error("cannot reset unsupported segmented download", partial_path(requests.front()), cleanup_error);
            }
            cleanup_error.clear();
            const bool removed_metadata = std::filesystem::remove(partial_metadata_path(requests.front()), cleanup_error);
            pending.front().redownload = pending.front().redownload || removed_metadata;
            if (cleanup_error) {
                throw std::filesystem::filesystem_error("cannot reset unsupported segmented metadata", partial_metadata_path(requests.front()), cleanup_error);
            }
            trace_benchmark_event(trace, "benchmark.download.segmented_fallback",
                                  [&] { return nlohmann::json{{"artifact", requests.front().artifact_id}, {"reason", error.what()},
                                                        {"redownload", requests.front().redownload || pending.front().redownload}}; });
        }
    }
    CurlGlobal::initialize();
    CurlMulti multi(curl_multi_init());
    if (!multi) { throw std::runtime_error("cannot allocate benchmark libcurl multi handle"); }
    const CURLMcode connection_limit =
        curl_multi_setopt(multi.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS, checked_cast<long>(maximum_concurrency, "benchmark download concurrency overflow"));
    if (connection_limit != CURLM_OK) {
        throw std::runtime_error(std::string("cannot set benchmark transfer concurrency: ") + curl_multi_strerror(connection_limit));
    }
    CurlMultiTransfers<Transfer> active(std::move(multi), "benchmark transfer");
    active.reserve(maximum_concurrency);
    const auto schedule_retry = [&](Transfer& transfer, const std::size_t request_index, const bool reset_partial, const CURLcode curl_code,
                                    const std::string& detail) {
        if (reset_partial) {
            transfer.partial = ScopedFd{};
            std::error_code error;
            std::filesystem::remove(partial_path(transfer.request), error);
            if (error) { throw std::filesystem::filesystem_error("cannot reset invalid benchmark partial download", partial_path(transfer.request), error); }
            error.clear();
            std::filesystem::remove(partial_metadata_path(transfer.request), error);
            if (error) { throw std::filesystem::filesystem_error("cannot reset benchmark partial metadata", partial_metadata_path(transfer.request), error); }
        } else {
            transfer.persist_partial_metadata();
        }
        trace_benchmark_event(trace, "benchmark.download.attempt_failed", [&] {
            return nlohmann::json{{"artifact", transfer.request.artifact_id},   {"attempt", transfer.attempt},    {"curl_code", static_cast<int>(curl_code)},
                                  {"http_status", transfer.http.response_code}, {"reset_partial", reset_partial}, {"detail", detail}};
        });
        if (transfer.attempt >= transfer.request.maximum_attempts) {
            throw std::runtime_error("benchmark download failed after retries for " + transfer.request.artifact_id + ": " + detail);
        }
        const auto backoff = std::chrono::milliseconds{std::min<std::uint64_t>(4000U, 250U << std::min<std::uint32_t>(transfer.attempt - 1U, 4U))};
        pending.push_back(PendingTransfer{request_index, transfer.attempt + 1U, Clock::now() + backoff, transfer.redownload || reset_partial});
    };
    try {
        while (!pending.empty() || !active.empty()) {
            throw_if_benchmark_cancelled(cancel_requested);
            const Clock::time_point now = Clock::now();
            for (auto iterator = pending.begin(); iterator != pending.end() && active.size() < maximum_concurrency;) {
                if (iterator->ready_at > now) {
                    ++iterator;
                    continue;
                }
                const PendingTransfer task = *iterator;
                iterator = pending.erase(iterator);
                active.add(std::make_unique<Transfer>(requests[task.request_index], progress, trace, cancel_requested, task.attempt, task.redownload));
                trace_benchmark_event(trace, "benchmark.download.start",
                                      [&] { return nlohmann::json{{"artifact", requests[task.request_index].artifact_id}, {"attempt", task.attempt}}; });
            }
            active.perform();
            while (std::optional completion = active.next_completed()) {
                std::unique_ptr<Transfer> transfer = std::move(completion->transfer);
                (void)curl_easy_getinfo(completion->handle, CURLINFO_RESPONSE_CODE, &transfer->http.response_code);
                const std::size_t request_index = checked_cast<std::size_t>(&transfer->request - requests.data(), "download request index overflow");
                if (transfer->callback_error) {
                    if (!transfer->callback_retryable) { std::rethrow_exception(transfer->callback_error); }
                    std::string detail = "invalid HTTP range response";
                    try {
                        std::rethrow_exception(transfer->callback_error);
                    } catch (const std::exception& error) { detail = error.what(); } catch (...) {
                        detail = "non-standard HTTP range callback exception";
                    }
                    schedule_retry(*transfer, request_index, true, completion->result, detail);
                    continue;
                }
                const bool successful_status =
                    transfer->response_headers_valid && (transfer->http.response_code == 200L || transfer->http.response_code == 206L ||
                                                         (transfer->http.response_code == 416L && transfer->response_total != 0U &&
                                                          regular_file_size(partial_path(transfer->request)) == transfer->response_total));
                const bool complete_range = transfer->http.response_code != 206L ||
                                            (transfer->response_total != 0U && regular_file_size(partial_path(transfer->request)) == transfer->response_total);
                if (completion->result == CURLE_OK && successful_status && complete_range) {
                    try {
                        results[request_index] = publish_completed_transfer(*transfer);
                    } catch (const DownloadVerificationError& error) { schedule_retry(*transfer, request_index, true, completion->result, error.what()); }
                    continue;
                }
                const std::string detail = completion->result == CURLE_OK && successful_status && !complete_range
                                               ? "resumed transfer did not reach the declared Content-Range total"
                                           : completion->result == CURLE_OK    ? "HTTP response status or headers were not acceptable"
                                           : transfer->error_buffer[0] != '\0' ? transfer->error_buffer.data()
                                                                               : curl_easy_strerror(completion->result);
                schedule_retry(*transfer, request_index, false, completion->result, detail);
            }
            if (!active.empty()) {
                active.poll(kBenchmarkTransferPollMilliseconds);
            } else if (!pending.empty()) {
                const Clock::time_point earliest = std::ranges::min_element(pending, {}, &PendingTransfer::ready_at)->ready_at;
                const auto delay = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(std::max(earliest - Clock::now(), Clock::duration::zero())),
                                            std::chrono::milliseconds{250});
                if (delay.count() > 0) {
                    // Pending transfers become eligible only at their externally imposed retry deadlines.
                    throw_if_benchmark_cancelled(cancel_requested);
                    std::this_thread::sleep_for(delay);
                    throw_if_benchmark_cancelled(cancel_requested);
                }
            }
        }
    } catch (...) {
        const std::exception_ptr original_error = std::current_exception();
        std::exception_ptr cleanup_error;
        active.abandon_all([&cleanup_error](Transfer& transfer) {
            try {
                transfer.persist_partial_metadata();
            } catch (...) {
                transfer.partial = ScopedFd{};
                try {
                    std::error_code error;
                    std::filesystem::remove(partial_path(transfer.request), error);
                    if (error) {
                        throw std::filesystem::filesystem_error("cannot invalidate interrupted benchmark partial download", partial_path(transfer.request),
                                                                error);
                    }
                    error.clear();
                    std::filesystem::remove(partial_metadata_path(transfer.request), error);
                    if (error) {
                        throw std::filesystem::filesystem_error("cannot invalidate interrupted benchmark partial metadata",
                                                                partial_metadata_path(transfer.request), error);
                    }
                } catch (...) {
                    if (!cleanup_error) { cleanup_error = std::current_exception(); }
                }
            }
        });
        if (cleanup_error) { std::rethrow_exception(cleanup_error); }
        std::rethrow_exception(original_error);
    }
    std::vector<DownloadResult> complete;
    complete.reserve(results.size());
    for (std::optional<DownloadResult>& result : results) {
        if (!result) { throw std::runtime_error("benchmark download loop ended with an incomplete artifact"); }
        complete.push_back(std::move(*result));
    }
    return complete;
}
void invalidate_download_artifact(const DownloadRequest& request, mmltk::common::concurrency::CancellationObservation cancel_requested,
                                  const BenchmarkTraceSink& trace) {
    if (request.artifact_id.empty() || request.destination.empty() || request.lock_path.empty()) {
        throw std::runtime_error("benchmark download invalidation request is incomplete");
    }
    ArtifactLease lease = ArtifactLease::acquire(request.lock_path, cancel_requested);
    const std::array paths{
        request.destination,
        complete_metadata_path(request),
        partial_path(request),
        partial_metadata_path(request),
    };
    std::uint32_t removed = 0U;
    for (const std::filesystem::path& path : paths) {
        throw_if_benchmark_cancelled(cancel_requested);
        std::error_code error;
        if (std::filesystem::remove(path, error)) { ++removed; }
        if (error) { throw std::filesystem::filesystem_error("cannot invalidate benchmark download artifact", path, error); }
    }
    if (removed != 0U) { sync_parent_directory(request.destination); }
    trace_benchmark_event(trace, "benchmark.download.invalidated",
                          [&] { return nlohmann::json{{"artifact", request.artifact_id}, {"removed_paths", removed}}; });
}
}  // namespace mmltk::backend::data::benchmark_internal
