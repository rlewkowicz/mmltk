#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "async_test_utils.hpp"
#include "filesystem_test_utils.hpp"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/data/dataset_loader.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/io/file_memory.h"
#include "test_fixture.h"

using namespace mmltk::backend::data;
using mmltk::common::io::FileHandle;
using namespace mmltk::backend::data::testsupport;

namespace fs = std::filesystem;

namespace {

std::atomic<std::uint64_t> g_compile_elapsed_seconds{0U};
std::atomic<std::size_t> g_overlap_clock_calls{0U};
std::atomic<std::shared_ptr<const mmltk::testsupport::TestGate::Receipt>> g_overlap_reset;

CompileTelemetry::Clock::time_point compile_test_now() noexcept {
    return CompileTelemetry::Clock::time_point{std::chrono::seconds{g_compile_elapsed_seconds.load(std::memory_order_relaxed)}};
}

CompileTelemetry::Clock::time_point overlap_compile_test_now() noexcept {
    const std::uint64_t captured_seconds = g_compile_elapsed_seconds.load(std::memory_order_relaxed);
    if (g_overlap_clock_calls.fetch_add(1U, std::memory_order_relaxed) == 1U) {
        if (const auto receipt = g_overlap_reset.load()) receipt->ArriveAndWait();
    }
    return CompileTelemetry::Clock::time_point{std::chrono::seconds{captured_seconds}};
}

void expect_compile_failure(const FixtureSpec& fixture, const std::function<void(const fs::path&)>& mutate,
                            const std::string& expected_error, int target_width = -1, int target_height = -1) {
    create_synthetic_dataset(fixture);
    const fs::path annotation_path = fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl";
    mutate(annotation_path);

    CompilerConfig config;
    config.source_dir = dataset_dir(fixture);
    config.output_dir = fixture.root_dir + "/compiled";
    config.split = fixture.split;
    config.target_width = static_cast<uint32_t>(target_width > 0 ? target_width : fixture.width);
    config.target_height = static_cast<uint32_t>(target_height > 0 ? target_height : fixture.height);
    config.num_workers = 2;

    bool threw = false;
    try {
        const DatasetCompilePlan plan = DatasetCompiler::prepare(config, {config.split});
        DatasetCompiler::compile(plan, 0U);
    } catch (const std::runtime_error& error) {
        threw = true;
        REQUIRE(std::string(error.what()).find(expected_error) != std::string::npos);
    }
    REQUIRE(threw);
}

void overwrite_annotation(const fs::path& annotation_path, const std::string& record) {
    std::ofstream file(annotation_path, std::ios::trunc);
    REQUIRE(file.is_open());
    file << record << "\n";
}

void compile_resized_fixture(const FixtureSpec& fixture) {
    CompilerConfig config;
    config.source_dir = dataset_dir(fixture);
    config.output_dir = compiled_dir(fixture);
    config.split = fixture.split;
    config.target_width = 8;
    config.target_height = 8;
    config.num_workers = 2;

    const DatasetCompilePlan plan = DatasetCompiler::prepare(config, {config.split});
    DatasetCompiler::compile(plan, 0U);
}

[[nodiscard]] DatasetLoader::Config resized_fixture_loader_config(const FixtureSpec& fixture) {
    DatasetLoader::Config config;
    config.loading.h2d_dataloader = true;
    config.compiled_path = compiled_bin_path(fixture);
    config.batch_size = 1;
    config.shuffle = false;
    return config;
}

[[nodiscard]] DatasetCompilePlan prepare_cancellation_compile(const FixtureSpec& fixture) {
    create_synthetic_dataset(fixture);
    CompilerConfig config;
    config.source_dir = dataset_dir(fixture);
    config.output_dir = fixture.root_dir + "/compiled";
    config.split = fixture.split;
    config.target_width = 32U;
    config.target_height = 32U;
    return DatasetCompiler::prepare(config, {config.split});
}

void test_vanished_masks_are_omitted() {
    const mmltk::testsupport::ScopedTempDir root("mmltk_compile_drop_vanished_mask");
    const FixtureSpec fixture{
        root.path().string(), "train", 16, 16, 20,
    };
    create_synthetic_dataset(fixture);
    overwrite_annotation(
        fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl",
        R"({"class":"person","bbox_xyxy":[0,0,1,1],"mask_rle_encoding":"row_major_start_length","mask_rle":"0:1","image_size_wh":[16,16]})");

    compile_resized_fixture(fixture);
    DatasetLoader loader(resized_fixture_loader_config(fixture));
    REQUIRE(loader.num_label_instances() == 9);
    REQUIRE(loader.label_index()[10].num_instances == 0);
}

void test_partial_mask_vanish_keeps_instance() {
    const mmltk::testsupport::ScopedTempDir root("mmltk_compile_keep_partial_mask");
    const FixtureSpec fixture{
        root.path().string(), "train", 16, 16, 20,
    };
    create_synthetic_dataset(fixture);
    overwrite_annotation(
        fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl",
        R"({"class":"person","bbox_xyxy":[0,0,6,6],"mask_rle_encoding":"row_major_start_length","mask_rle":"0:1 17:1 85:1","image_size_wh":[16,16]})");

    compile_resized_fixture(fixture);
    DatasetLoader loader(resized_fixture_loader_config(fixture));
    REQUIRE(loader.num_label_instances() == 10);
    const LabelIndexEntry& entry = loader.label_index()[10];
    REQUIRE(entry.num_instances == 1);
    const PackedInstance& instance = loader.label_data()[entry.label_begin];
    REQUIRE(instance.mask_rle_pairs == 2);
    REQUIRE(instance.bbox_x1 == 0);
    REQUIRE(instance.bbox_y1 == 0);
    REQUIRE(instance.bbox_x2 == 3);
    REQUIRE(instance.bbox_y2 == 3);
}

void test_native_compile_letterboxes_pixels_boxes_and_masks() {
    const mmltk::testsupport::ScopedTempDir root("mmltk_compile_letterbox");
    const FixtureSpec fixture{
        root.path().string(), "train", 16, 8, 11,
    };
    create_synthetic_dataset(fixture);
    overwrite_annotation(
        fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl",
        R"({"class":"person","bbox_xyxy":[4,2,12,6],"mask_rle_encoding":"row_major_start_length","mask_rle":"36:8 52:8 68:8 84:8","image_size_wh":[16,8]})");

    compile_resized_fixture(fixture);
    DatasetLoader loader(resized_fixture_loader_config(fixture));
    REQUIRE(loader.image_width() == 8U);
    REQUIRE(loader.image_height() == 8U);

    const FileHandle compiled_file = FileHandle::open_readonly(compiled_bin_path(fixture));
    const FileHeader compiled_header = read_compiled_header(compiled_file);
    std::vector<ImageEntry> compiled_index(compiled_header.num_images);
    compiled_file.pread_all(compiled_index.data(), compiled_index.size() * sizeof(ImageEntry), compiled_header.index_offset);
    REQUIRE(compiled_index[0].original_width == 16U);
    REQUIRE(compiled_index[0].original_height == 8U);

    constexpr size_t row = 8U;
    constexpr size_t plane = row * row;
    const float* first_image = loader.pixel_blob();
    for (size_t channel = 0U; channel < 3U; ++channel) {
        REQUIRE(first_image[channel * plane] == 0.0F);
        REQUIRE(first_image[channel * plane + 7U * row] == 0.0F);
    }
    REQUIRE((first_image[2U * row] != 0.0F || first_image[plane + 2U * row] != 0.0F || first_image[plane * 2U + 2U * row] != 0.0F));

    const LabelIndexEntry& entry = loader.label_index()[10];
    REQUIRE(entry.num_instances == 1U);
    const PackedInstance& instance = loader.label_data()[entry.label_begin];
    REQUIRE(instance.bbox_x1 == 2);
    REQUIRE(instance.bbox_y1 == 3);
    REQUIRE(instance.bbox_x2 == 6);
    REQUIRE(instance.bbox_y2 == 5);
    REQUIRE(instance.mask_rle_pairs != 0U);

    std::vector<uint8_t> dense_mask(plane, uint8_t{0});
    const size_t first_pair = static_cast<size_t>(instance.mask_rle_offset) / sizeof(RLEPair);
    for (size_t pair_index = 0U; pair_index < instance.mask_rle_pairs; ++pair_index) {
        const RLEPair& pair = loader.rle_data()[first_pair + pair_index];
        REQUIRE(static_cast<size_t>(pair.start) + pair.length <= plane);
        std::fill(dense_mask.begin() + pair.start, dense_mask.begin() + pair.start + pair.length, uint8_t{1});
    }
    REQUIRE(std::ranges::count(dense_mask, uint8_t{1}) == 8);
    using MaskDifference = std::vector<uint8_t>::difference_type;
    constexpr MaskDifference row_offset = 8;
    REQUIRE(std::ranges::all_of(dense_mask.begin(), dense_mask.begin() + 3 * row_offset, [](const uint8_t value) { return value == 0U; }));
    REQUIRE(std::ranges::all_of(dense_mask.begin() + 5 * row_offset, dense_mask.end(), [](const uint8_t value) { return value == 0U; }));
}

void test_compiled_tiny_masks_keep_outer_pixel_edges() {
    const mmltk::testsupport::ScopedTempDir root("mmltk_compile_tiny_masks");
    const FixtureSpec fixture{root.path().string(), "train", 8, 4, 11};
    create_synthetic_dataset(fixture);
    const std::array source_runs{RLEPair{0, 1},  RLEPair{7, 1},  RLEPair{24, 1}, RLEPair{31, 1},
                                 RLEPair{11, 1}, RLEPair{11, 2}, RLEPair{10, 4}};
    std::ostringstream annotations;
    for (const auto run : source_runs) {
        annotations << R"({"class":"person","bbox_xyxy":[0,0,8,4],"mask_rle_encoding":"row_major_start_length","mask_rle":")" << run.start
                    << ':' << run.length << R"(","image_size_wh":[8,4]})" << '\n';
    }
    overwrite_annotation(fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl", annotations.str());
    compile_resized_fixture(fixture);
    DatasetLoader loader(resized_fixture_loader_config(fixture));
    const auto& entry = loader.label_index()[10];
    REQUIRE(entry.num_instances == source_runs.size());
    for (std::size_t index = 0; index < source_runs.size(); ++index) {
        const auto& instance = loader.label_data()[entry.label_begin + index];
        const auto source = source_runs[index];
        CHECK(instance.bbox_x1 == static_cast<int>(source.start % 8));
        CHECK(instance.bbox_y1 == static_cast<int>(source.start / 8 + 2));
        CHECK(instance.bbox_x2 == static_cast<int>(source.start % 8 + source.length));
        CHECK(instance.bbox_y2 == static_cast<int>(source.start / 8 + 3));
        REQUIRE(instance.mask_rle_pairs == 1);
        const auto& compiled = loader.rle_data()[instance.mask_rle_offset / sizeof(RLEPair)];
        CHECK(compiled.start == source.start + 16);
        CHECK(compiled.length == source.length);
    }
}

