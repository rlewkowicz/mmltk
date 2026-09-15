#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <future>
#include <optional>
#include "async_test_utils.hpp"
#include "filesystem_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "src/backend/data/compiled_format.h"
#include "src/backend/data/compiled_dataset.h"
#include "src/backend/data/compiled_image_stream.h"
#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/data/dataset_loader.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "test_fixture.h"
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
#include "src/common/system/cpu_affinity.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/gdr_mapped_buffer.h"

namespace fs = std::filesystem;
using namespace mmltk::backend::data;
using namespace mmltk::common::concurrency;
using namespace mmltk::common::logging;
using namespace mmltk::common::system;
using mmltk::frameworks::gpu::ensure_cuda_ok;
using namespace mmltk::backend::data::testsupport;

void exercise_compiled_stream(const std::string& path, bool h2d) {
    const auto source = CompiledDataset::open(path);
    const auto stride = static_cast<std::size_t>(source.header().image_stride);
    REQUIRE(source.label_index().size() == source.image_entries().size());
    REQUIRE(source.image_pixels(0) == source.pixel_blob());
    DatasetLoader::Config training_config;
    training_config.compiled_path = path;
    training_config.batch_size = 3U;
    training_config.prefetch_factor = 2;
    training_config.loading.h2d_dataloader = h2d;
    DatasetLoader training(training_config);
    training.begin_epoch();
    // Model construction precedes the isolated runtime context. The stream
    // must capture the later context and bind it on its completion/I/O workers.
    std::optional<mmltk::frameworks::gpu::DeviceContext> context;
    // Callback facts and receiver memory outlive stream teardown on assertions.
    std::vector<std::byte> received(2U * stride);
    struct Completion {
        std::atomic<bool> done = false;
        std::exception_ptr failure;
    } completion;
    CompiledImageStream stream({.slots = 1U, .workers = 1U, .device = 0, .loading = data_loading_options(h2d)});
    context.emplace(0, mmltk::frameworks::gpu::cuda_image_copy_backend());
    context->Bind();
    stream.bind_current_context();
    stream.prepare_images(0, 2U * stride);
    CHECK(stream.host_storage(0).owns_allocation() == h2d);
    const auto* host = stream.host_storage(0).data();

    const std::array reads{CompiledImageRead{3U, 0U}, CompiledImageRead{1U, stride}};
    stream.submit(0U, source, reads, {});
    REQUIRE(stream.wait_read(0U));
    host = stream.host_images(0U).data();
    const auto* device = stream.device_storage(0U).data();
    REQUIRE(std::memcmp(host, source.image_pixels(3U), stride) == 0);
    REQUIRE(std::memcmp(static_cast<const std::byte*>(host) + stride, source.image_pixels(1U), stride) == 0);
    stream.handoff(0U, nullptr);
    ensure_cuda_ok(cudaMemcpyAsync(received.data(), device, received.size(), cudaMemcpyDeviceToHost, nullptr),
                   "compiled stream consumer read");
    const CompiledImageStream::CompletionObserver completed{.context = &completion,
                                                            .complete = [](void* raw, std::size_t, std::exception_ptr error) noexcept {
                                                                auto& state = *static_cast<Completion*>(raw);
                                                                state.failure = error;
                                                                state.done.store(true, std::memory_order_release);
                                                            }};
    stream.release(0U, nullptr, completed);
    stream.synchronize();
    REQUIRE(completion.done.load(std::memory_order_acquire));
    REQUIRE_FALSE(completion.failure);
    REQUIRE(std::memcmp(received.data(), host, received.size()) == 0);
    Batch training_batch{};
    std::size_t training_images = 0U;
    while (training.next_batch(training_batch)) {
        training.wait_batch(training_batch);
        training_images += training_batch.num_images;
        training.release_batch(training_batch);
    }
    CHECK(training_images == source.header().num_images);
    training.synchronize();
    stream.prepare_host(0U, stride);
    stream.prepare_device(0U, stride);
    REQUIRE(stream.host_storage(0U).data() == host);
    REQUIRE(stream.device_storage(0U).data() == device);
    for (std::size_t iteration = 0U; iteration < 8U; ++iteration) {
        stream.submit(0U, source, reads, {});
        REQUIRE(stream.wait_read(0U));
        stream.handoff(0U, nullptr);
        stream.release(0U, nullptr, {});
        stream.synchronize();
        CHECK(stream.host_storage(0U).data() == host);
        CHECK(stream.device_storage(0U).data() == device);
        host = stream.host_images(0U).data();
        CHECK(std::memcmp(host, source.image_pixels(3U), stride) == 0);
    }

    const std::array invalid{CompiledImageRead{source.header().num_images, 0U}};
    stream.submit(0U, source, invalid, {});
    REQUIRE_THROWS_AS(stream.wait_read(0U), std::out_of_range);
    stream.synchronize();
    {
        mmltk::testsupport::TestGate gate("compiled stream physical read");
        auto receipt = gate.receipt();
        std::future<bool> read_result;
        const mmltk::testsupport::ScopedTestCleanup release_read([&] {
            stream.cancel_read(0U);
            gate.Release();
            stream.wait_reads();
        });
        stream.submit(0U, source, reads, {.context = &receipt, .before = [](void* raw, std::size_t) {
                                              static_cast<mmltk::testsupport::TestGate::Receipt*>(raw)->ArriveAndWait();
                                              return true;
                                          }});
        read_result = std::async(std::launch::async, [&] { return stream.wait_read(0U); });
        REQUIRE(gate.WaitEntered(std::chrono::seconds{2}));
        // A checked-out physical read job cannot be replaced while its worker owns it.
        CHECK_THROWS_AS(stream.submit(0U, source, reads, {}), std::logic_error);
        stream.cancel_read(0U);
        gate.Release();
        REQUIRE_FALSE(mmltk::testsupport::await_test_future(read_result, "cancelled compiled read settlement"));
        stream.synchronize();
    }
    // The cancelled worker has begun its mapped write but never published an
    // input lease. Retained-cache work still needs the lane's completion event.
    if (!h2d) REQUIRE(stream.device_storage(0U).data() == nullptr);
    completion.done.store(false, std::memory_order_release);
    stream.fence(0U, nullptr, completed);
    stream.synchronize();
    REQUIRE(completion.done.load(std::memory_order_acquire));
    REQUIRE_FALSE(completion.failure);
    stream.submit(0U, source, reads, {});
    REQUIRE(stream.wait_read(0U));
    stream.handoff(0U, nullptr);
    ensure_cuda_ok(cudaMemcpyAsync(received.data(), device, received.size(), cudaMemcpyDeviceToHost, nullptr),
                   "compiled stream reused consumer read");
    completion.done.store(false, std::memory_order_release);
    stream.release(0U, nullptr, completed);
    stream.synchronize();
    REQUIRE(completion.done.load(std::memory_order_acquire));
    REQUIRE_FALSE(completion.failure);
    CHECK(std::memcmp(received.data(), source.image_pixels(3U), stride) == 0);
    CHECK(std::memcmp(received.data() + stride, source.image_pixels(1U), stride) == 0);
    REQUIRE(stream.reset_storage() == cudaSuccess);
    REQUIRE_FALSE(stream.owns_allocation());
    stream.close();
    REQUIRE_FALSE(stream.owns_resources());
}

