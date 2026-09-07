#pragma once  // backend.data private implementation boundary

#include <curl/curl.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "src/common/concurrency/cancellation_observation.h"

namespace mmltk::backend::data::benchmark_internal {

// Every benchmark HTTP transfer targets the same infrastructure, so it shares one hardening policy.
inline constexpr long kBenchmarkConnectTimeoutSeconds = 30L;
inline constexpr long kBenchmarkLowSpeedLimitBytes = 1024L;
inline constexpr long kBenchmarkLowSpeedSeconds = 30L;
inline constexpr const char* kBenchmarkTransferUserAgent = "mmltk/1 benchmark-dataset";
// Upper bound on how long a transfer loop parks in curl_multi_poll before re-checking its own
// pending work (retry deadlines, cache writers, cancellation).
inline constexpr int kBenchmarkTransferPollMilliseconds = 250;

struct CurlEasyDestroy {
    void operator()(CURL* handle) const noexcept;
};

struct CurlMultiDestroy {
    void operator()(CURLM* handle) const noexcept;
};

struct CurlHeadersDestroy {
    void operator()(curl_slist* headers) const noexcept;
};

using CurlEasy = std::unique_ptr<CURL, CurlEasyDestroy>;
using CurlMulti = std::unique_ptr<CURLM, CurlMultiDestroy>;
using CurlHeaders = std::unique_ptr<curl_slist, CurlHeadersDestroy>;

// libcurl requires a single process-wide global initialization; the first caller's outcome is
// shared by every later caller.
inline void ensure_curl_global_initialized(const char* failure_prefix) {
    static std::once_flag initialized;
    static CURLcode status = CURLE_OK;
    std::call_once(initialized, [] { status = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (status != CURLE_OK) { throw std::runtime_error(std::string(failure_prefix) + curl_easy_strerror(status)); }
}

template <typename Value>
void set_curl_option_with_prefix(CURL* handle, const CURLoption option, Value value, const char* failure_prefix, const char* description) {
    const CURLcode status = curl_easy_setopt(handle, option, value);
    if (status != CURLE_OK) { throw std::runtime_error(std::string(failure_prefix) + description + ": " + curl_easy_strerror(status)); }
}

// Computes the byte count of a libcurl callback invocation, rejecting overflowing products.
[[nodiscard]] inline bool curl_callback_byte_count(const std::size_t size, const std::size_t count, std::size_t& bytes) noexcept {
    if (count != 0U && size > std::numeric_limits<std::size_t>::max() / count) { return false; }
    bytes = size * count;
    return true;
}

// Shared CURLOPT_WRITEFUNCTION/CURLOPT_HEADERFUNCTION body: recovers the owner from the opaque pointer, resolves
// the byte count and runs `body(owner, bytes)`. Any escaping exception is parked in the owner's `callback_error`
// and reported to libcurl as a short write, so nothing unwinds through C. `Owner` must expose a
// `std::exception_ptr callback_error` member.
template <typename Owner, typename Body>
std::size_t curl_run_data_callback(const std::size_t size, const std::size_t count, void* opaque, Body&& body) {
    Owner& owner = *static_cast<Owner*>(opaque);
    try {
        std::size_t bytes = 0U;
        if (!curl_callback_byte_count(size, count, bytes)) { return 0U; }
        return body(owner, bytes);
    } catch (...) {
        owner.callback_error = std::current_exception();
        return 0U;
    }
}

// Shared CURLOPT_XFERINFOFUNCTION body: aborts the transfer when the owner's cancel flag is set.
// Owner must expose a `mmltk::common::concurrency::CancellationObservation cancel_requested` member.
template <typename Owner>
int curl_cancel_progress_callback(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const Owner& owner = *static_cast<const Owner*>(opaque);
    return owner.cancel_requested.requested() ? 1 : 0;
}

// Shared CURLOPT_HEADERFUNCTION body: materializes the header line, feeds it to the owner's HTTP
// header state machine and forwards the resulting kind plus the raw line to `owner.on_header(...)`.
// `Owner` must expose an `http` member with `apply(std::string_view)` and a matching `on_header`.
template <typename Owner>
std::size_t curl_run_header_callback(char* data, const std::size_t size, const std::size_t count, void* opaque) {
    return curl_run_data_callback<Owner>(size, count, opaque, [data](Owner& owner, const std::size_t bytes) {
        const std::string_view header(data, bytes);
        owner.on_header(owner.http.apply(header), header);
        return bytes;
    });
}

// The baseline every benchmark transfer needs. `owner` is the transfer instance handed back to each
// libcurl callback; a null `header_callback` leaves header parsing disabled.
struct CurlTransferSetup {
    const char* url = nullptr;
    long maximum_redirects = 10L;
    char* error_buffer = nullptr;
    void* owner = nullptr;
    curl_write_callback write_callback = nullptr;
    curl_write_callback header_callback = nullptr;
    curl_xferinfo_callback progress_callback = nullptr;
};

// Single owner of how a benchmark easy handle is configured: a redirect-following GET with the
// benchmark user agent, signal suppression, connect and stall timeouts, error reporting, and the
// owning transfer registered with every callback. Callers add only the options their transfer
// genuinely needs on top (ranges, extra headers, protocol tuning).
inline void configure_curl_transfer(CURL* handle, const CurlTransferSetup& setup, const char* failure_prefix) {
    set_curl_option_with_prefix(handle, CURLOPT_ERRORBUFFER, setup.error_buffer, failure_prefix, "error buffer");
    set_curl_option_with_prefix(handle, CURLOPT_URL, setup.url, failure_prefix, "URL");
    set_curl_option_with_prefix(handle, CURLOPT_FOLLOWLOCATION, 1L, failure_prefix, "redirect following");
    set_curl_option_with_prefix(handle, CURLOPT_MAXREDIRS, setup.maximum_redirects, failure_prefix, "redirect limit");
    set_curl_option_with_prefix(handle, CURLOPT_NOSIGNAL, 1L, failure_prefix, "signal suppression");
    set_curl_option_with_prefix(handle, CURLOPT_CONNECTTIMEOUT, kBenchmarkConnectTimeoutSeconds, failure_prefix, "connect timeout");
    set_curl_option_with_prefix(handle, CURLOPT_LOW_SPEED_LIMIT, kBenchmarkLowSpeedLimitBytes, failure_prefix, "low-speed limit");
    set_curl_option_with_prefix(handle, CURLOPT_LOW_SPEED_TIME, kBenchmarkLowSpeedSeconds, failure_prefix, "low-speed timeout");
    set_curl_option_with_prefix(handle, CURLOPT_USERAGENT, kBenchmarkTransferUserAgent, failure_prefix, "user agent");
    set_curl_option_with_prefix(handle, CURLOPT_WRITEFUNCTION, setup.write_callback, failure_prefix, "write callback");
    set_curl_option_with_prefix(handle, CURLOPT_WRITEDATA, setup.owner, failure_prefix, "write callback data");
    if (setup.header_callback != nullptr) {
        set_curl_option_with_prefix(handle, CURLOPT_HEADERFUNCTION, setup.header_callback, failure_prefix, "header callback");
        set_curl_option_with_prefix(handle, CURLOPT_HEADERDATA, setup.owner, failure_prefix, "header callback data");
    }
    set_curl_option_with_prefix(handle, CURLOPT_NOPROGRESS, 0L, failure_prefix, "progress enablement");
    set_curl_option_with_prefix(handle, CURLOPT_XFERINFOFUNCTION, setup.progress_callback, failure_prefix, "progress callback");
    set_curl_option_with_prefix(handle, CURLOPT_XFERINFODATA, setup.owner, failure_prefix, "progress callback data");
    set_curl_option_with_prefix(handle, CURLOPT_PRIVATE, setup.owner, failure_prefix, "private transfer data");
}

// Owns the transfers in flight on a libcurl multi handle. Both benchmark transfer loops drive
// through this type, so handle registration, completion demultiplexing, polling, abandonment, and
// multi-API error reporting have one owner; callers keep only their admission and completion policy.
// `Transfer` must expose an `easy` member holding its `CurlEasy` handle.
template <typename Transfer>
class CurlMultiTransfers {
   public:
    struct Completion {
        std::unique_ptr<Transfer> transfer;
        CURL* handle = nullptr;
        CURLcode result = CURLE_OK;
    };

    CurlMultiTransfers(CurlMulti multi, std::string label) : multi_(std::move(multi)), label_(std::move(label)) {}

    [[nodiscard]] bool empty() const noexcept { return active_.empty(); }

    [[nodiscard]] std::size_t size() const noexcept { return active_.size(); }

    void reserve(const std::size_t transfers) { active_.reserve(transfers); }

    // Registers an already-configured transfer and starts it on the multi handle.
    void add(std::unique_ptr<Transfer> transfer) {
        CURL* handle = transfer->easy.get();
        auto [iterator, inserted] = active_.emplace(handle, std::move(transfer));
        if (!inserted) { throw std::runtime_error(label_ + " easy handle is already active"); }
        const CURLMcode status = curl_multi_add_handle(multi_.get(), handle);
        if (status != CURLM_OK) {
            active_.erase(iterator);
            throw std::runtime_error("cannot add " + label_ + ": " + curl_multi_strerror(status));
        }
    }

    void perform() {
        int running = 0;
        const CURLMcode status = curl_multi_perform(multi_.get(), &running);
        if (status != CURLM_OK) { throw std::runtime_error(label_ + " loop failed: " + curl_multi_strerror(status)); }
    }

    // Detaches the next finished transfer, or reports nothing when none has completed yet.
    [[nodiscard]] std::optional<Completion> next_completed() {
        int remaining = 0;
        while (CURLMsg* message = curl_multi_info_read(multi_.get(), &remaining)) {
            if (message->msg != CURLMSG_DONE) { continue; }
            const auto iterator = active_.find(message->easy_handle);
            if (iterator == active_.end()) { throw std::runtime_error(label_ + " completion references an unknown handle"); }
            Completion completion{std::move(iterator->second), message->easy_handle, message->data.result};
            active_.erase(iterator);
            (void)curl_multi_remove_handle(multi_.get(), completion.handle);
            return std::optional<Completion>(std::move(completion));
        }
        return std::nullopt;
    }

    void poll(const int timeout_milliseconds) {
        int descriptors = 0;
        const CURLMcode status = curl_multi_poll(multi_.get(), nullptr, 0, timeout_milliseconds, &descriptors);
        if (status != CURLM_OK) { throw std::runtime_error(label_ + " poll failed: " + curl_multi_strerror(status)); }
    }

    // Detaches every still-running transfer on the failure path, handing each to `release` so the
    // caller can salvage or discard its partial work.
    template <typename Release>
    void abandon_all(Release&& release) {
        // Cleanup outcome does not depend on CURL handle address order.
        for (auto& [handle, transfer] : active_) {  // NOLINT(bugprone-nondeterministic-pointer-iteration-order)
            (void)curl_multi_remove_handle(multi_.get(), handle);
            release(*transfer);
        }
        active_.clear();
    }

   private:
    CurlMulti multi_;
    std::string label_;
    std::unordered_map<CURL*, std::unique_ptr<Transfer>> active_;
};

}  // namespace mmltk::backend::data::benchmark_internal
