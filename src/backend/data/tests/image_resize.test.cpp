#include "src/backend/data/image_resize.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace mmltk::backend::data;

namespace {

std::vector<uint8_t> make_test_image(int width, int height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3;
            pixels[index + 0] = static_cast<uint8_t>((x * 17 + y * 13) & 0xFF);
            pixels[index + 1] = static_cast<uint8_t>((x * 7 + y * 29 + 11) & 0xFF);
            pixels[index + 2] = static_cast<uint8_t>((x * 31 + y * 5 + 19) & 0xFF);
        }
    }
    return pixels;
}

void test_resize_plan_respects_budget() {
    for (int workers = 1; workers <= 32; ++workers) {
        const ResizeWorkerPlan plan = plan_rgb_resize_workers(workers, true, true);
        REQUIRE(plan.image_workers == workers);
        REQUIRE(plan.resize_threads_per_image == 1);
    }
}

void test_single_thread_resize_is_stable() {
    const std::vector<uint8_t> source = make_test_image(61, 47);
    std::vector<uint8_t> first_pass(static_cast<size_t>(143) * static_cast<size_t>(109) * 3U);
    std::vector<uint8_t> second_pass(first_pass.size());

    RgbImageResizer resizer(1);
    resizer.resize(source.data(), 61, 47, first_pass.data(), 143, 109);
    resizer.resize(source.data(), 61, 47, second_pass.data(), 143, 109);

    REQUIRE(first_pass == second_pass);
}

void test_resizer_rejects_internal_threading() {
    bool threw = false;
    try {
        RgbImageResizer resizer(4);
    } catch (const std::runtime_error&) { threw = true; }
    REQUIRE(threw);
}

void test_same_size_copy_preserves_pixels() {
    const std::vector<uint8_t> source = make_test_image(23, 19);
    std::vector<uint8_t> output(source.size(), 0);
    RgbImageResizer resizer(1);
    resizer.resize(source.data(), 23, 19, output.data(), 23, 19);
    REQUIRE(source == output);
}

void test_parallel_worker_local_resizers_are_stable() {
    const std::vector<uint8_t> source = make_test_image(211, 157);
    std::vector<std::vector<uint8_t>> outputs(8, std::vector<uint8_t>(static_cast<size_t>(332) * static_cast<size_t>(332) * 3U));
    std::vector<std::thread> threads;
    threads.reserve(outputs.size());
    for (auto& output : outputs) {
        threads.emplace_back([&source, &output] {
            RgbImageResizer resizer(1);
            resizer.resize(source.data(), 211, 157, output.data(), 332, 332);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (size_t index = 1; index < outputs.size(); ++index) {
        REQUIRE(outputs[index] == outputs[0]);
    }
}

}  // namespace

TEST_CASE("resize plans respect worker budgets", "[backend][data][image_resize]") { test_resize_plan_respects_budget(); }
TEST_CASE("single-thread resize is stable", "[backend][data][image_resize]") { test_single_thread_resize_is_stable(); }
TEST_CASE("resizer rejects internal threading", "[backend][data][image_resize]") { test_resizer_rejects_internal_threading(); }
TEST_CASE("same-size resize preserves pixels", "[backend][data][image_resize]") { test_same_size_copy_preserves_pixels(); }
TEST_CASE("worker-local resizers remain stable", "[backend][data][image_resize]") { test_parallel_worker_local_resizers_are_stable(); }