void exercise_roundtrip_transport(const FixtureSpec& fixture, const bool h2d, cudaStream_t compute_stream) {
    const std::string bin_path = compiled_bin_path(fixture);
    const std::string dataset_dir_path = dataset_dir(fixture);
    const std::string& split = fixture.split;
    const int W = fixture.width;
    const int H = fixture.height;
    const int NUM_IMAGES = fixture.num_images;
    const size_t IMAGE_STRIDE = static_cast<size_t>(3) * H * W * sizeof(float);
    const size_t STRIDE_FLOATS = IMAGE_STRIDE / sizeof(float);
    INFO("h2d_dataloader=" << h2d);
    exercise_compiled_stream(bin_path, h2d);
    ensure_cuda_ok(cudaSetDevice(0), "restore primary training context after isolated stream coverage");
    {
        DatasetLoader::Config direct_cfg;
        direct_cfg.loading.h2d_dataloader = h2d;
        direct_cfg.compiled_path = bin_path;
        direct_cfg.batch_size = 1;
        direct_cfg.shuffle = false;
        DatasetLoader direct_loader(direct_cfg);
        REQUIRE(direct_loader.num_images() == static_cast<size_t>(NUM_IMAGES));
        REQUIRE(direct_loader.num_classes() == 6u);
    }

    DatasetLoader::Config direct_cfg;
    direct_cfg.loading.h2d_dataloader = h2d;
    direct_cfg.compiled_path = bin_path;
    direct_cfg.batch_size = 8;
    direct_cfg.shuffle = false;
    direct_cfg.prefetch_factor = 1;

    DatasetLoader direct_loader(direct_cfg);
    REQUIRE(direct_loader.num_images() == static_cast<size_t>(NUM_IMAGES));
    REQUIRE(direct_loader.image_width() == static_cast<uint32_t>(W));
    REQUIRE(direct_loader.image_height() == static_cast<uint32_t>(H));
    REQUIRE(direct_loader.num_classes() == 6u);
    REQUIRE(direct_loader.image_stride() == IMAGE_STRIDE);

    const fs::path first_image_path = fs::path(dataset_dir_path) / split / "000001.png";
    const std::vector<float> expected_first_image = expected_nchw_stub(first_image_path.string(), W, H);
    assert_image_matches(direct_loader.pixel_blob(), expected_first_image);

    for (size_t i = 0; i < 100; ++i) {
        const float pixel = direct_loader.pixel_blob()[i];
        REQUIRE((pixel >= 0.0f && pixel <= 1.0f));
    }
    printf("Pixel values in [0,1] — OK\n");

    direct_loader.begin_epoch();
    Batch batch;
    size_t total = 0;
    size_t total_instances = 0;
    size_t next_expected_idx = 0;
    while (direct_loader.next_batch(batch)) {
        REQUIRE(batch.image_indices[0] == next_expected_idx);
        REQUIRE(direct_loader.host_images(batch).data() == direct_loader.pixel_blob() + next_expected_idx * STRIDE_FLOATS);
        total += batch.num_images;

        for (size_t i = 0; i < batch.num_images; ++i) {
            const uint32_t idx = batch.image_indices[i];
            const auto& entry = batch.label_index[idx];
            total_instances += entry.num_instances;
            REQUIRE(idx == batch.image_indices[0] + i);

            for (int p = 0; p < 10; ++p) {
                const float pixel = direct_loader.host_images(batch).data()[i * STRIDE_FLOATS + static_cast<size_t>(p)];
                REQUIRE((pixel >= 0.0f && pixel <= 1.0f));
            }

            REQUIRE(entry.num_instances == (idx >= 10 ? 1 : 0));
            if (entry.num_instances > 0) {
                const PackedInstance& inst = batch.labels[entry.label_begin];
                REQUIRE(inst.class_id == static_cast<uint8_t>((idx - 10) % direct_loader.num_classes()));
                REQUIRE(inst.mask_rle_pairs == static_cast<uint16_t>(std::min(H - 1, 30) - 10));
                const RLEPair& first_pair = batch.rle_pairs[inst.mask_rle_offset / sizeof(RLEPair)];
                REQUIRE(first_pair.start == static_cast<uint32_t>(10 * W + 10));
                REQUIRE(first_pair.length == static_cast<uint32_t>(std::min(W - 1, 30) - 10));
            }
        }
        direct_loader.handoff_batch(batch, compute_stream);
        ensure_cuda_ok(cudaMemsetAsync(const_cast<float*>(batch.device_images), 0, batch.num_images * IMAGE_STRIDE, compute_stream),
                       "cudaMemsetAsync");
        direct_loader.release_batch(batch, compute_stream);
        next_expected_idx += batch.num_images;
    }
    ensure_cuda_ok(cudaStreamSynchronize(compute_stream), "cudaStreamSynchronize");
    direct_loader.synchronize();
    REQUIRE(total == static_cast<size_t>(NUM_IMAGES));
    REQUIRE(total_instances == static_cast<size_t>(std::max(NUM_IMAGES - 10, 0)));
    printf("Sequential iteration: %zu images, %zu instances\n", total, total_instances);

    direct_loader.begin_epoch();
    total = 0;
    while (direct_loader.next_batch(batch)) {
        direct_loader.wait_batch(batch);
        direct_loader.release_batch(batch);
        total += batch.num_images;
    }
    direct_loader.synchronize();
    REQUIRE(total == static_cast<size_t>(NUM_IMAGES));

    direct_loader.begin_epoch();
    if (direct_loader.next_batch(batch)) {
        direct_loader.wait_batch(batch);
        direct_loader.handoff_batch(batch, nullptr);
        ensure_cuda_ok(cudaMemsetAsync(const_cast<float*>(batch.device_images), 0, batch.num_images * IMAGE_STRIDE, nullptr),
                       "cudaMemsetAsync");
        direct_loader.release_batch(batch, nullptr);
    }
    while (direct_loader.next_batch(batch)) {
        direct_loader.release_batch(batch);
    }
    ensure_cuda_ok(cudaStreamSynchronize(nullptr), "cudaStreamSynchronize");
    direct_loader.synchronize();

    DatasetLoader::Config shuffled_cfg = direct_cfg;
    shuffled_cfg.shuffle = true;
    shuffled_cfg.seed = 7;
    shuffled_cfg.prefetch_factor = 5;

    DatasetLoader shuffled_loader(shuffled_cfg);
    shuffled_loader.begin_epoch();
    std::vector<uint32_t> seen_indices;
    total = 0;
    while (shuffled_loader.next_batch(batch)) {
        CHECK_THROWS_AS(direct_loader.host_images(batch), std::runtime_error);
        total += batch.num_images;
        seen_indices.insert(seen_indices.end(), batch.image_indices, batch.image_indices + batch.num_images);
        const bool direct_shuffle_span =
            shuffled_loader.host_images(batch).data() == shuffled_loader.pixel_blob() + batch.image_indices[0] * STRIDE_FLOATS;
        if (direct_shuffle_span) {
            for (size_t i = 0; i < batch.num_images; ++i) {
                REQUIRE(batch.image_indices[i] == batch.image_indices[0] + i);
            }
        }
        shuffled_loader.handoff_batch(batch, compute_stream);
        ensure_cuda_ok(cudaMemsetAsync(const_cast<float*>(batch.device_images), 0, batch.num_images * IMAGE_STRIDE, compute_stream),
                       "cudaMemsetAsync");
        shuffled_loader.release_batch(batch, compute_stream);
        CHECK_THROWS_AS(shuffled_loader.host_images(batch), std::runtime_error);
    }
    ensure_cuda_ok(cudaStreamSynchronize(compute_stream), "cudaStreamSynchronize");
    shuffled_loader.synchronize();
    REQUIRE(total == static_cast<size_t>(NUM_IMAGES));
    std::sort(seen_indices.begin(), seen_indices.end());
    for (uint32_t i = 0; i < static_cast<uint32_t>(seen_indices.size()); ++i) {
        REQUIRE(seen_indices[i] == i);
    }
    printf("Shuffled iteration: %zu images with prefetch_factor=%d\n", total, shuffled_cfg.prefetch_factor);

    shuffled_loader.begin_epoch();
    seen_indices.clear();
    while (shuffled_loader.next_batch(batch)) {
        seen_indices.insert(seen_indices.end(), batch.image_indices, batch.image_indices + batch.num_images);
        shuffled_loader.release_batch(batch);
    }
    shuffled_loader.synchronize();
    std::sort(seen_indices.begin(), seen_indices.end());
    for (uint32_t i = 0; i < static_cast<uint32_t>(seen_indices.size()); ++i) {
        REQUIRE(seen_indices[i] == i);
    }

    const std::vector<int> cpu_set = allowed_cpu_set();
    DatasetLoader::Config budgeted_cfg = shuffled_cfg;
    budgeted_cfg.prefetch_factor = 2;
    budgeted_cfg.gather_workers = 1;
    budgeted_cfg.cpu_affinity = std::to_string(cpu_set.front());

    DatasetLoader budgeted_loader(budgeted_cfg);
    std::vector<const float*> persistent_device_slots(static_cast<std::size_t>(budgeted_cfg.prefetch_factor));
    std::vector<std::uint64_t> previous_leases(persistent_device_slots.size());
    for (int epoch = 0; epoch < 3; ++epoch) {
        budgeted_loader.begin_epoch();
        total = 0;
        while (budgeted_loader.next_batch(batch)) {
            auto& device = persistent_device_slots[batch.slot_index];
            if (device == nullptr) device = batch.device_images;
            CHECK(batch.device_images == device);
            CHECK(batch.lease_id > previous_leases[batch.slot_index]);
            previous_leases[batch.slot_index] = batch.lease_id;
            total += batch.num_images;
            if ((batch.lease_id & 1U) != 0U) {
                budgeted_loader.handoff_batch(batch, compute_stream);
                budgeted_loader.release_batch(batch, compute_stream);
            } else {
                budgeted_loader.release_batch(batch);
            }
        }
        budgeted_loader.synchronize();
        REQUIRE(total == static_cast<size_t>(NUM_IMAGES));
    }

    DatasetLoader::Config invalid_budget_cfg = shuffled_cfg;
    invalid_budget_cfg.prefetch_factor = 2;
    invalid_budget_cfg.gather_workers = 3;
    bool invalid_budget_threw = false;
    try {
        DatasetLoader invalid_budget_loader(invalid_budget_cfg);
    } catch (const std::invalid_argument&) { invalid_budget_threw = true; }
    REQUIRE(invalid_budget_threw);

    DatasetLoader::Config invalid_affinity_cfg = direct_cfg;
    invalid_affinity_cfg.cpu_affinity = "999999";
    bool invalid_affinity_threw = false;
    try {
        DatasetLoader invalid_affinity_loader(invalid_affinity_cfg);
    } catch (const std::runtime_error&) { invalid_affinity_threw = true; }
    REQUIRE(invalid_affinity_threw);

    DatasetLoader::Config shard0_cfg = direct_cfg;
    shard0_cfg.batch_size = 4;
    shard0_cfg.batch_shard_rank = 0;
    shard0_cfg.batch_shard_count = 2;
    DatasetLoader::Config shard1_cfg = shard0_cfg;
    shard1_cfg.batch_shard_rank = 1;

    DatasetLoader shard0_loader(shard0_cfg);
    DatasetLoader shard1_loader(shard1_cfg);
    shard0_loader.begin_epoch();
    shard1_loader.begin_epoch();

    std::vector<uint32_t> sharded_seen;
    while (shard0_loader.next_batch(batch)) {
        sharded_seen.insert(sharded_seen.end(), batch.image_indices, batch.image_indices + batch.num_images);
        shard0_loader.release_batch(batch);
    }
    while (shard1_loader.next_batch(batch)) {
        sharded_seen.insert(sharded_seen.end(), batch.image_indices, batch.image_indices + batch.num_images);
        shard1_loader.release_batch(batch);
    }
    shard0_loader.synchronize();
    shard1_loader.synchronize();
    std::sort(sharded_seen.begin(), sharded_seen.end());
    REQUIRE(sharded_seen.size() == static_cast<size_t>(NUM_IMAGES));
    for (uint32_t i = 0; i < static_cast<uint32_t>(sharded_seen.size()); ++i) {
        REQUIRE(sharded_seen[i] == i);
    }
    printf("Batch sharding: %zu images across %u shards\n", sharded_seen.size(), shard0_cfg.batch_shard_count);

    DatasetLoader::Config partial_cfg = direct_cfg;
    partial_cfg.batch_size = 7U;
    DatasetLoader partial_loader(partial_cfg);
    std::vector<std::size_t> batch_sizes;
    while (partial_loader.next_batch(batch)) {
        batch_sizes.push_back(batch.num_images);
        REQUIRE_THROWS(partial_loader.begin_epoch());
        partial_loader.release_batch(batch);
        REQUIRE_THROWS(partial_loader.release_batch(batch));
    }
    partial_loader.synchronize();
    REQUIRE(batch_sizes == std::vector<std::size_t>{7U, 7U, 6U});
    partial_cfg.drop_last = true;
    DatasetLoader dropped_loader(partial_cfg);
    REQUIRE(dropped_loader.num_batches() == 2U);
    total = 0U;
    while (dropped_loader.next_batch(batch)) {
        total += batch.num_images;
        dropped_loader.release_batch(batch);
    }
    REQUIRE(total == 14U);
    dropped_loader.synchronize();
    DatasetLoader same_seed_a(shuffled_cfg);
    DatasetLoader same_seed_b(shuffled_cfg);
    for (int epoch = 0; epoch < 2; ++epoch) {
        same_seed_a.begin_epoch();
        same_seed_b.begin_epoch();
        Batch other{};
        while (same_seed_a.next_batch(batch)) {
            REQUIRE(same_seed_b.next_batch(other));
            REQUIRE(batch.num_images == other.num_images);
            REQUIRE(std::equal(batch.image_indices, batch.image_indices + batch.num_images, other.image_indices));
            same_seed_a.release_batch(batch);
            same_seed_b.release_batch(other);
        }
        REQUIRE_FALSE(same_seed_b.next_batch(other));
    }
    // Replacing an unconsumed epoch cancels bounded reads and starts from the
    // next deterministic schedule after all physical transfers settle.
    same_seed_a.begin_epoch();
    same_seed_a.begin_epoch();
    while (same_seed_a.next_batch(batch))
        same_seed_a.release_batch(batch);
    same_seed_a.synchronize();
    DatasetLoader stopped(direct_cfg);
    stopped.begin_epoch();
    Batch retained{};
    REQUIRE(stopped.next_batch(retained));
    stopped.wait_batch(retained);
    stopped.stop_workers();
    stopped.stop_workers();
    REQUIRE(retained.device_images != nullptr);
    float first_channel = -1.0F;
    REQUIRE(cudaMemcpy(&first_channel, retained.device_images, sizeof(float), cudaMemcpyDeviceToHost) == cudaSuccess);
    REQUIRE(std::isfinite(first_channel));
    REQUIRE_FALSE(stopped.next_batch(batch));
    REQUIRE_THROWS(stopped.begin_epoch());
    stopped.release_batch(retained, compute_stream);
    DatasetLoader cancellable(direct_cfg);
    cancellable.begin_epoch();
    Batch occupied{};
    REQUIRE(cancellable.next_batch(occupied));
    cancellable.wait_batch(occupied);
    // One prefetch slot remains checked out: readiness cannot complete this wait.
    std::stop_source cancellation;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    auto waiting = std::async(std::launch::async, [&] {
        entered.set_value();
        Batch unused{};
        return cancellable.next_batch(unused, cancellation.get_token());
    });
    mmltk::testsupport::await_test_future(entered_future, "compiled acquisition entered");
    CHECK(cancellation.request_stop());
    CHECK_FALSE(cancellation.request_stop());
    CHECK_FALSE(mmltk::testsupport::await_test_future(waiting, "cancelled batch acquisition"));
    // Stop neither releases nor joins on the requester; the owner still owns this lease.
    cancellable.release_batch(occupied, compute_stream);
    Batch after_stop{};
    CHECK_FALSE(cancellable.next_batch(after_stop, cancellation.get_token()));
    // Cancellation is scoped to one acquisition, not the ordinary training loader.
    REQUIRE(cancellable.next_batch(after_stop));
    cancellable.wait_batch(after_stop);
    cancellable.release_batch(after_stop, compute_stream);
    cancellable.begin_epoch();
    std::stop_source checkout_stop;
    REQUIRE(cancellable.next_batch(after_stop, checkout_stop.get_token()));
    CHECK(checkout_stop.request_stop());
    cancellable.wait_batch(after_stop);
    cancellable.release_batch(after_stop, compute_stream);
    CHECK_FALSE(cancellable.next_batch(after_stop, checkout_stop.get_token()));
    cancellable.begin_epoch();
    std::stop_source readiness_stop;
    auto stopper = std::async(std::launch::async, [&] { return readiness_stop.request_stop(); });
    const bool checked_out = cancellable.next_batch(after_stop, readiness_stop.get_token());
    CHECK(mmltk::testsupport::await_test_future(stopper, "stop racing batch readiness"));
    if (checked_out) {
        cancellable.wait_batch(after_stop);
        cancellable.release_batch(after_stop, compute_stream);
    }
    CHECK_FALSE(cancellable.next_batch(after_stop, readiness_stop.get_token()));
    DatasetLoader never_started(direct_cfg);
    never_started.stop_workers();
    REQUIRE_FALSE(never_started.next_batch(batch));
    REQUIRE_THROWS(never_started.begin_epoch());
}

