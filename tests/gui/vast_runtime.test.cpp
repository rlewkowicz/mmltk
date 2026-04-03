#include "gui/vast_runtime.h"
#include "gui/vast_query_controller.h"

#include "mmltk/runtime/async_runtime.h"

#include "support/catch2_compat.hpp"

#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>
#include <string>
#include <vector>

namespace {

using namespace mmltk::gui;
using namespace mmltk::runtime;

void drain_until(UiCallbackQueue& queue, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done()) {
        queue.drain();
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    queue.drain();
}

VastOfferSummary make_offer_summary() {
    VastOfferSummary offer;
    offer.offer_id = 9001;
    offer.family = RemoteGpuFamily::H100;
    offer.gpu_name = "H100 SXM";
    offer.num_gpus = 8;
    offer.dlperf_usd = 18.25;
    offer.dph = 8.5;
    offer.reliability = 0.99;
    return offer;
}

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

void test_vast_query_controller_launch_updates_state_on_success() {
    BackgroundExecutor executor(1);
    UiCallbackQueue queue;
    VastQueryConfig captured_config;
    std::vector<RemoteGpuFamily> captured_families;
    bool error_callback_called = false;
    VastQueryController controller(
        executor,
        queue,
        [&captured_config, &captured_families](
            const VastQueryConfig& config,
            const std::vector<RemoteGpuFamily>& families) {
            captured_config = config;
            captured_families = families;
            return std::vector<VastOfferSummary>{make_offer_summary()};
        });

    controller.launch(
        "vast-secret",
        {RemoteGpuFamily::H100},
        [&](const std::string&) { error_callback_called = true; });
    assert(controller.running());
    controller.launch("ignored", {RemoteGpuFamily::B200});

    drain_until(queue, [&]() { return !controller.running(); });
    executor.wait_idle();

    assert(!error_callback_called);
    assert(captured_config.api_key == "vast-secret");
    assert(captured_config.min_gpus == 4);
    assert(captured_config.result_limit == 2U);
    assert(captured_families == std::vector<RemoteGpuFamily>{RemoteGpuFamily::H100});
    assert(controller.state().results.size() == 1U);
    assert(controller.state().results.front().offer_id == 9001);
    assert(controller.state().last_error.empty());
    assert(controller.state().last_summary == "query completed: 1 offers");
}

void test_vast_query_controller_launch_reports_errors() {
    BackgroundExecutor executor(1);
    UiCallbackQueue queue;
    std::string callback_error;
    VastQueryController controller(
        executor,
        queue,
        [](const VastQueryConfig&, const std::vector<RemoteGpuFamily>&) -> std::vector<VastOfferSummary> {
            throw std::runtime_error("vast query failed");
        });

    controller.launch(
        "vast-secret",
        {RemoteGpuFamily::H100},
        [&](const std::string& error) { callback_error = error; });

    drain_until(queue, [&]() { return !controller.running(); });
    executor.wait_idle();

    assert(controller.state().results.empty());
    assert(controller.state().last_summary.empty());
    assert(controller.state().last_error == "vast query failed");
    assert(callback_error == "vast query failed");
}

void test_vast_query_controller_armed_offer_summary_roundtrip() {
    BackgroundExecutor executor(1);
    UiCallbackQueue queue;
    VastQueryController controller(executor, queue);

    controller.arm_offer(make_offer_summary());
    assert(controller.armed_offer().has_value());
    assert(controller.armed_offer_summary() == "H100 · H100 SXM · 8 GPUs · DLPerf/$ 18.25 · $8.50/hr");

    controller.clear_armed_offer();
    assert(!controller.armed_offer().has_value());
    assert(controller.armed_offer_summary().empty());
}

} // namespace

MMLTK_REGISTER_TEST_CASE("[gui][vast_runtime]", test_parse_payload);
MMLTK_REGISTER_TEST_CASE("[gui][vast_runtime]", test_parse_payload_from_object_rows);
MMLTK_REGISTER_TEST_CASE("[gui][vast_runtime]", test_rank_filters_and_limits);
MMLTK_REGISTER_TEST_CASE("[gui][vast_runtime]", test_rank_excludes_under_min_gpu_count);
MMLTK_REGISTER_TEST_CASE("[gui][vast_runtime]", test_l_series_matches_l4_family);
MMLTK_REGISTER_TEST_CASE("[gui][vast_runtime]", test_rank_tie_breakers_prefer_lower_price_then_reliability);
MMLTK_REGISTER_TEST_CASE("[gui][vast_runtime]", test_vast_query_controller_launch_updates_state_on_success);
MMLTK_REGISTER_TEST_CASE("[gui][vast_runtime]", test_vast_query_controller_launch_reports_errors);
MMLTK_REGISTER_TEST_CASE("[gui][vast_runtime]", test_vast_query_controller_armed_offer_summary_roundtrip);