void test_invalid_annotations_fail_loud() {
    const mmltk::testsupport::ScopedTempDir invalid_json("mmltk_compile_invalid_json");
    const mmltk::testsupport::ScopedTempDir unknown_class("mmltk_compile_unknown_class");
    expect_compile_failure(
        FixtureSpec{
            invalid_json.path().string(),
            "train",
            65,
            65,
            20,
        },
        [](const fs::path& annotation_path) {
            std::ofstream file(annotation_path, std::ios::trunc);
            REQUIRE(file.is_open());
            file << "{invalid json}\n";
        },
        "invalid JSON annotation record");

    expect_compile_failure(
        FixtureSpec{
            unknown_class.path().string(),
            "train",
            65,
            65,
            20,
        },
        [](const fs::path& annotation_path) {
            std::ofstream file(annotation_path, std::ios::trunc);
            REQUIRE(file.is_open());
            file
                << R"({"class":"unknown","bbox_xyxy":[10,10,20,20],"mask_rle_encoding":"row_major_start_length","mask_rle":"660:10","image_size_wh":[65,65]})"
                << "\n";
        },
        "is not declared in categories.json");
}

void test_checked_progress_estimates() {
    CHECK(estimate_progress(0U, 100U, 10U) == ProgressEstimate{});
    CHECK(estimate_progress(10U, 100U, 0U) == ProgressEstimate{});
    CHECK(estimate_progress(25U, 100U, 5U) == (ProgressEstimate{.remaining_seconds = 15U, .throughput_per_second = 5U}));
    CHECK(estimate_progress(100U, 100U, 20U) == (ProgressEstimate{.remaining_seconds = 0U, .throughput_per_second = 5U}));
    CHECK(estimate_progress(1U, 10U, 7U) == (ProgressEstimate{.remaining_seconds = 63U, .throughput_per_second = 0U}));
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    CHECK(estimate_progress(1U, maximum, maximum) == (ProgressEstimate{.remaining_seconds = maximum, .throughput_per_second = 0U}));
    CHECK(estimate_progress(maximum - 1U, maximum, maximum) == (ProgressEstimate{.remaining_seconds = 1U, .throughput_per_second = 0U}));
}

}  // namespace

