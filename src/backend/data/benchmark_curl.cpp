#include "detail/benchmark_curl.h"
namespace mmltk::backend::data::benchmark_internal {
void CurlEasyDestroy::operator()(CURL* const handle) const noexcept { curl_easy_cleanup(handle); }
void CurlMultiDestroy::operator()(CURLM* const handle) const noexcept { curl_multi_cleanup(handle); }
void CurlHeadersDestroy::operator()(curl_slist* const headers) const noexcept { curl_slist_free_all(headers); }
}  // namespace mmltk::backend::data::benchmark_internal
