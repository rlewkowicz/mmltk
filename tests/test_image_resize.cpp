#include "image_resize.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace fastloader;

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
        assert(plan.image_workers == workers);
        assert(plan.resize_threads_per_image == 1);
    }
}

void test_single_thread_resize_is_stable() {
    const std::vector<uint8_t> source = make_test_image(61, 47);
    std::vector<uint8_t> first_pass(143 * 109 * 3);
    std::vector<uint8_t> second_pass(first_pass.size());

    RgbImageResizer resizer(1);
    resizer.resize(source.data(), 61, 47, first_pass.data(), 143, 109);
    resizer.resize(source.data(), 61, 47, second_pass.data(), 143, 109);

    assert(first_pass == second_pass);
}

void test_resizer_rejects_internal_threading() {
    bool threw = false;
    try {
        RgbImageResizer resizer(4);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

void test_same_size_copy_preserves_pixels() {
    const std::vector<uint8_t> source = make_test_image(23, 19);
    std::vector<uint8_t> output(source.size(), 0);
    RgbImageResizer resizer(1);
    resizer.resize(source.data(), 23, 19, output.data(), 23, 19);
    assert(source == output);
}

void test_parallel_worker_local_resizers_are_stable() {
    const std::vector<uint8_t> source = make_test_image(211, 157);
    std::vector<std::vector<uint8_t>> outputs(8, std::vector<uint8_t>(332 * 332 * 3));
    std::vector<std::thread> threads;
    threads.reserve(outputs.size());
    for (size_t index = 0; index < outputs.size(); ++index) {
        threads.emplace_back([&, index] {
            RgbImageResizer resizer(1);
            resizer.resize(source.data(), 211, 157, outputs[index].data(), 332, 332);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (size_t index = 1; index < outputs.size(); ++index) {
        assert(outputs[index] == outputs[0]);
    }
}

} // namespace

int main() {
    test_resize_plan_respects_budget();
    test_single_thread_resize_is_stable();
    test_resizer_rejects_internal_threading();
    test_same_size_copy_preserves_pixels();
    test_parallel_worker_local_resizers_are_stable();
    return 0;
}
