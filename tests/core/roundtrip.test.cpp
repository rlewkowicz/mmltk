#include "cuda_test_utils.h"
#include "dataset_compiler.h"
#include "dataset_loader.h"
#include "profile_utils.h"
#include "test_fixture.h"
#include "worker_pool.h"

#include <algorithm>
#include "support/catch2_compat.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace mmltk;
using namespace mmltk::testsupport;

struct TestOptions {
    std::string test_dir = "/tmp/mmltk_test";
    bool keep_artifacts = false;
    int width = 65;
    int height = 65;
    int num_images = 20;
};

void assert_files_equal(const fs::path& lhs_path, const fs::path& rhs_path) {
    assert(fs::file_size(lhs_path) == fs::file_size(rhs_path));
    std::ifstream lhs(lhs_path, std::ios::binary);
    std::ifstream rhs(rhs_path, std::ios::binary);
    assert(lhs.is_open());
    assert(rhs.is_open());

    std::vector<char> lhs_buffer(1 << 16);
    std::vector<char> rhs_buffer(1 << 16);
    while (lhs && rhs) {
        lhs.read(lhs_buffer.data(), static_cast<std::streamsize>(lhs_buffer.size()));
        rhs.read(rhs_buffer.data(), static_cast<std::streamsize>(rhs_buffer.size()));
        const std::streamsize lhs_read = lhs.gcount();
        const std::streamsize rhs_read = rhs.gcount();
        assert(lhs_read == rhs_read);
        assert(std::memcmp(lhs_buffer.data(), rhs_buffer.data(), static_cast<size_t>(lhs_read)) == 0);
        if (lhs_read == 0) {
            break;
        }
    }
}