void test_roundtrip_end_to_end() {
    profile_set_run_label("test_roundtrip");
    const mmltk::testsupport::ScopedTempDir root("mmltk_roundtrip");
    const FixtureSpec fixture{root.path().string(), "train", 65, 65, 20};
    const std::string dataset_dir_path = dataset_dir(fixture);
    const std::string compiled_dir_path = compiled_dir(fixture);
    const std::string split = fixture.split;
    const int W = fixture.width;
    const int H = fixture.height;
    const int NUM_IMAGES = fixture.num_images;
    ensure_cuda_ok(cudaSetDevice(0), "cudaSetDevice");
    const mmltk::frameworks::gpu::DeviceContext context(0, mmltk::frameworks::gpu::cuda_image_copy_backend(),
                                                        mmltk::frameworks::gpu::DeviceContextMode::PrimaryInterop);
    mmltk::frameworks::gpu::ImageStream owned_stream(context);
    const auto compute_stream = reinterpret_cast<cudaStream_t>(owned_stream.native_handle());

    create_synthetic_dataset(fixture);

    printf("=== Test dataset: %d images at %dx%d ===\n", NUM_IMAGES, W, H);

    CompilerConfig ccfg;
    ccfg.source_dir = dataset_dir_path;
    ccfg.output_dir = compiled_dir_path;
    ccfg.split = split;
    ccfg.target_width = W;
    ccfg.target_height = H;
    ccfg.num_workers = 2;
    const DatasetCompilePlan compile_plan = DatasetCompiler::prepare(ccfg, {ccfg.split});
    DatasetCompiler::compile(compile_plan, 0U);
    printf("=== Compiled ===\n");

    // Resized compilation, canonical progress, letterboxing, and tiny masks
    // are covered by compile_progress; all transport runs share this artifact.
    const std::string bin_path = compiled_bin_path(fixture);
    const bool bin_exists = fs::exists(bin_path);
    REQUIRE(bin_exists);
    printf("Compiled file: %zu bytes\n", static_cast<size_t>(fs::file_size(bin_path)));
    // Run every H2D assertion before probing the optional GDR transport.
    for (const bool h2d : {true, false}) {
        try {
            exercise_roundtrip_transport(fixture, h2d, compute_stream);
        } catch (const mmltk::frameworks::gpu::GdrTransportUnavailable& error) {
            if (h2d) throw;
            SKIP("GDR hardware unavailable after complete H2D coverage: " << error.what());
        }
    }
    printf("=== ALL TESTS PASSED ===\n");
}

