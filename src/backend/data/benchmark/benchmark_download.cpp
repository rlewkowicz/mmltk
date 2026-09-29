#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include <curl/curl.h>
#include "src/pch_linux.h"
#include "src/pch_std.h"
#include <list>
#include <queue>
#include <numeric>
#include <condition_variable>
#include <thread>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <unordered_set>
#include "src/backend/data/benchmark/benchmark_hash.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/common/types/string_utils.h"
// CLEANUP-IGNORE: This download unit names its boundary-specific imports and private headers.
#include "src/backend/data/benchmark/detail/benchmark_curl.h"
#include "src/backend/data/benchmark/detail/benchmark_download.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
namespace mmltk::backend::data::benchmark_internal {
const std::string& download_failure_sha256(DownloadResult& artifact, mmltk::common::concurrency::CancellationObservation cancellation) {
 if (!artifact.failure_digest) {
  auto& memo = artifact.failure_digest.emplace();
  try {
   memo.sha256 = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(artifact.path, [&] { return cancellation.requested(); }));
  } catch (...) { memo.error = std::current_exception(); }
 }
 const auto& memo = *artifact.failure_digest;
 if (memo.error) std::rethrow_exception(memo.error);
 return memo.sha256;
}
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
using mmltk::common::math::checked_multiply;
using mmltk::common::types::trim_http_field_value;
namespace {
using Clock = std::chrono::steady_clock;
void trace_transfer_progress(const BenchmarkTraceSink& trace, const DownloadRequest& request, const std::uint64_t completed, const std::uint64_t total, const std::uint32_t attempt, const bool resumed,
 const std::uint64_t retained, const std::uint64_t durable, const bool redownload) {
 trace_benchmark_event(trace, "benchmark.download.progress", [&] {
  return nlohmann::json{
   {"artifact", request.artifact_id}, {"completed_bytes", completed}, {"total_bytes", total}, {"attempt", attempt}, {"resumed", resumed}, {"cache_hit", false}, {"retained_bytes", retained},
   {"durable_bytes", durable}, {"redownload", redownload}
  };
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
[[nodiscard]] std::string artifact_identity(const DownloadRequest& request, const std::uint64_t size, const std::string_view etag, const std::string_view last_modified) {
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
[[nodiscard]] std::uint64_t checked_u64_add(const std::uint64_t left, const std::uint64_t right, const char* context) { return checked_add(left, right, context); }
// Writes a whole response chunk at an absolute file offset, absorbing EINTR and short writes. Every download path
// lands its bytes this way, so the retry loop lives here instead of inside each libcurl write callback.
void write_all_at(
 const int descriptor, const std::uint8_t* source, const std::size_t bytes, const std::uint64_t offset, const char* offset_context, const char* write_context, const char* progress_context) {
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
 write_all_at(descriptor, reinterpret_cast<const std::uint8_t*>(data), bytes, *write_offset, context.offset_overflow, context.write_failure, context.no_progress);
 *write_offset = checked_u64_add(*write_offset, bytes, context.size_overflow);
}
[[nodiscard]] std::optional<DownloadResult> validate_complete_artifact(
 const DownloadRequest& request, mmltk::common::concurrency::CancellationObservation cancel_requested, const DownloadProgressSink& progress, const BenchmarkTraceSink& trace) {
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
   if (metadata.value("schema_version", 0U) != kBenchmarkCacheSchemaVersion || metadata.value("complete", false) == false || metadata.value("url", std::string{}) != request.url ||
       metadata.value("size", 0ULL) != size) {
    return std::nullopt;
   }
   metadata_identity = metadata.value("identity", std::string{});
   etag = metadata.value("etag", std::string{});
   last_modified = metadata.value("last_modified", std::string{});
   attempts = metadata.value("attempts", 0U);
  } catch (const std::exception& error) {
   if (is_benchmark_capacity_failure(error)) throw;
   return std::nullopt;
  }
 }
 throw_if_benchmark_cancelled(cancel_requested);
 const std::string identity = artifact_identity(request, size, etag, last_modified);
 if (has_completion_metadata && (metadata_identity.empty() || metadata_identity != identity)) { return std::nullopt; }
 trace_benchmark_event(trace, has_completion_metadata ? "benchmark.download.cache_hit" : "benchmark.download.preseeded",
  [&] { return nlohmann::json{{"artifact", request.artifact_id}, {"bytes", size}, {"integrity", has_completion_metadata ? "atomic_http_identity" : "pending_structural_validation"}}; });
 return DownloadResult{
  request.destination,
  size,
  identity,
  {},
  etag,
  last_modified,
  attempts,
  false,
  true,
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
 if (start.ec != std::errc{} || start.ptr != start_text.data() + start_text.size() || end.ec != std::errc{} || end.ptr != end_text.data() + end_text.size() || parsed.end < parsed.start ||
     parsed.end >= parsed.total) {
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
// Resumable partials keep their checkpoint lifetime on unwind. This owner only
// settles their growth promise; discard policy remains in the transfer engine.
struct DownloadStorage {
 StorageReservationPool filesystem;
 StorageReservationPool::Reservation growth;
 DownloadStorage(const DownloadRequest& request, StorageReservationPool& shared, const BenchmarkTraceSink& trace)
     : filesystem(request.destination, trace, &shared),
       growth(filesystem.reserve_download(request.destination, std::max(request.expected_size, request.storage_estimate), "benchmark download and replacement")) {}
};
struct Transfer {
 DownloadStorage& storage;
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
 Transfer(const DownloadRequest& request_value, DownloadProgressSink progress_value, BenchmarkTraceSink trace_value, mmltk::common::concurrency::CancellationObservation cancel,
  const std::uint32_t attempt_value, const bool redownload_value, DownloadStorage& destination, std::optional<nlohmann::json> observed_metadata)
     : storage(destination),
       request(request_value),
       progress(std::move(progress_value)),
       trace(std::move(trace_value)),
       cancel_requested(cancel),
       attempt(attempt_value),
       redownload(request_value.redownload || redownload_value) {
  prepare_partial(std::move(observed_metadata));
  prepare_easy();
 }
 void prepare_partial(std::optional<nlohmann::json> observed_metadata) {
  (void)mmltk::common::io::ensure_parent_directory(request.destination);
  const std::filesystem::path part_path = partial_path(request);
  bool metadata_matches = false;
  const std::filesystem::path metadata_path = partial_metadata_path(request);
  if (observed_metadata || std::filesystem::exists(metadata_path)) {
   try {
    const nlohmann::json metadata = observed_metadata ? std::move(*observed_metadata) : read_json_file(metadata_path);
    metadata_matches =
     metadata.value("schema_version", 0U) == kBenchmarkCacheSchemaVersion && metadata.value("url", std::string{}) == request.url && metadata.value("mode", std::string{}) != "segmented";
    resume_etag = metadata.value("etag", std::string{});
    resume_last_modified = metadata.value("last_modified", std::string{});
   } catch (const std::exception& error) {
    if (is_benchmark_capacity_failure(error)) throw;
    metadata_matches = false;
   }
  }
  if (!metadata_matches) {
   storage.growth.withdraw_allocation();
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
   storage.growth.withdraw_allocation();
   if (::ftruncate(descriptor, 0) != 0) { throw errno_error("cannot reset oversized benchmark partial download", part_path.string()); }
   resume_offset = 0U;
  }
  if (resume_offset != 0U && resume_etag.empty() && resume_last_modified.empty()) {
   storage.growth.withdraw_allocation();
   if (::ftruncate(descriptor, 0) != 0) { throw errno_error("cannot reset unvalidated benchmark partial download", part_path.string()); }
   resume_offset = 0U;
  }
  storage.growth.reconcile(descriptor);
  requested_resume_offset = resume_offset;
  write_offset = resume_offset;
  resumed = resume_offset != 0U;
  write_json_atomically(metadata_path,
   nlohmann::json{{"schema_version", kBenchmarkCacheSchemaVersion}, {"url", request.url}, {"etag", resume_etag}, {"last_modified", resume_last_modified}, {"bytes", resume_offset}}, cancel_requested,
   &storage.filesystem);
 }
 void prepare_easy() {
  easy.reset(curl_easy_init());
  if (!easy) { throw std::runtime_error("cannot allocate benchmark libcurl handle"); }
  // CLEANUP-IGNORE: Independent CURL transfer modes initialize the common transfer-setup record; their callbacks and ownership differ.
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
  set_curl_option_with_prefix(easy.get(), CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L, "cannot configure benchmark transfer ", "proxy CONNECT header suppression");
  if (resume_offset != 0U) {
   // Our header admission owns both a validated 206 resume and a 200
   // restart. libcurl's resume option rejects the latter before delivery.
   const std::string range = std::to_string(resume_offset) + "-";
   set_curl_option_with_prefix(easy.get(), CURLOPT_RANGE, range.c_str(), "cannot configure benchmark transfer ", "resume range");
   const std::string validator = !resume_etag.empty() ? "If-Range: " + resume_etag : !resume_last_modified.empty() ? "If-Range: " + resume_last_modified : std::string{};
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
   transfer.storage.growth.grow(checked_u64_add(transfer.write_offset, bytes_u64, "download storage extent overflow"), "additional benchmark download bytes");
   append_transfer_bytes(&transfer.write_offset, Transfer::kWriteContext, transfer.partial.get(), data, bytes);
   transfer.storage.growth.reconcile(transfer.partial.get());
   return bytes;
  });
 }
 static std::size_t header_callback(char* data, const std::size_t size, const std::size_t count, void* opaque) { return curl_run_header_callback<Transfer>(data, size, count, opaque); }
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
   valid = requested_resume_offset != 0U && http.range && !http.range->unsatisfied && http.range->start == requested_resume_offset && http.range->end == http.range->total - 1U;
  } else if (http.response_code == 416L) {
   valid = requested_resume_offset != 0U && http.range && http.range->unsatisfied && http.range->total == requested_resume_offset;
  } else if (http.response_code == 200L) {
   valid = true;
  }
  if (valid && request.expected_size != 0U && (http.response_code == 206L || http.response_code == 416L)) { valid = response_total == request.expected_size; }
  if (!valid) {
   callback_retryable = true;
   throw std::runtime_error("benchmark server returned an invalid HTTP byte range");
  }
  if (http.response_code == 200L && requested_resume_offset != 0U) {
   storage.growth.withdraw_allocation();
   if (::ftruncate(partial.get(), 0) != 0) { throw errno_error("cannot restart benchmark partial download"); }
   storage.growth.reconcile(partial.get());
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
     if (total == 0U && download_total > 0) { total = checked_u64_add(base, checked_cast<std::uint64_t>(download_total, "download total overflow"), "download total overflow"); }
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
  if (progress) { progress(DownloadProgress{request.artifact_id, {completed, total, retained, attempt, false, retained_resume}, DownloadProgressPhase::kDownloading, redownload, request.source}); }
  trace_transfer_progress(trace, request, completed, total, attempt, retained_resume, retained, durable, redownload);
 }
 [[nodiscard]] const std::string& effective_etag() const noexcept { return !response_headers_valid || http.etag.empty() ? resume_etag : http.etag; }
 [[nodiscard]] const std::string& effective_last_modified() const noexcept { return !response_headers_valid || http.last_modified.empty() ? resume_last_modified : http.last_modified; }
 void persist_partial_metadata() const {
  storage.growth.reconcile(partial.get());
  write_json_atomically(partial_metadata_path(request),
   nlohmann::json{{"schema_version", kBenchmarkCacheSchemaVersion}, {"url", request.url}, {"etag", effective_etag()}, {"last_modified", effective_last_modified()}, {"bytes", write_offset}}, {},
   &storage.filesystem);
  trace_benchmark_event(trace, "benchmark.download.partial_checkpoint",
   [&] { return nlohmann::json{{"artifact", request.artifact_id}, {"bytes", write_offset}, {"resume_eligible", !effective_etag().empty() || !effective_last_modified().empty()}}; });
 }
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
 IdentityProbe(const DownloadRequest& request_value, mmltk::common::concurrency::CancellationObservation cancel) : request(request_value), cancel_requested(cancel), easy(curl_easy_init()) {
  if (!easy) { throw std::runtime_error("cannot allocate segmented download identity probe"); }
  // CLEANUP-IGNORE: Independent CURL transfer modes initialize the common transfer-setup record; their callbacks and ownership differ.
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
 static std::size_t header_callback(char* data, const std::size_t size, const std::size_t count, void* opaque) { return curl_run_header_callback<IdentityProbe>(data, size, count, opaque); }
 // The probe only needs the parsed response state; individual header kinds carry no policy.
 void on_header(HttpHeaderKind, std::string_view) const noexcept {}
};
[[nodiscard]] RemoteArtifactIdentity probe_remote_identity(
 const DownloadRequest& request, mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace, BenchmarkCurl& transport) {
 CurlMultiTransfers<IdentityProbe> active(transport, BenchmarkCurl::Class::Artifact, cancel_requested);
 for (std::uint32_t attempt = 1U; attempt <= request.maximum_attempts; ++attempt) {
  throw_if_benchmark_cancelled(cancel_requested);
  active.add(std::make_unique<IdentityProbe>(request, cancel_requested));
  std::optional<CurlMultiTransfers<IdentityProbe>::Completion> completed;
  while (!(completed = active.next_completed())) {
   throw_if_benchmark_cancelled(cancel_requested);
   active.poll(kBenchmarkTransferPollMilliseconds);
  }
  auto& probe = *completed->transfer;
  const CURLcode result = completed->result;
  (void)curl_easy_getinfo(probe.easy.get(), CURLINFO_RESPONSE_CODE, &probe.http.response_code);
  if (probe.callback_error) { std::rethrow_exception(probe.callback_error); }
  throw_if_benchmark_cancelled(cancel_requested);
  reject_local_curl_failure(result);
  const bool valid_range = result == CURLE_OK && probe.http.response_code == 206L && probe.http.range && !probe.http.range->unsatisfied && probe.http.range->start == 0U &&
                           probe.http.range->end == 0U && probe.http.range->total == request.expected_size;
  const bool strong_etag = !probe.http.etag.empty() && !starts_with_case_insensitive(probe.http.etag, "W/");
  if (valid_range && (strong_etag || !probe.http.last_modified.empty())) {
   RemoteArtifactIdentity identity{
    strong_etag ? probe.http.etag : std::string{},
    probe.http.last_modified,
   };
   trace_benchmark_event(trace, "benchmark.download.segmented_probe",
    [&] { return nlohmann::json{{"artifact", request.artifact_id}, {"attempt", attempt}, {"bytes", request.expected_size}, {"etag", identity.etag}, {"last_modified", identity.last_modified}}; });
   return identity;
  }
  if (probe.http.response_code == 200L || (valid_range && !strong_etag && probe.http.last_modified.empty())) { throw SegmentedDownloadUnsupported("server does not provide stable ranged downloads"); }
  trace_benchmark_event(trace, "benchmark.download.segmented_probe_retry", [&] {
   return nlohmann::json{
    {"artifact", request.artifact_id}, {"attempt", attempt}, {"curl_code", static_cast<int>(result)}, {"http_status", probe.http.response_code},
    {"detail", probe.error_buffer[0] != '\0' ? probe.error_buffer.data() : curl_easy_strerror(result)}
   };
  });
  if (attempt == request.maximum_attempts) { throw BenchmarkDownloadUnavailable("cannot establish a stable ranged download identity for " + request.artifact_id); }
  // The remote probe retry is intentionally deadline-based HTTP backoff, not local status polling.
  throw_if_benchmark_cancelled(cancel_requested);
  const auto deadline = Clock::now() + std::chrono::milliseconds{std::min<std::uint64_t>(4000U, 250U << std::min<std::uint32_t>(attempt - 1U, 4U))};
  while (Clock::now() < deadline) {
   throw_if_benchmark_cancelled(cancel_requested);
   active.wait_until(std::min(deadline, Clock::now() + std::chrono::milliseconds{250}));
  }
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
 DownloadStorage& storage_;

public:
 SegmentedDownloadState(const DownloadRequest& request, const RemoteArtifactIdentity& identity, const std::size_t segment_count, const DownloadProgressSink& progress, const BenchmarkTraceSink& trace,
  const int descriptor, const mmltk::common::concurrency::CancellationObservation cancellation, DownloadStorage& storage, nlohmann::json cached)
     : storage_(storage), request_(request), identity_(identity), progress_(progress), trace_(trace), descriptor_(descriptor), cancellation_(cancellation) {
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
  load_resume_state(cached);
 }
 [[nodiscard]] std::size_t size() const noexcept { return segments_.size(); }
 [[nodiscard]] DownloadSegment segment(const std::size_t index) {
  const std::lock_guard lock(mutex_);
  return segments_.at(index);
 }
 void report_in_flight(const std::size_t index, const std::uint64_t bytes) {
  if (!progress_ && !trace_) return;
  const std::lock_guard lock(mutex_);
  in_flight_bytes_ -= in_flight_.at(index);
  in_flight_bytes_ = checked_u64_add(in_flight_bytes_, bytes, "segmented in-flight byte overflow");
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
  if (attempt > 1U) retained_bytes_ = committed_bytes_;
  emit_progress_locked(true);
 }
 void commit_attempt(const std::size_t index, const std::uint64_t attempt_begin, const std::uint64_t transferred, const std::uint32_t attempt) {
  std::vector<DownloadSegment> checkpoint;
  bool complete = false;
  {
   const std::lock_guard lock(mutex_);
   DownloadSegment& segment = segments_.at(index);
   const auto expected_begin = checked_u64_add(segment.begin, segment.completed, "segmented download attempt offset overflow");
   if (attempt_begin != expected_begin || transferred > segment.end + 1U - expected_begin) throw std::runtime_error("segmented download attempt is outside its assigned range");
   segment.completed = checked_u64_add(segment.completed, transferred, "segmented download completion overflow");
   committed_bytes_ = checked_u64_add(committed_bytes_, transferred, "segmented committed byte overflow");
   maximum_attempt_ = std::max(maximum_attempt_, attempt);
   segment.attempts = std::max(segment.attempts, attempt);
   in_flight_bytes_ -= in_flight_.at(index);
   in_flight_.at(index) = 0;
   checkpoint = segments_;
   complete = segment.completed == segment.end + 1U - segment.begin;
  }
  persist(checkpoint);
  {
   const std::lock_guard lock(mutex_);
   emit_progress_locked(complete);
  }
 }
 void abandon_attempt(const std::size_t index, const std::uint32_t attempt) {
  std::vector<DownloadSegment> checkpoint;
  {
   const std::lock_guard lock(mutex_);
   segments_.at(index).attempts = std::max(segments_.at(index).attempts, attempt);
   maximum_attempt_ = std::max(maximum_attempt_, attempt);
   in_flight_bytes_ -= in_flight_.at(index);
   in_flight_.at(index) = 0;
   checkpoint = segments_;
  }
  persist(checkpoint);
 }
 [[nodiscard]] bool resumed() const noexcept { return resumed_; }
 [[nodiscard]] std::uint64_t retained_bytes() const {
  const std::lock_guard lock(mutex_);
  return retained_bytes_;
 }
 [[nodiscard]] std::uint32_t maximum_attempts() const {
  const std::lock_guard lock(mutex_);
  return maximum_attempt_;
 }

private:
 [[nodiscard]] nlohmann::json metadata(std::span<const DownloadSegment> snapshot) const {
  nlohmann::json segments = nlohmann::json::array();
  for (const DownloadSegment& segment : snapshot) { segments.push_back({{"begin", segment.begin}, {"end", segment.end}, {"completed", segment.completed}, {"attempts", segment.attempts}}); }
  return nlohmann::json{
   {"schema_version", kBenchmarkCacheSchemaVersion}, {"mode", "segmented"}, {"url", request_.url}, {"size", request_.expected_size}, {"etag", identity_.etag},
   {"last_modified", identity_.last_modified}, {"segments", std::move(segments)}
  };
 }
 void load_resume_state(const nlohmann::json& cached) {
  bool valid = false;
  struct stat partial_status{};
  if (::fstat(descriptor_, &partial_status) != 0) throw errno_error("cannot inspect segmented partial", partial_path(request_).string());
  if (!cached.is_null() && S_ISREG(partial_status.st_mode) && partial_status.st_size >= 0 && static_cast<std::uint64_t>(partial_status.st_size) == request_.expected_size) {
   try {
    const nlohmann::json& cached_segments = cached.at("segments");
    valid = cached.value("schema_version", 0U) == kBenchmarkCacheSchemaVersion && cached.value("mode", std::string{}) == "segmented" && cached.value("url", std::string{}) == request_.url &&
            cached.value("size", 0ULL) == request_.expected_size && cached.value("etag", std::string{}) == identity_.etag && cached.value("last_modified", std::string{}) == identity_.last_modified &&
            cached_segments.is_array() && !cached_segments.empty() &&
            cached_segments.size() <= std::min<std::uint64_t>(8, request_.expected_size / kMinimumSegmentBytes + (request_.expected_size % kMinimumSegmentBytes != 0));
    // Bound untrusted topology before allocation by what historical writers
    // could emit, independently of the current connection budget.
    if (valid) {
     std::vector<DownloadSegment> admitted;
     admitted.reserve(cached_segments.size());
     std::uint64_t next = 0;
     for (const auto& value : cached_segments) {
      const auto begin = value.at("begin").get<std::uint64_t>();
      const auto end = value.at("end").get<std::uint64_t>();
      const auto completed = value.at("completed").get<std::uint64_t>();
      if (begin != next || end < begin || end >= request_.expected_size || completed > end - begin + 1) {
       valid = false;
       break;
      }
      admitted.push_back({begin, end, completed, 0});
      next = end + 1;
     }
     valid = valid && next == request_.expected_size;
     if (valid) segments_ = std::move(admitted);
    }
   } catch (const std::exception& error) {
    if (is_benchmark_capacity_failure(error)) throw;
    valid = false;
   }
  }
  if (!valid) {
   for (DownloadSegment& segment : segments_) {
    segment.completed = 0U;
    segment.attempts = 0U;
   }
   storage_.growth.withdraw_allocation();
   if (::ftruncate(descriptor_, 0) != 0 || ::ftruncate(descriptor_, checked_cast<off_t>(request_.expected_size, "segmented download allocation overflow")) != 0) {
    throw errno_error("cannot allocate segmented benchmark download", partial_path(request_).string());
   }
  }
  resumed_ = std::ranges::any_of(segments_, [](const DownloadSegment& segment) { return segment.completed != 0U; });
  in_flight_.assign(segments_.size(), 0U);
  for (const auto& segment : segments_) committed_bytes_ = checked_u64_add(committed_bytes_, segment.completed, "segmented retained byte overflow");
  retained_bytes_ = committed_bytes_;
  persist(segments_);
  emit_progress_locked(false);
  trace_benchmark_event(trace_, "benchmark.download.segmented_resume_state", [&] { return nlohmann::json{{"artifact", request_.artifact_id}, {"segments", segments_.size()}, {"resumed", resumed_}}; });
 }
 // Only the artifact controller commits attempts. Ordered immutable snapshots
 // persist outside the callback/progress mutex while other sockets keep running.
 // Metadata sync does not make each partial data write power-loss durable.
 void persist(std::span<const DownloadSegment> checkpoint) const {
  storage_.growth.reconcile(descriptor_);
  write_json_atomically(partial_metadata_path(request_), metadata(checkpoint), cancellation_, &storage_.filesystem);
 }

public:
 void settle_written() { storage_.growth.reconcile(descriptor_); }

private:
 void emit_progress_locked(const bool force) const {
  if (!progress_ && !trace_) { return; }
  const auto completed = checked_u64_add(committed_bytes_, in_flight_bytes_, "segmented progress byte overflow");
  if (completed > request_.expected_size) throw std::runtime_error("segmented download progress exceeds artifact size");
  const auto attempts = std::max(maximum_attempt_, active_attempt_);
  if (force || completed <= request_.expected_size) {
   if (progress_) {
    progress_(DownloadProgress{
     request_.artifact_id, {completed, request_.expected_size, retained_bytes_, attempts, false, retained_bytes_ != 0U}, DownloadProgressPhase::kDownloading, request_.redownload, request_.source
    });
   }
   trace_transfer_progress(trace_, request_, completed, request_.expected_size, attempts, retained_bytes_ != 0U, retained_bytes_, committed_bytes_, request_.redownload);
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
 std::uint32_t active_attempt_ = 0U, maximum_attempt_ = 0U;
 std::uint64_t committed_bytes_ = 0U, in_flight_bytes_ = 0U;
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
 SegmentTransfer(const DownloadRequest& request_value, const RemoteArtifactIdentity& identity_value, SegmentedDownloadState& state_value, const std::size_t index, const std::uint64_t begin,
  const std::uint64_t end, const std::uint32_t attempt_value, const int descriptor_value, mmltk::common::concurrency::CancellationObservation cancel)
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
  // CLEANUP-IGNORE: Independent CURL transfer modes initialize the common transfer-setup record; their callbacks and ownership differ.
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
   if (bytes > transfer.range_end + 1U - transfer.write_offset) { throw BenchmarkDownloadUnavailable("segmented response exceeds its validated byte range"); }
   append_transfer_bytes(&transfer.write_offset, SegmentTransfer::kWriteContext, transfer.descriptor, data, bytes);
   transfer.state.settle_written();
   transfer.state.report_in_flight(transfer.segment_index, transfer.transferred());
   return bytes;
  });
 }
 static std::size_t header_callback(char* data, const std::size_t size, const std::size_t count, void* opaque) { return curl_run_header_callback<SegmentTransfer>(data, size, count, opaque); }
 void on_header(const HttpHeaderKind kind, const std::string_view header) {
  if (kind == HttpHeaderKind::kStatusLine) {
   response_headers_valid = false;
  } else if (kind == HttpHeaderKind::kOther && is_header_block_terminator(header)) {
   response_headers_valid = http.response_code == 206L && http.range && !http.range->unsatisfied && http.range->start == range_begin && http.range->end == range_end &&
                            http.range->total == request.expected_size && response_identity_matches();
  }
 }
};
[[nodiscard]] DownloadResult download_segmented_artifact(const DownloadRequest& request, const std::size_t maximum_concurrency, mmltk::common::concurrency::CancellationObservation cancel_requested,
 const DownloadProgressSink& progress, const BenchmarkTraceSink& trace, DownloadStorage& storage, BenchmarkCurl& transport, nlohmann::json cached) {
 CurlGlobal::initialize();
 const RemoteArtifactIdentity identity = probe_remote_identity(request, cancel_requested, trace, transport);
 const std::uint64_t segments_for_size = request.expected_size / kMinimumSegmentBytes + static_cast<std::uint64_t>(request.expected_size % kMinimumSegmentBytes != 0U);
 const std::size_t initial_segment_count = checked_cast<std::size_t>(
  std::max<std::uint64_t>(1U, std::min(checked_cast<std::uint64_t>(maximum_concurrency, "segmented concurrency overflow"), segments_for_size)), "segmented download count overflow");
 (void)mmltk::common::io::ensure_parent_directory(request.destination);
 const int descriptor = ::open(partial_path(request).c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
 if (descriptor < 0) { throw errno_error("cannot open segmented benchmark download", partial_path(request).string()); }
 ScopedFd partial(descriptor);
 SegmentedDownloadState state(request, identity, initial_segment_count, progress, trace, descriptor, cancel_requested, storage, std::move(cached));
 const auto segment_count = state.size();
 trace_benchmark_event(trace, "benchmark.download.segmented_start",
  [&] { return nlohmann::json{{"artifact", request.artifact_id}, {"segments", segment_count}, {"bytes", request.expected_size}, {"resumed", state.resumed()}}; });
 struct PendingRange {
  Clock::time_point deadline;
  std::size_t index;
  bool operator>(const PendingRange& other) const { return deadline > other.deadline || (deadline == other.deadline && index > other.index); }
 };
 std::priority_queue<PendingRange, std::vector<PendingRange>, std::greater<>> pending;
 for (std::size_t index = 0; index < segment_count; ++index) {
  const auto segment = state.segment(index);
  if (segment.completed != segment.end - segment.begin + 1) pending.push({Clock::time_point{}, index});
 }
 CurlMultiTransfers<SegmentTransfer> active(transport, BenchmarkCurl::Class::Artifact, cancel_requested);
 const auto settle = [&](SegmentTransfer& transfer) {
  if (transfer.response_headers_valid)
   state.commit_attempt(transfer.segment_index, transfer.range_begin, transfer.transferred(), transfer.attempt);
  else
   state.abandon_attempt(transfer.segment_index, transfer.attempt);
 };
 try {
  while (!pending.empty() || !active.empty()) {
   throw_if_benchmark_cancelled(cancel_requested);
   while (!pending.empty() && active.size() < maximum_concurrency && pending.top().deadline <= Clock::now()) {
    const auto index = pending.top().index;
    pending.pop();
    const auto segment = state.segment(index);
    const auto attempt = segment.attempts + 1;
    if (attempt > request.maximum_attempts) throw BenchmarkDownloadUnavailable("segmented benchmark download exhausted retries for " + request.artifact_id);
    state.begin_attempt(attempt);
    // The first live range gets an artifact turn; additional ranges yield to
    // independent whole artifacts and identity probes in the shared transport.
    const bool extra = !active.empty();
    active.add(std::make_unique<SegmentTransfer>(request, identity, state, index, segment.begin + segment.completed, segment.end, attempt, descriptor, cancel_requested), extra);
   }
   while (auto completed = active.next_completed()) {
    auto& transfer = *completed->transfer;
    (void)curl_easy_getinfo(transfer.easy.get(), CURLINFO_RESPONSE_CODE, &transfer.http.response_code);
    settle(transfer);
    if (transfer.callback_error) std::rethrow_exception(transfer.callback_error);
    throw_if_benchmark_cancelled(cancel_requested);
    reject_local_curl_failure(completed->result);
    if (transfer.http.response_code == 200L) throw SegmentedDownloadUnsupported("server ignored a segmented byte range");
    if (completed->result == CURLE_OK && transfer.response_headers_valid && transfer.write_offset == transfer.range_end + 1) continue;
    trace_benchmark_event(trace, "benchmark.download.segment_retry", [&] {
     return nlohmann::json{
      {"artifact", request.artifact_id}, {"segment", transfer.segment_index}, {"attempt", transfer.attempt}, {"curl_code", static_cast<int>(completed->result)},
      {"http_status", transfer.http.response_code}, {"completed_bytes", transfer.transferred()},
      {"detail", transfer.error_buffer[0] ? transfer.error_buffer.data() : curl_easy_strerror(completed->result)}
     };
    });
    if (transfer.attempt >= request.maximum_attempts) throw BenchmarkDownloadUnavailable("segmented benchmark download failed after retries for " + request.artifact_id);
    pending.push({Clock::now() + std::chrono::milliseconds{std::min<std::uint64_t>(4000, 250U << std::min<std::uint32_t>(transfer.attempt - 1, 4))}, transfer.segment_index});
   }
   if (!pending.empty() || !active.empty()) {
    const auto now = Clock::now();
    active.wait_until(std::min(now + std::chrono::milliseconds{250}, pending.empty() || pending.top().deadline <= now ? Clock::time_point::max() : pending.top().deadline));
   }
  }
 } catch (...) {
  const auto error = std::current_exception();
  // Detach all callbacks before snapshots, fallback removal or truncation.
  active.abandon_all([&](SegmentTransfer& transfer) {
   try {
    settle(transfer);
   } catch (...) {}
  });
  std::rethrow_exception(error);
 }
 if (::fdatasync(descriptor) != 0) { throw errno_error("cannot flush segmented benchmark download", partial_path(request).string()); }
 storage.growth.reconcile(descriptor);
 partial = ScopedFd{};
 throw_if_benchmark_cancelled(cancel_requested);
 const std::string identity_digest = artifact_identity(request, request.expected_size, identity.etag, identity.last_modified);
 throw_if_benchmark_cancelled(cancel_requested);
 std::filesystem::rename(partial_path(request), request.destination);
 storage.growth.reconcile_download(request.destination);
 sync_parent_directory(request.destination);
 const std::uint32_t attempts = state.maximum_attempts();
 write_json_atomically(complete_metadata_path(request),
  nlohmann::json{
   {"schema_version", kBenchmarkCacheSchemaVersion}, {"complete", true}, {"url", request.url}, {"size", request.expected_size}, {"identity", identity_digest}, {"integrity_mode", "http_identity_size"},
   {"etag", identity.etag}, {"last_modified", identity.last_modified}, {"attempts", attempts}, {"segments", segment_count}
  },
  cancel_requested, &storage.filesystem);
 std::error_code remove_error;
 std::filesystem::remove(partial_metadata_path(request), remove_error);
 if (remove_error) { throw std::filesystem::filesystem_error("cannot remove segmented download metadata", partial_metadata_path(request), remove_error); }
 trace_benchmark_event(trace, "benchmark.download.segmented_complete", [&] {
  return nlohmann::json{
   {"artifact", request.artifact_id}, {"bytes", request.expected_size}, {"identity", identity_digest}, {"segments", segment_count}, {"attempts", attempts}, {"resumed", state.retained_bytes() != 0U},
   {"retained_bytes", state.retained_bytes()}, {"redownload", request.redownload}
  };
 });
 return DownloadResult{
  request.destination,
  request.expected_size,
  identity_digest,
  {},
  identity.etag,
  identity.last_modified,
  attempts,
  state.resumed(),
  false,
 };
}
[[nodiscard]] DownloadResult publish_completed_transfer(Transfer& transfer) {
 const DownloadRequest& request = transfer.request;
 if (::fdatasync(transfer.partial.get()) != 0) { throw errno_error("cannot flush benchmark partial download", partial_path(request).string()); }
 transfer.storage.growth.reconcile(transfer.partial.get());
 transfer.partial = ScopedFd{};
 const std::uint64_t size = regular_file_size(partial_path(request));
 if (size == 0U || (request.expected_size != 0U && size != request.expected_size)) { throw DownloadVerificationError("benchmark download size does not match its catalog"); }
 throw_if_benchmark_cancelled(transfer.cancel_requested);
 const std::string identity = artifact_identity(request, size, transfer.effective_etag(), transfer.effective_last_modified());
 throw_if_benchmark_cancelled(transfer.cancel_requested);
 std::filesystem::rename(partial_path(request), request.destination);
 transfer.storage.growth.reconcile_download(request.destination);
 sync_parent_directory(request.destination);
 write_json_atomically(complete_metadata_path(request),
  nlohmann::json{
   {"schema_version", kBenchmarkCacheSchemaVersion}, {"complete", true}, {"url", request.url}, {"size", size}, {"identity", identity}, {"integrity_mode", "http_identity_size"},
   {"etag", transfer.effective_etag()}, {"last_modified", transfer.effective_last_modified()}, {"attempts", transfer.attempt}
  },
  transfer.cancel_requested, &transfer.storage.filesystem);
 std::error_code ignored;
 std::filesystem::remove(partial_metadata_path(request), ignored);
 transfer.emit_progress(size, size, size);
 trace_benchmark_event(transfer.trace, "benchmark.download.complete", [&] {
  return nlohmann::json{
   {"artifact", request.artifact_id}, {"bytes", size}, {"attempt", transfer.attempt}, {"resumed", transfer.resumed && !transfer.response_restarted}, {"redownload", transfer.redownload}
  };
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
namespace {
DownloadResult download_locked_artifact(const DownloadRequest& request, std::size_t concurrency, mmltk::common::concurrency::CancellationObservation cancellation, const DownloadProgressSink& progress,
 const BenchmarkTraceSink& trace, BenchmarkCurl& transport, BenchmarkCompilePipeline* execution, const BenchmarkAllowance& parent, StorageReservationPool& storage) {
 DownloadStorage destination(request, storage, trace);
 if (auto result = validate_complete_artifact(request, cancellation, progress, trace)) {
  if (progress) progress({request.artifact_id, {result->size, result->size, 0, result->attempts, true, false}, DownloadProgressPhase::kDownloading, request.redownload, request.source});
  return *result;
 }
 // The partial descriptor and atomic checkpoint/publication descriptor belong
 // to this source. Active request workspace is admitted separately by Curl.
 BenchmarkAllowance files;
 if (execution) files = execution->reserve(BenchmarkResources::handles(2, true), parent);
 destination.growth.withdraw_allocation();
 remove_invalid_complete_artifact(request);
 destination.growth.reconcile_download(request.destination);
 bool redownload = false;
 // Stored segmented topology is admitted against the current remote identity,
 // independently of today's number of sockets (including a single socket).
 std::optional<nlohmann::json> cached;
 if (request.expected_size >= kSegmentedDownloadThreshold) {
  // An engaged null document records an observed missing/invalid checkpoint.
  // Only the first ordinary attempt consumes it; retries read their new state.
  cached.emplace();
  if (std::filesystem::is_regular_file(partial_metadata_path(request))) {
   try {
    *cached = read_json_file(partial_metadata_path(request));
   } catch (const std::exception& error) {
    if (is_benchmark_capacity_failure(error)) throw;
   }
  }
 }
 const bool stored_segments = cached && cached->is_object() && cached->contains("mode") && (*cached)["mode"] == "segmented";
 if (request.expected_size >= kSegmentedDownloadThreshold && (concurrency > 1 || stored_segments)) {
  try {
   return download_segmented_artifact(request, concurrency, cancellation, progress, trace, destination, transport, std::move(*cached));
  } catch (const SegmentedDownloadUnsupported& error) {
   destination.growth.withdraw_allocation();
   for (const auto& path : {partial_path(request), partial_metadata_path(request)}) {
    std::error_code failure;
    redownload = std::filesystem::remove(path, failure) || redownload;
    if (failure) throw std::filesystem::filesystem_error("cannot reset unsupported segmented download", path, failure);
   }
   cached.emplace();  // The fallback removed both partial backing and metadata.
   destination.growth.reconcile_download(request.destination);
   trace_benchmark_event(
    trace, "benchmark.download.segmented_fallback", [&] { return nlohmann::json{{"artifact", request.artifact_id}, {"reason", error.what()}, {"redownload", request.redownload || redownload}}; });
  }
 }
 CurlMultiTransfers<Transfer> active(transport, BenchmarkCurl::Class::Artifact, cancellation);
 const auto salvage = [](Transfer& transfer) {
  try {
   transfer.persist_partial_metadata();
  } catch (...) {
   transfer.storage.growth.withdraw_allocation();
   transfer.partial = ScopedFd{};
   for (const auto& path : {partial_path(transfer.request), partial_metadata_path(transfer.request)}) {
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) throw std::filesystem::filesystem_error("cannot invalidate interrupted benchmark partial download", path, error);
   }
  }
 };
 try {
  for (std::uint32_t attempt = 1; attempt <= request.maximum_attempts; ++attempt) {
   throw_if_benchmark_cancelled(cancellation);
   active.add(std::make_unique<Transfer>(request, progress, trace, cancellation, attempt, redownload, destination, std::exchange(cached, std::nullopt)));
   trace_benchmark_event(trace, "benchmark.download.start", [&] { return nlohmann::json{{"artifact", request.artifact_id}, {"attempt", attempt}}; });
   std::optional<CurlMultiTransfers<Transfer>::Completion> completed;
   while (!(completed = active.next_completed())) {
    throw_if_benchmark_cancelled(cancellation);
    active.poll(kBenchmarkTransferPollMilliseconds);
   }
   auto& transfer = *completed->transfer;
   (void)curl_easy_getinfo(transfer.easy.get(), CURLINFO_RESPONSE_CODE, &transfer.http.response_code);
   bool reset = false;
   std::string detail;
   if (transfer.callback_error) {
    if (!transfer.callback_retryable) {
     salvage(transfer);
     std::rethrow_exception(transfer.callback_error);
    }
    reset = true;
    try {
     std::rethrow_exception(transfer.callback_error);
    } catch (const std::exception& error) {
     if (is_benchmark_capacity_failure(error)) throw;
     detail = error.what();
    } catch (...) { detail = "non-standard HTTP range callback exception"; }
   } else {
    const bool successful_status =
     transfer.response_headers_valid && (transfer.http.response_code == 200L || transfer.http.response_code == 206L ||
                                         (transfer.http.response_code == 416L && transfer.response_total && regular_file_size(partial_path(request)) == transfer.response_total));
    const bool complete_range = transfer.http.response_code != 206L || (transfer.response_total && regular_file_size(partial_path(request)) == transfer.response_total);
    if (completed->result == CURLE_OK && successful_status && complete_range) {
     // No connection slot is held during sync, metadata publication, or callbacks.
     try {
      return publish_completed_transfer(transfer);
     } catch (const DownloadVerificationError& error) {
      reset = true;
      detail = error.what();
     }
    } else
     detail = completed->result == CURLE_OK && successful_status && !complete_range ? "resumed transfer did not reach the declared Content-Range total"
              : completed->result == CURLE_OK                                       ? "HTTP response status or headers were not acceptable"
               : transfer.error_buffer[0]                                           ? transfer.error_buffer.data()
                                                                                    : curl_easy_strerror(completed->result);
   }
   if (!reset) transfer.persist_partial_metadata();
   throw_if_benchmark_cancelled(cancellation);
   reject_local_curl_failure(completed->result);
   if (reset) {
    destination.growth.withdraw_allocation();
    transfer.partial = ScopedFd{};
    for (const auto& path : {partial_path(request), partial_metadata_path(request)}) {
     std::error_code error;
     std::filesystem::remove(path, error);
     if (error) throw std::filesystem::filesystem_error("cannot reset invalid benchmark partial download", path, error);
    }
   }
   destination.growth.reconcile_download(request.destination);
   trace_benchmark_event(trace, "benchmark.download.attempt_failed", [&] {
    return nlohmann::json{
     {"artifact", request.artifact_id}, {"attempt", attempt}, {"curl_code", static_cast<int>(completed->result)}, {"http_status", transfer.http.response_code}, {"reset_partial", reset},
     {"detail", detail}
    };
   });
   if (attempt == request.maximum_attempts) throw BenchmarkDownloadUnavailable("benchmark download failed after retries for " + request.artifact_id + ": " + detail);
   redownload = transfer.redownload || reset;
   completed.reset();
   const auto deadline = Clock::now() + std::chrono::milliseconds{std::min<std::uint64_t>(4000, 250U << std::min<std::uint32_t>(attempt - 1, 4))};
   while (Clock::now() < deadline) {
    throw_if_benchmark_cancelled(cancellation);
    active.wait_until(std::min(deadline, Clock::now() + std::chrono::milliseconds{250}));
   }
  }
 } catch (...) {
  const auto original = std::current_exception();
  active.abandon_all(salvage);
  std::rethrow_exception(original);
 }
 throw std::logic_error("benchmark download exhausted its attempt loop");
}
class DownloadBatch final {
 using Cancellation = mmltk::common::concurrency::CancellationObservation;
 struct Job {
  std::size_t index;
  std::shared_ptr<ArtifactLease> lease;
 };
 const std::vector<DownloadRequest>& requests_;
 const DownloadProgressSink& observer_;
 const BenchmarkTraceSink& trace_;
 const DownloadReadySink& ready_;
 BenchmarkCompilePipeline* execution_;
 const BenchmarkAllowance& parent_;
 Cancellation external_;
 std::atomic<bool> stopped_{false};
 std::unique_ptr<BenchmarkCurl> local_transport_;
 BenchmarkCurl& transport_;
 std::size_t concurrency_;
 BenchmarkResources source_resources_ = BenchmarkResources::handles(1, true, 2);
 std::unique_ptr<BenchmarkCurl::Channel> source_transport_;
 StorageReservationPool storage_;
 Cancellation cancellation_ = Cancellation::Borrow(*this);
 std::mutex mutex_, observer_mutex_;
 std::condition_variable changed_, jobs_changed_;
 std::exception_ptr error_;
 DownloadProgressSink progress_;
 std::vector<DownloadResult> results_;
 std::list<std::size_t> pending_;
 std::vector<std::optional<Job>> jobs_;
 std::vector<std::size_t> free_, completed_, done_;
 std::size_t running_ = 0;
 // Last member and explicitly joined: every borrow above survives each worker.
 std::vector<std::jthread> controllers_;
 void retire() noexcept {
  stopped_.store(true, std::memory_order_relaxed);
  jobs_changed_.notify_all();
  controllers_.clear();
 }
 void consume(std::size_t slot) noexcept {
  for (;;) {
   std::optional<Job> job;
   {
    std::unique_lock lock(mutex_);
    jobs_changed_.wait(lock, [&] { return stopped_.load(std::memory_order_relaxed) || jobs_[slot].has_value(); });
    if (stopped_.load(std::memory_order_relaxed)) return;
    job = std::move(jobs_[slot]);
    jobs_[slot].reset();
   }
   try {
    results_[job->index] = download_locked_artifact(requests_[job->index], concurrency_, cancellation_, progress_, trace_, transport_, execution_, job->lease->allowance(), storage_);
    job->lease.reset();
    if (ready_) {
     const std::lock_guard lock(observer_mutex_);
     ready_({job->index, results_[job->index]});
    }
   } catch (...) {
    const std::lock_guard lock(mutex_);
    if (!error_) error_ = std::current_exception();
    stopped_.store(true, std::memory_order_relaxed);
    jobs_changed_.notify_all();
   }
   // The slot is reusable only after its complete borrowed job has retired.
   job.reset();
   {
    const std::lock_guard lock(mutex_);
    completed_.push_back(slot);
   }
   changed_.notify_one();
  }
 }

public:
 DownloadBatch(const std::vector<DownloadRequest>& requests, std::size_t requested_concurrency, Cancellation cancellation, const DownloadProgressSink& observer, const BenchmarkTraceSink& trace,
  const DownloadReadySink& ready, BenchmarkCompilePipeline* execution, const BenchmarkAllowance& parent, StorageReservationPool* storage)
     : requests_(requests),
       observer_(observer),
       trace_(trace),
       ready_(ready),
       execution_(execution),
       parent_(parent),
       external_(cancellation),
       local_transport_(execution ? nullptr : std::make_unique<BenchmarkCurl>(requested_concurrency)),
       transport_(execution ? execution->curl() : *local_transport_),
       concurrency_(std::min(requested_concurrency, transport_.limit(BenchmarkCurl::Class::Artifact))),
       source_transport_(transport_.channel(BenchmarkCurl::Class::Artifact, cancellation, source_resources_)),
       storage_(execution ? execution->storage()
                : storage ? *storage
                          : StorageReservationPool(requests.front().destination, trace)),
       results_(requests.size()),
       jobs_(std::min(requests.size(), concurrency_)),
       free_(jobs_.size()) {
  if (observer_)
   progress_ = [this](const DownloadProgress& update) {
    const std::lock_guard lock(observer_mutex_);
    observer_(update);
   };
  for (std::size_t index = 0; index < requests.size(); ++index) pending_.push_back(index);
  std::iota(free_.begin(), free_.end(), 0);
  completed_.reserve(jobs_.size());
  done_.reserve(jobs_.size());
  controllers_.reserve(jobs_.size());
 }
 ~DownloadBatch() { retire(); }
 DownloadBatch(const DownloadBatch&) = delete;
 DownloadBatch& operator=(const DownloadBatch&) = delete;
 bool cancelled() const noexcept { return stopped_.load(std::memory_order_relaxed) || external_.requested(); }
 std::vector<DownloadResult> run() try {
  // Thread startup is inside the fully constructed owner's lifetime. A partial
  // startup failure signals and joins every already-started controller.
  for (std::size_t slot = 0; slot < jobs_.size(); ++slot) controllers_.emplace_back([this, slot] { consume(slot); });
  while (!pending_.empty() || running_) {
   {
    const std::lock_guard lock(mutex_);
    if (error_) std::rethrow_exception(error_);
    done_.swap(completed_);
   }
   throw_if_benchmark_cancelled(external_);
   for (const auto slot : done_) {
    free_.push_back(slot);
    --running_;
   }
   done_.clear();
   for (auto it = pending_.begin(); it != pending_.end() && !free_.empty();) {
    const auto index = *it;
    auto lease = ArtifactLease::try_acquire_charged(requests_[index].lock_path, cancellation_, execution_, source_resources_, parent_);
    if (!lease) {
     ++it;
     continue;
    }
    const auto slot = free_.back();
    free_.pop_back();
    it = pending_.erase(it);
    ++running_;
    {
     const std::lock_guard lock(mutex_);
     if (error_) std::rethrow_exception(error_);
     jobs_[slot].emplace(index, std::move(lease));
    }
    jobs_changed_.notify_all();
   }
   if (!pending_.empty() || running_) {
    std::unique_lock lock(mutex_);
    // Only source-lock retry has no readiness event. Completed jobs notify.
    changed_.wait_for(lock, std::chrono::milliseconds{100}, [&] { return error_ || !completed_.empty() || external_.requested(); });
   }
  }
  retire();
  return std::move(results_);
 } catch (...) {
  {
   const std::lock_guard lock(mutex_);
   if (!error_) error_ = std::current_exception();
  }
  retire();
  std::rethrow_exception(error_);
 }
};
}  // namespace
std::vector<DownloadResult> download_artifacts(const std::vector<DownloadRequest>& requests, std::size_t requested_concurrency, mmltk::common::concurrency::CancellationObservation cancel_requested,
 const DownloadProgressSink& observer, const BenchmarkTraceSink& trace, const DownloadReadySink& ready, BenchmarkCompilePipeline* execution, const BenchmarkAllowance& parent,
 StorageReservationPool* storage) {
 if (requests.empty()) return {};
 if (!requested_concurrency) throw std::runtime_error("benchmark download concurrency must be positive");
 std::unordered_set<std::filesystem::path> locks;
 for (const auto& request : requests) {
  if (request.artifact_id.empty() || request.url.empty() || request.destination.empty() || request.lock_path.empty() || !request.maximum_attempts)
   throw std::runtime_error("benchmark download request is incomplete");
  if (request.expected_sha256) (void)mmltk::common::io::parse_sha256_hex(*request.expected_sha256);
  if (!locks.insert(request.lock_path).second) throw std::runtime_error("benchmark download batch contains a duplicate cache lock");
 }
 DownloadBatch batch(requests, requested_concurrency, cancel_requested, observer, trace, ready, execution, parent, storage);
 return batch.run();
}
void invalidate_download_artifact(const DownloadRequest& request, mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace,
 BenchmarkCompilePipeline* execution, const BenchmarkAllowance& parent) {
 if (request.artifact_id.empty() || request.destination.empty() || request.lock_path.empty()) { throw std::runtime_error("benchmark download invalidation request is incomplete"); }
 auto lease = ArtifactLease::acquire_charged(request.lock_path, cancel_requested, execution, BenchmarkResources::handles(1, true), parent);
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
 trace_benchmark_event(trace, "benchmark.download.invalidated", [&] { return nlohmann::json{{"artifact", request.artifact_id}, {"removed_paths", removed}}; });
}
}  // namespace mmltk::backend::data::benchmark_internal