void test_compile_progress_reports_monotonic_updates() {
    const mmltk::testsupport::ScopedTempDir root("mmltk_compile_progress");
    const FixtureSpec fixture{
        root.path().string(), "train", 257, 193, 96,
    };
    create_synthetic_dataset(fixture);

    CompilerConfig config;
    config.source_dir = dataset_dir(fixture);
    config.output_dir = fixture.root_dir + "/compiled";
    config.split = fixture.split;
    config.target_width = 97;
    config.target_height = 73;
    config.num_workers = 4;

    const std::thread::id main_thread_id = std::this_thread::get_id();
    struct ProgressRecorder final {
        struct Entry { CompileProgress progress; std::thread::id thread; };
        std::array<Entry, 1024U> entries{};
        std::size_t size = 0U;
        std::mutex mutex;
        std::atomic<unsigned> active{0U};
        std::atomic<bool> overlap{false};
        bool overflow = false;
        void Record(const CompileProgress& progress) noexcept {
            if (active.fetch_add(1U, std::memory_order_acq_rel) != 0U) overlap.store(true, std::memory_order_relaxed);
            {
                std::scoped_lock lock(mutex);
                if (size == entries.size()) overflow = true;
                else entries[size++] = {progress, std::this_thread::get_id()};
                g_compile_elapsed_seconds.fetch_add(1U, std::memory_order_relaxed);
            }
            active.fetch_sub(1U, std::memory_order_release);
        }
    } state;
    const DatasetCompilePlan plan = DatasetCompiler::prepare(config, {config.split});
    const size_t expected_images = static_cast<size_t>(fixture.num_images);
    const size_t expected_total = expected_images * 2 + 1;
    REQUIRE(plan.splits.size() == 1U);
    REQUIRE(plan.splits.front().image_count == static_cast<uint32_t>(fixture.num_images));
    REQUIRE(plan.total_steps() == expected_total);
    g_compile_elapsed_seconds.store(0U, std::memory_order_relaxed);
    CompileTelemetry telemetry{plan.splits[0].image_count,
                               {.context = &state, .report = [](void* context, const CompileProgress& progress) noexcept {
                                    static_cast<ProgressRecorder*>(context)->Record(progress);
                                }}, &compile_test_now};
    DatasetCompiler::compile(plan, 0U, &telemetry);

    REQUIRE_FALSE(state.overflow);
    REQUIRE_FALSE(state.overlap.load(std::memory_order_relaxed));
    REQUIRE(state.size != 0U);
    const std::span observed{state.entries.data(), state.size};
    std::vector<DatasetCompilePhase> semantic_transitions;
    bool main_thread_observed = false;
    for (std::size_t index = 0; index < observed.size(); ++index) {
        const auto& progress = observed[index].progress;
        CHECK(progress.total == expected_total);
        CHECK(progress.done <= expected_total);
        main_thread_observed |= observed[index].thread == main_thread_id;
        if (semantic_transitions.empty() || semantic_transitions.back() != progress.phase) semantic_transitions.push_back(progress.phase);
        if (index != 0U) {
            const auto& previous = observed[index - 1U].progress;
            CHECK(progress.done >= previous.done);
            CHECK(progress.label_done >= previous.label_done);
            CHECK(progress.pixel_done >= previous.pixel_done);
            CHECK(progress.elapsed_seconds >= previous.elapsed_seconds);
        }
        const auto estimate = estimate_progress(progress.done, progress.total, progress.elapsed_seconds);
        CHECK(progress.remaining_seconds == estimate.remaining_seconds);
        CHECK(progress.throughput_per_second == estimate.throughput_per_second);
    }
    REQUIRE(semantic_transitions == std::vector{DatasetCompilePhase::Planning, DatasetCompilePhase::Labels, DatasetCompilePhase::Pixels,
                                                DatasetCompilePhase::Syncing, DatasetCompilePhase::Publishing});
    const auto& completed = observed.back().progress;
    CHECK(completed.done == expected_total);
    CHECK(completed.phase == DatasetCompilePhase::Publishing);
    CHECK(completed.label_done == expected_images);
    CHECK(completed.pixel_done == expected_images);
    CHECK(completed.active_workers == 0U);
    CHECK(completed.dropped_instances == 0U);
    CHECK(completed.remaining_seconds == 0U);
    CHECK(observed.front().progress.elapsed_seconds == 0U);
    CHECK(main_thread_observed);

    test_vanished_masks_are_omitted();
    test_partial_mask_vanish_keeps_instance();
    test_native_compile_letterboxes_pixels_boxes_and_masks();
    test_compiled_tiny_masks_keep_outer_pixel_edges();
    test_invalid_annotations_fail_loud();
}