TEST_CASE("compiled dataset round trip", "[backend][data][roundtrip][cuda]") { test_roundtrip_end_to_end(); }

TEST_CASE("compiled source teardown retains one durable authority across replacement", "[data][gpu][custody]") {
    namespace gpu = mmltk::frameworks::gpu;
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    REQUIRE(cudaFree(nullptr) == cudaSuccess);
    auto authority = std::make_shared<gpu::TerminalCudaRetirementOwner>(1U);
    CompiledImageStream::Config config{.slots = 1U, .workers = 1U, .device = 0, .loading = data_loading_options(true)};
    config.settle = +[](cudaStream_t stream) -> cudaError_t {
        const auto status = cudaStreamSynchronize(stream);
        return status == cudaSuccess ? cudaErrorUnknown : status;
    };
    {
        CompiledImageStream source(config, authority);
        source.bind_current_context();
        source.prepare_device(0U, 64U);
        source.stop_workers();
        source.stop_workers();
    }
    CHECK_FALSE(authority->admission_open());
    CHECK(authority->fact().occupancy == 1U);
    CHECK(authority->fact().reservations == 0U);
    for (unsigned attempt = 0U; attempt < 4U; ++attempt) CHECK_THROWS(CompiledImageStream(config, authority));
    CHECK(authority->fact().occupancy == 1U);
}
