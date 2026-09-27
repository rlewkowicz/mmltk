#include "src/backend/data/benchmark/detail/benchmark_curl.h"
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data::benchmark_internal {
BenchmarkTransferEnvelope benchmark_curl_envelope(std::size_t leases, std::uint64_t fixed_bytes, std::uint64_t payload_bytes) {
 using mmltk::common::math::checked_add;
 return {{fixed_bytes, checked_add(std::size_t{8}, leases, "benchmark Curl descriptor overflow"), true},
  {checked_add(std::uint64_t{256U << 10}, payload_bytes, "benchmark Curl workspace overflow"), 3, true}};
}
void CurlEasyDestroy::operator()(CURL* const handle) const noexcept { curl_easy_cleanup(handle); }
void CurlMultiDestroy::operator()(CURLM* const handle) const noexcept { curl_multi_cleanup(handle); }
void CurlHeadersDestroy::operator()(curl_slist* const headers) const noexcept { curl_slist_free_all(headers); }
}  // namespace mmltk::backend::data::benchmark_internal