void test_roundtrip_end_to_end() {
    MMLTK_PROFILE_RUN_LABEL("test_roundtrip");
    const TestOptions opts;
    const FixtureSpec fixture{
        opts.test_dir,
        "train",
        opts.width,
        opts.height,
        opts.num_images,
    };
    const std::string test_dir = fixture.root_dir;
    const std::string dataset_dir_path = dataset_dir(fixture);
    const std::string compiled_dir_path = compiled_dir(fixture);
    const std::string split = fixture.split;
    const int W = fixture.width;
    const int H = fixture.height;
    const int NUM_IMAGES = fixture.num_images;
    const size_t IMAGE_STRIDE = static_cast<size_t>(3) * H * W * sizeof(float);
    const size_t STRIDE_FLOATS = IMAGE_STRIDE / sizeof(float);
    cudaStream_t compute_stream = nullptr;

    CUDA_ASSERT_OK(cudaSetDevice(0));
    CUDA_ASSERT_OK(cudaStreamCreateWithFlags(&compute_stream, cudaStreamNonBlocking));

    create_synthetic_dataset(fixture);

    printf("=== Test dataset: %d images at %dx%d ===\n", NUM_IMAGES, W, H);

    CompilerConfig ccfg;
    ccfg.source_dir = dataset_dir_path;
    ccfg.output_dir = compiled_dir_path;
    ccfg.split = split;
    ccfg.target_width = W;
    ccfg.target_height = H;
    ccfg.num_workers = 2;
    DatasetCompiler::compile(ccfg);
    printf("=== Compiled ===\n");

    CompilerConfig resized_cpu_cfg = ccfg;
    resized_cpu_cfg.output_dir = test_dir + "/compiled_resized_cpu";
    resized_cpu_cfg.target_width = std::max(32, W - 13);
    resized_cpu_cfg.target_height = std::max(32, H - 9);
    std::vector<size_t> resized_progress_steps;
    DatasetCompiler::compile(resized_cpu_cfg, [&](const CompileProgress& progress) {
        resized_progress_steps.push_back(progress.done);
    });
    const bool saw_intermediate_pixel_progress =
        std::any_of(resized_progress_steps.begin(),
                    resized_progress_steps.end(),
                    [&](size_t done) { return done > static_cast<size_t>(NUM_IMAGES) &&
                                              done < (static_cast<size_t>(NUM_IMAGES) * 2U); });
    assert(saw_intermediate_pixel_progress);

    const std::string bin_path = compiled_bin_path(fixture);
    const bool bin_exists = fs::exists(bin_path);
    assert(bin_exists);
    printf("Compiled file: %zu bytes\n", static_cast<size_t>(fs::file_size(bin_path)));
    {
        DatasetLoader::Config direct_cfg;
        direct_cfg.compiled_path = bin_path;
        direct_cfg.batch_size = 1;
        direct_cfg.shuffle = false;
        DatasetLoader direct_loader(direct_cfg);
        assert(direct_loader.num_images() == static_cast<size_t>(NUM_IMAGES));
        assert(direct_loader.num_classes() == 6u);
    }

    DatasetLoader::Config direct_cfg;
    direct_cfg.compiled_path = bin_path;
    direct_cfg.batch_size = 8;
    direct_cfg.shuffle = false;
    direct_cfg.prefetch_factor = 1;

    DatasetLoader direct_loader(direct_cfg);
    assert(direct_loader.num_images() == static_cast<size_t>(NUM_IMAGES));
    assert(direct_loader.image_width() == static_cast<uint32_t>(W));
    assert(direct_loader.image_height() == static_cast<uint32_t>(H));
    assert(direct_loader.num_classes() == 6u);
    assert(direct_loader.image_stride() == IMAGE_STRIDE);

    const fs::path first_image_path = fs::path(dataset_dir_path) / split / "000001.png";
    const std::vector<float> expected_first_image =
        expected_nchw_stub(first_image_path.string(), W, H);
    assert_image_matches(direct_loader.pixel_blob(), expected_first_image);

    for (size_t i = 0; i < 100; ++i) {
        const float pixel = direct_loader.pixel_blob()[i];
        assert(pixel >= 0.0f && pixel <= 1.0f);
    }
    printf("Pixel values in [0,1] — OK\n");

    direct_loader.begin_epoch();
    Batch batch;
    size_t total = 0;
    size_t total_instances = 0;
    size_t next_expected_idx = 0;
    while (direct_loader.next_batch(batch)) {
        assert(batch.image_indices[0] == next_expected_idx);
        assert(batch.host_images == direct_loader.pixel_blob() + next_expected_idx * STRIDE_FLOATS);
        total += batch.num_images;

        for (size_t i = 0; i < batch.num_images; ++i) {
            const uint32_t idx = batch.image_indices[i];
            const auto& entry = batch.label_index[idx];
            total_instances += entry.num_instances;
            assert(idx == batch.image_indices[0] + i);

            for (int p = 0; p < 10; ++p) {
                const float pixel = batch.host_images[i * STRIDE_FLOATS + static_cast<size_t>(p)];
                assert(pixel >= 0.0f && pixel <= 1.0f);
            }

            assert(entry.num_instances == (idx >= 10 ? 1 : 0));
            if (entry.num_instances > 0) {
                const PackedInstance& inst = batch.labels[entry.label_begin];
                assert(inst.class_id ==
                       static_cast<uint8_t>((idx - 10) % direct_loader.num_classes()));
                assert(inst.mask_rle_pairs == static_cast<uint16_t>(std::min(H - 1, 30) - 10));
                const RLEPair& first_pair =
                    batch.rle_pairs[inst.mask_rle_offset / sizeof(RLEPair)];
                assert(first_pair.start == static_cast<uint32_t>(10 * W + 10));
                assert(first_pair.length == static_cast<uint32_t>(std::min(W - 1, 30) - 10));
            }
        }
        direct_loader.handoff_batch(batch, compute_stream);
        CUDA_ASSERT_OK(cudaMemsetAsync(const_cast<float*>(batch.device_images),
                                       0,
                                       batch.num_images * IMAGE_STRIDE,
                                       compute_stream));
        direct_loader.release_batch(batch, compute_stream);
        next_expected_idx += batch.num_images;
    }
    CUDA_ASSERT_OK(cudaStreamSynchronize(compute_stream));
    direct_loader.cuda().sync();
    assert(total == static_cast<size_t>(NUM_IMAGES));
    assert(total_instances == static_cast<size_t>(std::max(NUM_IMAGES - 10, 0)));
    printf("Sequential iteration: %zu images, %zu instances\n", total, total_instances);

    direct_loader.begin_epoch();
    total = 0;
    while (direct_loader.next_batch(batch)) {
        direct_loader.wait_batch(batch);
        direct_loader.release_batch(batch);
        total += batch.num_images;
    }
    direct_loader.cuda().sync();
    assert(total == static_cast<size_t>(NUM_IMAGES));

    direct_loader.begin_epoch();
    if (direct_loader.next_batch(batch)) {
        direct_loader.wait_batch(batch);
        direct_loader.handoff_batch(batch, nullptr);
        CUDA_ASSERT_OK(cudaMemsetAsync(
            const_cast<float*>(batch.device_images), 0, batch.num_images * IMAGE_STRIDE, nullptr));
        direct_loader.release_batch(batch, nullptr);
    }
    while (direct_loader.next_batch(batch)) {
        direct_loader.release_batch(batch);
    }
    CUDA_ASSERT_OK(cudaStreamSynchronize(nullptr));
    direct_loader.cuda().sync();

    DatasetLoader::Config shuffled_cfg = direct_cfg;
    shuffled_cfg.shuffle = true;
    shuffled_cfg.seed = 7;
    shuffled_cfg.prefetch_factor = 5;

    DatasetLoader shuffled_loader(shuffled_cfg);
    shuffled_loader.begin_epoch();
    std::vector<uint32_t> seen_indices;
    total = 0;
    while (shuffled_loader.next_batch(batch)) {
        total += batch.num_images;
        seen_indices.insert(seen_indices.end(), batch.image_indices, batch.image_indices + batch.num_images);
        const bool direct_shuffle_span =
            batch.host_images == shuffled_loader.pixel_blob() + batch.image_indices[0] * STRIDE_FLOATS;
        if (direct_shuffle_span) {
            for (size_t i = 0; i < batch.num_images; ++i) {
                assert(batch.image_indices[i] == batch.image_indices[0] + i);
            }
        }
        shuffled_loader.handoff_batch(batch, compute_stream);
        CUDA_ASSERT_OK(cudaMemsetAsync(const_cast<float*>(batch.device_images),
                                       0,
                                       batch.num_images * IMAGE_STRIDE,
                                       compute_stream));
        shuffled_loader.release_batch(batch, compute_stream);
    }
    CUDA_ASSERT_OK(cudaStreamSynchronize(compute_stream));
    shuffled_loader.cuda().sync();
    assert(total == static_cast<size_t>(NUM_IMAGES));
    std::sort(seen_indices.begin(), seen_indices.end());
    for (uint32_t i = 0; i < static_cast<uint32_t>(seen_indices.size()); ++i) {
        assert(seen_indices[i] == i);
    }
    printf("Shuffled iteration: %zu images with prefetch_factor=%d\n", total, shuffled_cfg.prefetch_factor);

    shuffled_loader.begin_epoch();
    seen_indices.clear();
    while (shuffled_loader.next_batch(batch)) {
        seen_indices.insert(seen_indices.end(), batch.image_indices, batch.image_indices + batch.num_images);
        shuffled_loader.release_batch(batch);
    }
    shuffled_loader.cuda().sync();
    std::sort(seen_indices.begin(), seen_indices.end());
    for (uint32_t i = 0; i < static_cast<uint32_t>(seen_indices.size()); ++i) {
        assert(seen_indices[i] == i);
    }

    const std::vector<int> cpu_set = allowed_cpu_set();
    DatasetLoader::Config budgeted_cfg = shuffled_cfg;
    budgeted_cfg.prefetch_factor = 2;
    budgeted_cfg.gather_workers = 1;
    budgeted_cfg.cpu_affinity = std::to_string(cpu_set.front());

    DatasetLoader budgeted_loader(budgeted_cfg);
    for (int epoch = 0; epoch < 2; ++epoch) {
        budgeted_loader.begin_epoch();
        total = 0;
        while (budgeted_loader.next_batch(batch)) {
            total += batch.num_images;
            budgeted_loader.release_batch(batch);
        }
        budgeted_loader.cuda().sync();
        assert(total == static_cast<size_t>(NUM_IMAGES));
    }

    DatasetLoader::Config invalid_budget_cfg = shuffled_cfg;
    invalid_budget_cfg.prefetch_factor = 2;
    invalid_budget_cfg.gather_workers = 3;
    bool invalid_budget_threw = false;
    try {
        DatasetLoader invalid_budget_loader(invalid_budget_cfg);
    } catch (const std::invalid_argument&) {
        invalid_budget_threw = true;
    }
    assert(invalid_budget_threw);

    DatasetLoader::Config invalid_affinity_cfg = direct_cfg;
    invalid_affinity_cfg.cpu_affinity = "999999";
    bool invalid_affinity_threw = false;
    try {
        DatasetLoader invalid_affinity_loader(invalid_affinity_cfg);
    } catch (const std::runtime_error&) {
        invalid_affinity_threw = true;
    }
    assert(invalid_affinity_threw);

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
    shard0_loader.cuda().sync();
    shard1_loader.cuda().sync();
    std::sort(sharded_seen.begin(), sharded_seen.end());
    assert(sharded_seen.size() == static_cast<size_t>(NUM_IMAGES));
    for (uint32_t i = 0; i < static_cast<uint32_t>(sharded_seen.size()); ++i) {
        assert(sharded_seen[i] == i);
    }
    printf("Batch sharding: %zu images across %u shards\n",
           sharded_seen.size(),
           shard0_cfg.batch_shard_count);

    CUDA_ASSERT_OK(cudaStreamDestroy(compute_stream));

    if (!opts.keep_artifacts) {
        fs::remove_all(test_dir);
    }
    printf("=== ALL TESTS PASSED ===\n");
}

MMLTK_REGISTER_TEST_CASE("[core][roundtrip][cuda]", test_roundtrip_end_to_end);
