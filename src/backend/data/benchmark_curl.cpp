
#include "detail/benchmark_curl.h"

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

namespace mmltk::backend::data::benchmark_internal {

void CurlEasyDestroy::operator()(CURL* const handle) const noexcept { curl_easy_cleanup(handle); }

void CurlMultiDestroy::operator()(CURLM* const handle) const noexcept { curl_multi_cleanup(handle); }

void CurlHeadersDestroy::operator()(curl_slist* const headers) const noexcept { curl_slist_free_all(headers); }

}  // namespace mmltk::backend::data::benchmark_internal