void test_snapshot_overlaps_compile_reset() {
    const mmltk::testsupport::ScopedTempDir root("mmltk_compile_progress_reset_overlap");
    const FixtureSpec fixture{
        root.path().string(), "train", 16, 16, 1,
    };
    create_synthetic_dataset(fixture);
    CompilerConfig config;
    config.source_dir = dataset_dir(fixture);
    config.output_dir = fixture.root_dir + "/compiled";
    config.split = fixture.split;
    config.target_width = 16U;
    config.target_height = 16U;
    config.num_workers = 1;
    const DatasetCompilePlan plan = DatasetCompiler::prepare(config, {config.split});

    g_compile_elapsed_seconds.store(5U, std::memory_order_relaxed);
    g_overlap_clock_calls.store(0U, std::memory_order_relaxed);
    mmltk::testsupport::TestGate reset("compile reset clock sample");
    g_overlap_reset.store(std::make_shared<const mmltk::testsupport::TestGate::Receipt>(reset.receipt()));
    CompileTelemetry telemetry{plan.splits.front().image_count, {}, &overlap_compile_test_now};
    g_compile_elapsed_seconds.store(20U, std::memory_order_relaxed);
    struct CancellationTag;
    using CancellationSource = mmltk::common::concurrency::EventCancellationSource<CancellationTag, false>;
    auto [stop, token] = CancellationSource::Mint();
    std::future<void> compiler;
    const mmltk::testsupport::ScopedTestCleanup cleanup([&] {
        static_cast<void>(stop.RequestCancel());
        reset.Release();
        if (compiler.valid()) compiler.wait();
        g_overlap_reset.store(nullptr);
    });
    compiler = std::async(std::launch::async, [&] {
        DatasetCompiler::compile(plan, 0U, &telemetry, mmltk::common::concurrency::CancellationObservation::Borrow(token));
    });
    REQUIRE(reset.WaitEntered(std::chrono::seconds{2}));
    g_compile_elapsed_seconds.store(30U, std::memory_order_relaxed);
    const CompileProgress overlapping = telemetry.snapshot();
    reset.Release();
    mmltk::testsupport::await_test_future(compiler, "compile reset settlement", std::chrono::seconds{30});

    CHECK(overlapping.elapsed_seconds == 25U);
    const ProgressEstimate overlapping_estimate = estimate_progress(overlapping.done, overlapping.total, overlapping.elapsed_seconds);
    CHECK(overlapping.remaining_seconds == overlapping_estimate.remaining_seconds);
    CHECK(overlapping.throughput_per_second == overlapping_estimate.throughput_per_second);
    const CompileProgress completed = telemetry.snapshot();
    CHECK(completed.phase == DatasetCompilePhase::Publishing);
    CHECK(completed.elapsed_seconds == 10U);
    CHECK(completed.remaining_seconds == 0U);
}

