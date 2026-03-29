#include "gui/vast_runtime.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

using namespace fastloader::gui;

std::string sample_payload() {
    return R"json([
        {"id": 1001, "gpu_name": "H100 SXM", "num_gpus": 8, "dlperf_usd": 18.0, "dlperf": 120.0, "dph": 8.0, "reliability": 0.99},
        {"id": 1002, "gpu_name": "H100 PCIe", "num_gpus": 4, "dlperf_usd": 17.0, "dlperf": 90.0, "dph": 5.5, "reliability": 0.98},
        {"id": 1003, "gpu_name": "L40S", "num_gpus": 4, "dlperf_usd": 19.0, "dlperf": 70.0, "dph": 4.0, "reliability": 0.97},
        {"id": 1004, "gpu_name": "A100", "num_gpus": 2, "dlperf_usd": 30.0, "dlperf": 80.0, "dph": 3.0, "reliability": 0.95},
        {"id": 1005, "gpu_name": "B200", "num_gpus": 8, "dlperf_usd": 16.5, "dlperf": 140.0, "dph": 9.0, "reliability": 0.995}
    ])json";
}

void test_parse_payload() {
    const std::vector<VastRawOffer> offers = parse_vast_offer_payload(sample_payload());
    assert(offers.size() == 5U);
    assert(offers[0].offer_id == 1001);
    assert(offers[2].gpu_name == "L40S");
}

void test_parse_payload_from_object_rows() {
    const std::vector<VastRawOffer> offers = parse_vast_offer_payload(
        R"json({"rows":[{"id": 2001, "gpu_name": "L4", "num_gpus": 4, "dlperf_usd": 12.0}]})json");
    assert(offers.size() == 1U);
    assert(offers[0].offer_id == 2001);
}

void test_rank_filters_and_limits() {
    const std::vector<VastRawOffer> offers = parse_vast_offer_payload(sample_payload());
    const std::vector<VastOfferSummary> ranked =
        rank_vast_offers(offers, {RemoteGpuFamily::H100, RemoteGpuFamily::LSeries, RemoteGpuFamily::B200}, 2, 4);
    assert(ranked.size() == 2U);
    assert(ranked[0].offer_id == 1003);
    assert(ranked[1].offer_id == 1001);
}

void test_rank_excludes_under_min_gpu_count() {
    const std::vector<VastRawOffer> offers = parse_vast_offer_payload(sample_payload());
    const std::vector<VastOfferSummary> ranked =
        rank_vast_offers(offers, {RemoteGpuFamily::A100, RemoteGpuFamily::H100}, 4, 4);
    assert(ranked.size() == 2U);
    for (const VastOfferSummary& offer : ranked) {
        assert(offer.num_gpus >= 4);
    }
}

void test_l_series_matches_l4_family() {
    const std::vector<VastRawOffer> offers = parse_vast_offer_payload(
        R"json([{"id": 3001, "gpu_name": "L4", "num_gpus": 4, "dlperf_usd": 15.0, "dlperf": 50.0, "dph": 3.0, "reliability": 0.96}])json");
    const std::vector<VastOfferSummary> ranked =
        rank_vast_offers(offers, {RemoteGpuFamily::LSeries}, 2, 4);
    assert(ranked.size() == 1U);
    assert(ranked[0].family == RemoteGpuFamily::LSeries);
}

void test_rank_tie_breakers_prefer_lower_price_then_reliability() {
    const std::vector<VastRawOffer> offers = parse_vast_offer_payload(
        R"json([
            {"id": 4001, "gpu_name": "H200", "num_gpus": 4, "dlperf_usd": 20.0, "dlperf": 100.0, "dph": 5.0, "reliability": 0.95},
            {"id": 4002, "gpu_name": "H200", "num_gpus": 4, "dlperf_usd": 20.0, "dlperf": 100.0, "dph": 4.5, "reliability": 0.94},
            {"id": 4003, "gpu_name": "H200", "num_gpus": 4, "dlperf_usd": 20.0, "dlperf": 100.0, "dph": 4.5, "reliability": 0.97}
        ])json");
    const std::vector<VastOfferSummary> ranked =
        rank_vast_offers(offers, {RemoteGpuFamily::H200}, 3, 4);
    assert(ranked.size() == 3U);
    assert(ranked[0].offer_id == 4003);
    assert(ranked[1].offer_id == 4002);
    assert(ranked[2].offer_id == 4001);
}

} // namespace

int main() {
    test_parse_payload();
    test_parse_payload_from_object_rows();
    test_rank_filters_and_limits();
    test_rank_excludes_under_min_gpu_count();
    test_l_series_matches_l4_family();
    test_rank_tie_breakers_prefer_lower_price_then_reliability();
    return 0;
}