void test_compile_observes_event_cancellation_without_progress() {
    const mmltk::testsupport::ScopedTempDir root("mmltk_compile_event_cancellation");
    const FixtureSpec fixture{
        root.path().string(), "train", 32, 32, 4,
    };
    struct CancellationTag;
    using CancellationSource = mmltk::common::concurrency::EventCancellationSource<CancellationTag, false>;
    auto [source, token] = CancellationSource::Mint();
    const DatasetCompilePlan plan = prepare_cancellation_compile(fixture);
    REQUIRE(source.RequestCancel());
    REQUIRE_THROWS_AS(DatasetCompiler::compile(plan, 0U, nullptr, mmltk::common::concurrency::CancellationObservation::Borrow(token)),
                      std::runtime_error);
}

void test_compile_reobserves_cancellation_after_publishing_event() {
    const mmltk::testsupport::ScopedTempDir root("mmltk_compile_publish_cancellation");
    const FixtureSpec fixture{
        root.path().string(), "train", 32, 32, 4,
    };
    struct CancellationTag;
    using CancellationSource = mmltk::common::concurrency::EventCancellationSource<CancellationTag, false>;
    auto [source, token] = CancellationSource::Mint();
    const DatasetCompilePlan plan = prepare_cancellation_compile(fixture);
    CompileTelemetry telemetry{plan.splits.front().image_count,
                               {.context = &source, .report = [](void* context, const CompileProgress& progress) noexcept {
                                    if (progress.phase == DatasetCompilePhase::Publishing) {
                                        static_cast<void>(static_cast<CancellationSource*>(context)->RequestCancel());
                                    }
                                }}};
    REQUIRE_THROWS_AS(DatasetCompiler::compile(plan, 0U, &telemetry, mmltk::common::concurrency::CancellationObservation::Borrow(token)),
                      std::runtime_error);
    REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(plan.config.output_dir) / "train.bin"));
}

TEST_CASE("compiler progress remains monotonic", "[backend][data][compile_progress]") {
    test_checked_progress_estimates();
    test_compile_progress_reports_monotonic_updates();
    test_snapshot_overlaps_compile_reset();
    test_compile_observes_event_cancellation_without_progress();
    test_compile_reobserves_cancellation_after_publishing_event();
}
