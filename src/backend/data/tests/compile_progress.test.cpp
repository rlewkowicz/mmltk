#include <algorithm>
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/compiled_file_layout.h"
#include "src/backend/data/compiled_dataset.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/data/dataset_loader.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/backend/data/detail/mask_rle_utils.h"
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
void expect_compile_failure(const FixtureSpec& fixture, const std::function<void(const fs::path&)>& mutate, const std::string& expected_error,
                            int target_width = -1, int target_height = -1) {
    create_synthetic_dataset(fixture);
    const fs::path annotation_path = fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl";
    mutate(annotation_path);
    auto config = compiler_config(fixture);
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
[[nodiscard]] std::fstream mutable_compiled_copy(const FixtureSpec& fixture, const fs::path& path) {
    fs::copy_file(compiled_bin_path(fixture), path, fs::copy_options::overwrite_existing);
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());
    return file;
}
void compile_resized_fixture(const FixtureSpec& fixture,
                             mmltk::backend::imaging::resample::ImageResizeMode mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch) {
    auto config = compiler_config(fixture);
    config.resize_mode = mode;
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
    auto config = compiler_config(fixture);
    config.target_width = 32U;
    config.target_height = 32U;
    return DatasetCompiler::prepare(config, {config.split});
}
void test_vanished_masks_retain_detection_annotations() {
    const mmltk::testsupport::ScopedTempDir root("mmltk_compile_drop_vanished_mask");
    const FixtureSpec fixture{
        root.path().string(), "train", 16, 16, 20,
    };
    create_synthetic_dataset(fixture);
    overwrite_annotation(fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl",
                         R"({"class":"person","bbox_xyxy":[0,0,1,1],"mask_rle_encoding":"row_major_start_length","mask_rle":"0:1","image_size_wh":[16,16]})");
    compile_resized_fixture(fixture);
    DatasetLoader loader(resized_fixture_loader_config(fixture));
    REQUIRE(loader.num_label_instances() == 10);
    const auto entry = loader.label_index()[10];
    REQUIRE(entry.num_instances == 1);
    const auto& annotation = loader.label_data()[entry.label_begin];
    CHECK(annotation.has_mask());
    CHECK(annotation.mask_rle_pairs == 0U);
    CHECK(annotation.bbox_x2 == 0.5F);
    CHECK(annotation.original_area == 1.0);
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
    REQUIRE(instance.bbox_x1 == 0.0F);
    REQUIRE(instance.bbox_y1 == 0.0F);
    REQUIRE(instance.bbox_x2 == 3.0F);
    REQUIRE(instance.bbox_y2 == 3.0F);
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
    compile_resized_fixture(fixture, mmltk::backend::imaging::resample::ImageResizeMode::Letterbox);
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
    REQUIRE(instance.bbox_x1 == 2.0F);
    REQUIRE(instance.bbox_y1 == 3.0F);
    REQUIRE(instance.bbox_x2 == 6.0F);
    REQUIRE(instance.bbox_y2 == 5.0F);
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
    const std::array source_runs{RLEPair{0, 1}, RLEPair{7, 1}, RLEPair{24, 1}, RLEPair{31, 1}, RLEPair{11, 1}, RLEPair{11, 2}, RLEPair{10, 4}};
    std::ostringstream annotations;
    for (const auto run : source_runs) {
        annotations << R"({"class":"person","mask_rle_encoding":"row_major_start_length","mask_rle":")" << run.start << ':' << run.length
                    << R"(","image_size_wh":[8,4]})" << '\n';
    }
    overwrite_annotation(fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl", annotations.str());
    compile_resized_fixture(fixture, mmltk::backend::imaging::resample::ImageResizeMode::Letterbox);
    DatasetLoader loader(resized_fixture_loader_config(fixture));
    const auto& entry = loader.label_index()[10];
    REQUIRE(entry.num_instances == source_runs.size());
    for (std::size_t index = 0; index < source_runs.size(); ++index) {
        const auto& instance = loader.label_data()[entry.label_begin + index];
        const auto source = source_runs[index];
        CHECK(instance.bbox_x1 == static_cast<PackedCoordinate>(source.start % 8));
        CHECK(instance.bbox_y1 == static_cast<PackedCoordinate>(source.start / 8 + 2));
        CHECK(instance.bbox_x2 == static_cast<PackedCoordinate>(source.start % 8 + source.length));
        CHECK(instance.bbox_y2 == static_cast<PackedCoordinate>(source.start / 8 + 3));
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
            file << R"({"class":"unknown","bbox_xyxy":[10,10,20,20],"mask_rle_encoding":"row_major_start_length","mask_rle":"660:10","image_size_wh":[65,65]})"
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
    auto config = compiler_config(fixture);
    config.target_width = 97;
    config.target_height = 73;
    config.num_workers = 4;
    const std::thread::id main_thread_id = std::this_thread::get_id();
    struct ProgressRecorder final {
        struct Entry {
            CompileProgress progress;
            std::thread::id thread;
        };
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
                if (size == entries.size())
                    overflow = true;
                else
                    entries[size++] = {progress, std::this_thread::get_id()};
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
    CompileTelemetry telemetry{
        plan.splits[0].image_count,
        {.context = &state,
         .report = [](void* context, const CompileProgress& progress) noexcept { static_cast<ProgressRecorder*>(context)->Record(progress); }},
        &compile_test_now};
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
    test_vanished_masks_retain_detection_annotations();
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
    auto config = compiler_config(fixture);
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
    compiler = std::async(std::launch::async,
                          [&] { DatasetCompiler::compile(plan, 0U, &telemetry, mmltk::common::concurrency::CancellationObservation::Borrow(token)); });
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
    REQUIRE_THROWS_AS(DatasetCompiler::compile(plan, 0U, nullptr, mmltk::common::concurrency::CancellationObservation::Borrow(token)), std::runtime_error);
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
    CompileTelemetry telemetry{plan.splits.front().image_count, {.context = &source, .report = [](void* context, const CompileProgress& progress) noexcept {
                                                                     if (progress.phase == DatasetCompilePhase::Publishing) {
                                                                         static_cast<void>(static_cast<CancellationSource*>(context)->RequestCancel());
                                                                     }
                                                                 }}};
    REQUIRE_THROWS_AS(DatasetCompiler::compile(plan, 0U, &telemetry, mmltk::common::concurrency::CancellationObservation::Borrow(token)), std::runtime_error);
    REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(plan.config.output_dir) / "train.bin"));
}
TEST_CASE("compiler progress remains monotonic", "[backend][data][compile_progress]") {
    test_checked_progress_estimates();
    test_compile_progress_reports_monotonic_updates();
    test_snapshot_overlaps_compile_reset();
    test_compile_observes_event_cancellation_without_progress();
    test_compile_reobserves_cancellation_after_publishing_event();
}
TEST_CASE("Compiler source IDs preserve catalog meaning through reordered dense tables", "[data][catalog]") {
    const auto root = mmltk::testsupport::make_temp_root("compiler-class-catalog");
    const mmltk::testsupport::ScopedTestCleanup cleanup{[&] { fs::remove_all(root); }};
    for (const int base : {0, 1}) {
        FixtureSpec fixture{.root_dir = (root / std::to_string(base)).string(), .num_images = 12, .first_class_id = base};
        create_synthetic_dataset(fixture);
        const auto path = fs::path(dataset_dir(fixture)) / "categories.json";
        std::ifstream input(path);
        auto categories = nlohmann::json::parse(input);
        input.close();
        auto& classes = categories.at("classes");
        std::reverse(classes.begin(), classes.end());
        const auto write = [&] {
            std::ofstream output(path);
            output << categories;
        };
        write();
        auto config = compiler_config(fixture);
        config.num_workers = 1;
        fs::copy(fs::path(dataset_dir(fixture)) / fixture.split, fs::path(dataset_dir(fixture)) / "val", fs::copy_options::recursive);
        auto prepared = DatasetCompiler::prepare(config, {fixture.split, "val"});
        const auto plan = std::move(prepared);
        CHECK(plan.class_catalog.resolve("person") == 0);
        CHECK(plan.class_catalog.resolve("ret") == 1);
        CHECK(plan.class_catalog.resolve("glint") == 5);
        CHECK(plan.source_category_base == base);
        for (std::size_t split = 0; split < plan.splits.size(); ++split) {
            overwrite_annotation(fs::path(dataset_dir(fixture)) / plan.splits[split].split / "000001.jsonl",
                                 R"({"class":"ret","bbox_xyxy":[1,1,3,3]})"
                                 "\n"
                                 R"({"class":"person","bbox_xyxy":[1,1,3,3],"category_id":0})"
                                 "\n"
                                 R"({"class":"person","bbox_xyxy":[1,1,3,3]})");
            DatasetCompiler::compile(plan, split);
            const auto store = CompiledDataset::open(fs::path(compiled_dir(fixture)) / (plan.splits[split].split + ".bin"));
            REQUIRE(store.image_labels(0).size() == 3U);
            CHECK(store.image_labels(0)[0].source_category_id == static_cast<std::uint64_t>(base + 1));
            CHECK(store.image_labels(0)[1].source_category_id == 0U);
            CHECK(store.image_labels(0)[2].source_category_id == static_cast<std::uint64_t>(base));
            CHECK(std::ranges::equal(store.class_names(), plan.class_catalog.names()));
        }
        const auto original = classes;
        classes.push_back({{"id", base + 6}, {"name", std::string(31, 'x')}});
        write();
        CHECK(DatasetCompiler::prepare(config, {fixture.split}).class_catalog.resolve(std::string(31, 'x')) == 6);
        classes.back()["name"] = std::string(32, 'x');
        write();
        CHECK_THROWS(DatasetCompiler::prepare(config, {fixture.split}));
        for (int invalid = 0; invalid < 3; ++invalid) {
            classes = original;
            if (invalid == 0) classes[0]["id"] = classes[1]["id"];
            if (invalid == 1) classes[0]["id"] = base + 8;
            if (invalid == 2) classes[0]["name"] = classes[1]["name"];
            write();
            CHECK_THROWS(DatasetCompiler::prepare(config, {fixture.split}));
        }
        classes = original;
        classes[0]["name"] = std::string("bad\0name", 8);
        write();
        CHECK_THROWS(DatasetCompiler::prepare(config, {fixture.split}));
        classes = nlohmann::json::array();
        for (std::uint32_t index = 0; index < MAX_CLASSES; ++index) classes.push_back({{"id", index + base}, {"name", "class-" + std::to_string(index)}});
        write();
        const auto full = DatasetCompiler::prepare(config, {fixture.split});
        CHECK(full.class_catalog.size() == MAX_CLASSES);
        CHECK(full.class_catalog.resolve("class-255") == 255U);
        classes = original;
        write();
        overwrite_annotation(fs::path(dataset_dir(fixture)) / "train/000001.jsonl", R"({"class":"unknown","bbox_xyxy":[1,1,3,3]})");
        CHECK_THROWS(DatasetCompiler::compile(plan, 0U));
        const auto retained = CompiledDataset::open(compiled_bin_path(fixture));
        CHECK(retained.image_labels(0)[0].source_category_id == static_cast<std::uint64_t>(base + 1));
    }
}
TEST_CASE("compiled annotations preserve continuous source meaning in both resize modes", "[backend][data][compiler]") {
    namespace resize = mmltk::backend::imaging::resample;
    const mmltk::testsupport::ScopedTempDir root("faithful-annotations");
    const FixtureSpec fixture{.root_dir = root.path().string(), .width = 16, .height = 8, .num_images = 1, .pixel_evidence = true};
    create_synthetic_dataset(fixture);
    overwrite_annotation(
        fs::path(dataset_dir(fixture)) / "train/000001.jsonl",
        R"({"class":"person","bbox_xyxy":[-0.5,1.25,12.5,6.75],"id":0,"image_id":0,"category_id":0,"area":7.25,"iscrowd":1,"ignore":1,"mask_rle_encoding":"row_major_start_length","mask_rle":""})"
        "\n"
        R"({"class":"person","bbox_xyxy":[-0.5,1.25,12.5,6.75],"id":8,"image_id":0,"category_id":42})"
        "\n"
        R"({"class":"person","mask_rle_encoding":"row_major_start_length","mask_rle":"36:8 52:8 68:8 84:8"})");
    for (const auto mode : {resize::ImageResizeMode::Stretch, resize::ImageResizeMode::Letterbox}) {
        auto config = compiler_config(fixture);
        config.resize_mode = mode;
        config.target_width = 8;
        config.target_height = 8;
        config.num_workers = 1;
        DatasetCompiler::compile(DatasetCompiler::prepare(config, {"train"}), 0U);
        const auto store = CompiledDataset::open(compiled_bin_path(fixture));
        CHECK(store.header().version == FORMAT_VERSION);
        CHECK(store.header().resize_mode == mode);
        const auto geometry = store.geometry(0);
        CHECK(geometry.resized_width == 8U);
        const float* pixels = store.image_pixels(0);
        // Reciprocal multiplication may differ from scalar division by one float32 step; padding stays exact.
        const auto max_ulps = mode == resize::ImageResizeMode::Stretch ? 1U : 0U;
        CHECK_THAT(pixels[0], Catch::Matchers::WithinULP(mode == resize::ImageResizeMode::Stretch ? 48.0F / 255.0F : 0.0F, max_ulps));
        CHECK_THAT(pixels[64], Catch::Matchers::WithinULP(mode == resize::ImageResizeMode::Stretch ? 80.0F / 255.0F : 0.0F, max_ulps));
        CHECK_THAT(pixels[128], Catch::Matchers::WithinULP(mode == resize::ImageResizeMode::Stretch ? 112.0F / 255.0F : 0.0F, max_ulps));
        CHECK(geometry.resized_height == (mode == resize::ImageResizeMode::Stretch ? 8U : 4U));
        const auto labels = store.image_labels(0);
        REQUIRE(labels.size() == 3U);
        CHECK(labels[0].bbox_x1 == -0.25F);
        CHECK(labels[0].bbox_y1 == (mode == resize::ImageResizeMode::Stretch ? 1.25F : 2.625F));
        CHECK(labels[0].bbox_x2 == labels[1].bbox_x2);
        CHECK(labels[0].original_area == 7.25);
        CHECK(labels[0].is_crowd());
        CHECK(labels[0].raw_ignore());
        CHECK(labels[0].has_mask());
        CHECK(labels[0].has_annotation_id());
        CHECK(labels[0].annotation_id == 0U);
        CHECK(labels[0].has_source_category());
        CHECK(labels[0].source_category_id == 0U);
        CHECK(labels[1].source_category_id == 42U);
        CHECK_FALSE(labels[1].has_mask());
        CHECK(store.instance_rle(labels[0]).empty());
        CHECK(store.masks_available());
        CHECK(labels[0].source_ordinal < labels[1].source_ordinal);
        CHECK(labels[1].source_ordinal < labels[2].source_ordinal);
        CHECK(labels[2].bbox_x1 == 2.0F);
        CHECK(labels[2].bbox_x2 == 6.0F);
        CHECK(labels[2].original_area == 32.0);
        CHECK(store.image_entry(0).has_source_image_id == 1U);
        CHECK(store.image_entry(0).source_image_id == 0U);
    }
}
TEST_CASE("compiled format admission rejects invalid metadata and old versions", "[backend][data][compiler]") {
    const mmltk::testsupport::ScopedTempDir root("compiled-format-admission");
    const FixtureSpec fixture{.root_dir = root.path().string(), .width = 16, .height = 16, .num_images = 1, .background_images = 0};
    create_synthetic_dataset(fixture);
    compile_existing_fixture(fixture);
    const auto original = CompiledDataset::open(compiled_bin_path(fixture));
    const auto mutate = [&](auto change) {
        const auto path = root.path() / "invalid.bin";
        auto file = mutable_compiled_copy(fixture, path);
        auto header = original.header();
        auto label = original.labels().front();
        change(header, label);
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.seekp(static_cast<std::streamoff>(original.header().label_offset));
        file.write(reinterpret_cast<const char*>(&label), sizeof(label));
        file.close();
        CHECK_THROWS(CompiledDataset::open(path));
    };
    mutate([](auto& header, auto&) { header.version = 7U; });
    mutate([](auto& header, auto&) { header.version = 8U; });
    mutate([](auto& header, auto&) { header.resize_mode = static_cast<mmltk::backend::imaging::resample::ImageResizeMode>(255U); });
    mutate([](auto& header, auto&) { header.mask_rle_offset = header.total_file_size + 1U; });
    mutate([](auto&, auto& label) { label.original_area = std::numeric_limits<double>::infinity(); });
    mutate([](auto&, auto& label) { label.bbox_x1 = std::numeric_limits<float>::quiet_NaN(); });
    mutate([](auto&, auto& label) { label.flags = 128U; });
    mutate([](auto&, auto& label) { label.flags &= ~kAnnotationMask; });
    mutate([](auto&, auto& label) { label.mask_rle_offset = std::numeric_limits<decltype(label.mask_rle_offset)>::max(); });
}
TEST_CASE("known empty masks declare availability without RLE storage", "[backend][data][compiler]") {
    const mmltk::testsupport::ScopedTempDir root("empty-mask-presence");
    const FixtureSpec fixture{.root_dir = root.path().string(), .width = 16, .height = 16, .num_images = 1};
    create_synthetic_dataset(fixture);
    overwrite_annotation(fs::path(dataset_dir(fixture)) / "train/000001.jsonl",
                         R"({"class":"person","bbox_xyxy":[1.25,2.5,7.75,9.5],"mask_rle_encoding":"row_major_start_length","mask_rle":""})");
    compile_existing_fixture(fixture);
    const auto store = CompiledDataset::open(compiled_bin_path(fixture));
    REQUIRE(store.labels().size() == 1U);
    CHECK(store.rle_pairs().empty());
    CHECK(store.masks_available());
    CHECK(store.labels()[0].has_mask());
    CHECK(store.labels()[0].original_area == 0.0);
    CHECK(store.labels()[0].bbox_x1 == 1.25F);
}
TEST_CASE("generic bbox narrowing rejects overflow and collapsed corners", "[backend][data][compiler]") {
    const mmltk::testsupport::ScopedTempDir root("bbox-narrowing");
    const FixtureSpec fixture{.root_dir = root.path().string(), .width = 16, .height = 16, .num_images = 1};
    create_synthetic_dataset(fixture);
    for (const auto& [bbox, expected] : std::array{std::pair{"[1e100,0,2e100,1]", "transformed bbox overflow"},
                                                   std::pair{"[1,0,1.000000000001,1]", "transformed bbox loses strict corner ordering"}}) {
        overwrite_annotation(fs::path(dataset_dir(fixture)) / "train/000001.jsonl", std::string(R"({"class":"person","bbox_xyxy":)") + bbox + "}");
        try {
            compile_existing_fixture(fixture);
            FAIL("invalid transformed bbox was accepted");
        } catch (const std::runtime_error& error) {
            CHECK(std::string(error.what()).find(expected) != std::string::npos);
            CHECK(std::string(error.what()).find("at line 1") != std::string::npos);
        }
    }
}
TEST_CASE("generic annotation flags admit only exact boolean or integer zero and one", "[backend][data][compiler]") {
    using mmltk::backend::imaging::resample::ImageResizeMode;
    const mmltk::testsupport::ScopedTempDir root("exact-annotation-flags");
    const FixtureSpec fixture{.root_dir = root.path().string(), .width = 16, .height = 8, .num_images = 1};
    create_synthetic_dataset(fixture);
    const auto annotation_path = fs::path(dataset_dir(fixture)) / "train/000001.jsonl";
    const auto record = [](const char* field, const char* value) {
        return std::string(R"({"class":"person","bbox_xyxy":[1,1,7,5],"id":0,"image_id":0,"category_id":0,"area":7.25,")") + field + "\":" + value + "}";
    };
    const auto read_artifact = [&] {
        std::ifstream file(compiled_bin_path(fixture), std::ios::binary);
        REQUIRE(file.is_open());
        std::ostringstream bytes;
        bytes << file.rdbuf();
        return bytes.str();
    };
    for (const auto mode : {ImageResizeMode::Stretch, ImageResizeMode::Letterbox}) {
        auto config = compiler_config(fixture);
        config.resize_mode = mode;
        config.target_width = 8;
        config.target_height = 8;
        config.num_workers = 1;
        const auto compile = [&] { DatasetCompiler::compile(DatasetCompiler::prepare(config, {"train"}), 0U); };
        for (const char* field : {"iscrowd", "ignore"}) {
            for (const auto& [value, enabled] : std::array{std::pair{"false", false}, std::pair{"true", true}, std::pair{"0", false}, std::pair{"1", true}}) {
                overwrite_annotation(annotation_path, record(field, value));
                compile();
                const auto store = CompiledDataset::open(compiled_bin_path(fixture));
                REQUIRE(store.labels().size() == 1U);
                const auto& label = store.labels().front();
                CHECK(label.is_crowd() == (std::string_view(field) == "iscrowd" && enabled));
                CHECK(label.raw_ignore() == (std::string_view(field) == "ignore" && enabled));
                CHECK_FALSE(label.has_mask());
                CHECK(label.has_annotation_id());
                CHECK(label.annotation_id == 0U);
                CHECK(label.has_source_category());
                CHECK(label.source_category_id == 0U);
                CHECK(label.source_ordinal == 1U);
                CHECK(label.original_area == 7.25);
                CHECK(store.image_entry(0).has_source_image_id == 1U);
                CHECK(store.image_entry(0).source_image_id == 0U);
            }
            const auto accepted_artifact = read_artifact();
            for (const char* value : {"-1", "2", "4294967296", "4294967297", "-4294967296", "18446744073709551615", "1.0", "\"1\""}) {
                overwrite_annotation(annotation_path, record(field, value));
                try {
                    compile();
                    FAIL("invalid annotation flag was accepted");
                } catch (const std::runtime_error& error) {
                    CHECK(std::string(error.what()).find("annotation flag must be") != std::string::npos);
                    CHECK(std::string(error.what()).find("at line 1") != std::string::npos);
                }
                CHECK(read_artifact() == accepted_artifact);
            }
        }
    }
}
TEST_CASE("compiler resize modes use canonical reflected admission", "[backend][data][compiler]") {
    using mmltk::backend::imaging::resample::ImageResizeMode;
    constexpr auto entries = mmltk::frameworks::reflection::enum_entries<ImageResizeMode>();
    STATIC_REQUIRE(entries.size() == 2U);
    STATIC_REQUIRE(entries[0].value == ImageResizeMode::Stretch);
    STATIC_REQUIRE(entries[1].value == ImageResizeMode::Letterbox);
    STATIC_REQUIRE(entries[0].name == "Stretch");
    STATIC_REQUIRE(entries[1].name == "Letterbox");
    CompilerConfig config;
    config.source_dir = "source";
    config.output_dir = "output";
    for (const auto entry : entries) {
        config.resize_mode = entry.value;
        CHECK(validate_compiler_config(config).has_value());
    }
    config.resize_mode = static_cast<ImageResizeMode>(255U);
    const auto invalid = validate_compiler_config(config);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error() == CompilerConfigViolation::InvalidStorage);
}
TEST_CASE("compiled benchmark provenance cannot be erased while Generic identities remain optional", "[backend][data][compiler]") {
    const mmltk::testsupport::ScopedTempDir root("compiled-provenance");
    const FixtureSpec fixture{.root_dir = root.path().string(), .width = 16, .height = 16, .num_images = 1, .background_images = 0};
    create_synthetic_dataset(fixture);
    compile_existing_fixture(fixture);
    const auto original = CompiledDataset::open(compiled_bin_path(fixture));
    const auto path = root.path() / "provenance.bin";
    const auto write = [&](const ImageEntry& image, const PackedInstance& label) {
        auto file = mutable_compiled_copy(fixture, path);
        file.seekp(static_cast<std::streamoff>(original.header().index_offset));
        file.write(reinterpret_cast<const char*>(&image), sizeof(image));
        file.seekp(static_cast<std::streamoff>(original.header().label_offset));
        file.write(reinterpret_cast<const char*>(&label), sizeof(label));
    };
    for (const auto source : {AnnotationSource::Coco, AnnotationSource::Objects365, AnnotationSource::OpenImages, AnnotationSource::CoconutCoco,
                              AnnotationSource::CoconutObjects365V1, AnnotationSource::CoconutObjects365V2}) {
        auto image = original.image_entry(0);
        auto label = original.labels()[0];
        image.source = source;
        image.has_source_image_id = 1U;
        image.source_image_id = 0U;
        label.flags |= kAnnotationCategory;
        label.source_category_id = source == AnnotationSource::OpenImages ? encode_open_images_category("/m/person") : 0U;
        write(image, label);
        REQUIRE_NOTHROW(CompiledDataset::open(path));
        image.has_source_image_id = 0U;
        write(image, label);
        CHECK_THROWS(CompiledDataset::open(path));
        image.has_source_image_id = 1U;
        label.flags &= ~kAnnotationCategory;
        label.source_category_id = 0U;
        write(image, label);
        CHECK_THROWS(CompiledDataset::open(path));
        image.source = AnnotationSource::Generic;
        image.has_source_image_id = 0U;
        write(image, label);
        CHECK_NOTHROW(CompiledDataset::open(path));
    }
    auto unknown = original.image_entry(0);
    unknown.source = static_cast<AnnotationSource>(255U);
    write(unknown, original.labels()[0]);
    CHECK_THROWS(CompiledDataset::open(path));
}
TEST_CASE("category IDs reject arithmetic identity loss before publication", "[backend][data][catalog]") {
    const mmltk::testsupport::ScopedTempDir root("exact-category-ids");
    const FixtureSpec fixture{.root_dir = root.path().string(), .width = 8, .height = 8, .num_images = 1};
    create_synthetic_dataset(fixture);
    const auto config = compiler_config(fixture);
    compile_existing_fixture(fixture);
    const auto destination = compiled_bin_path(fixture);
    const auto read_bytes = [&] {
        std::ifstream file(destination, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    const auto original = read_bytes();
    const std::vector<nlohmann::json> invalid_ids{0.5, 1.5, -1, -4294967295LL, 4294967296ULL, 4294967297ULL, std::numeric_limits<std::uint64_t>::max(),
                                                  "1", true};
    for (const auto& id : invalid_ids) {
        CAPTURE(id);
        const nlohmann::json categories{{"classes", {{{"id", id}, {"name", "person"}}}}};
        {
            std::ofstream file(fs::path(dataset_dir(fixture)) / "categories.json");
            file << categories;
        }
        CHECK_THROWS(DatasetCompiler::prepare(config, {fixture.split}));
        CHECK(read_bytes() == original);
    }
}
TEST_CASE("compiled layout checks alignment and arithmetic without storage", "[backend][data][compiler]") {
    const auto boundary_count = static_cast<std::uint32_t>((HUGE_PAGE_SIZE - sizeof(FileHeader)) / sizeof(ImageEntry));
    CHECK(compute_pixel_layout(boundary_count, 12U).pixel_offset == HUGE_PAGE_SIZE);
    CHECK(compute_pixel_layout(boundary_count + 1U, 12U).pixel_offset == 2U * HUGE_PAGE_SIZE);
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    CHECK_THROWS(compute_pixel_layout(2U, maximum));
    CHECK_THROWS(compute_pixel_layout(1U, maximum));
    auto layout = compute_pixel_layout(1U, 12U);
    CHECK_THROWS(finalize_layout(layout, {maximum, 0U}));
    CHECK_THROWS(finalize_layout(layout, {0U, maximum}));
    CHECK_THROWS(finalize_layout(layout, {maximum / sizeof(PackedInstance), 0U}));
    CHECK_THROWS(finalize_layout(layout, {0U, maximum / sizeof(RLEPair)}));
    const std::vector<std::string> names{"person"};
    for (const auto mode : {mmltk::backend::imaging::resample::ImageResizeMode::Stretch, mmltk::backend::imaging::resample::ImageResizeMode::Letterbox}) {
        for (const std::size_t count : {0U, 1U}) {
            layout = compute_pixel_layout(1U, 12U);
            const auto pixel_offset = layout.pixel_offset;
            finalize_layout(layout, {count, count});
            CHECK(layout.pixel_offset == pixel_offset);
            const auto header = make_file_header({1U, 1U, 1U, 3U, static_cast<std::uint32_t>(count), 12U, mode}, names, layout);
            validate_compiled_header(header);
            const auto sections = validate_compiled_file_sections(header, layout.total_size);
            CHECK(sections.label_count == count);
            CHECK(sections.rle_region_bytes == count * sizeof(RLEPair));
            CHECK(sections.pixel_blob_size == 12U);
            CHECK(std::ranges::all_of(header._reserved, [](auto value) { return value == 0U; }));
            auto invalid = header;
            invalid.pixel_offset += HUGE_PAGE_SIZE;
            CHECK_THROWS(validate_compiled_file_sections(invalid, layout.total_size));
            invalid = header;
            invalid.image_stride = maximum;
            CHECK_THROWS(validate_compiled_file_sections(invalid, layout.total_size));
            invalid = header;
            ++invalid.total_file_size;
            CHECK_THROWS(validate_compiled_file_sections(invalid, invalid.total_file_size));
        }
    }
    CHECK_THROWS(make_file_header({1U, 1U, 1U, 3U, 65536U, 12U}, names, layout));
    constexpr auto runs_per_label = std::numeric_limits<std::uint16_t>::max();
    constexpr std::size_t label_count = std::numeric_limits<std::uint32_t>::max() / (std::size_t{runs_per_label} * sizeof(RLEPair)) + 2U;
    std::vector<PackedInstance> labels(label_count);
    std::uint64_t mask_bytes = 0U;
    for (auto& label : labels) {
        label.flags = kAnnotationMask;
        label.bbox_x2 = label.bbox_y2 = 1.0F;
        label.mask_rle_offset = mmltk::common::math::checked_cast<decltype(label.mask_rle_offset)>(mask_bytes, "compiled mask offset overflow");
        label.mask_rle_pairs = runs_per_label;
        mask_bytes += std::uint64_t{runs_per_label} * sizeof(RLEPair);
    }
    REQUIRE(labels.back().mask_rle_offset > std::numeric_limits<std::uint32_t>::max());
    layout = compute_pixel_layout(1U, 512U * 512U * 3U * sizeof(float));
    finalize_layout(layout, {labels.size(), mask_bytes / sizeof(RLEPair)});
    const auto header = make_file_header({1U, 512U, 512U, 3U, static_cast<std::uint32_t>(labels.size()), 512U * 512U * 3U * sizeof(float)}, names, layout);
    validate_compiled_header(header);
    const auto sections = validate_compiled_file_sections(header, layout.total_size);
    CHECK(validate_compiled_label_entries(labels, header, sections.rle_region_bytes) == mask_bytes);
    CHECK_THROWS(validate_compiled_label_entries(labels, header, mask_bytes - sizeof(RLEPair)));
}
TEST_CASE("categorical resize agrees exactly with dense nearest-center sampling", "[backend][data][compiler][mask]") {
    using namespace mmltk::backend::data::dataset;
    using namespace mmltk::backend::imaging::resample;
    const MaskDimensions source_size{11U, 7U};
    const std::vector<std::vector<RLEPair>> masks{
        {}, {{0U, 77U}}, {{0U, 1U}, {76U, 1U}}, {{8U, 21U}, {31U, 9U}}, {{0U, 4U}, {4U, 7U}, {14U, 11U}, {25U, 3U}}, {{38U, 1U}}};
    MaskResizeScratch scratch;
    const auto check_bounds = [](const RowMajorMaskBounds& actual, const RowMajorMaskBounds& expected) {
        CHECK(actual.has_foreground == expected.has_foreground);
        CHECK(actual.min_x == expected.min_x);
        CHECK(actual.min_y == expected.min_y);
        CHECK(actual.max_x == expected.max_x);
        CHECK(actual.max_y == expected.max_y);
    };
    for (const auto target_size : {MaskDimensions{1U, 1U}, MaskDimensions{3U, 2U}, MaskDimensions{11U, 7U}, MaskDimensions{23U, 19U}, MaskDimensions{19U, 3U}})
        for (const auto mode : {ImageResizeMode::Stretch, ImageResizeMode::Letterbox})
            for (const auto& pairs : masks) {
                INFO("target " << target_size.width << "x" << target_size.height << " mode " << static_cast<int>(mode) << " runs " << pairs.size());
                const auto geometry = compute_image_resize_geometry(source_size.width, source_size.height, target_size.width, target_size.height, mode);
                std::vector<std::uint8_t> source, target(std::size_t(target_size.width) * target_size.height, 0U);
                RowMajorMaskBounds expected_source, actual_source;
                materialize_row_major_mask(pairs, source_size, &source, &expected_source);
                for (std::uint32_t y = 0U; y < geometry.resized_height; ++y) {
                    const auto sy = std::min<std::uint64_t>(source_size.height - 1U, ((2ULL * y + 1U) * source_size.height) / (2ULL * geometry.resized_height));
                    for (std::uint32_t x = 0U; x < geometry.resized_width; ++x) {
                        const auto sx =
                            std::min<std::uint64_t>(source_size.width - 1U, ((2ULL * x + 1U) * source_size.width) / (2ULL * geometry.resized_width));
                        target[std::size_t(y + geometry.offset_y) * target_size.width + x + geometry.offset_x] = source[sy * source_size.width + sx];
                    }
                }
                const auto expected = encode_dense_row_major_mask(target, target_size);
                const auto actual = resize_row_major_mask(pairs, source_size, target_size, geometry, &scratch, &actual_source);
                REQUIRE(actual.pairs.size() == expected.pairs.size());
                for (std::size_t i = 0; i < actual.pairs.size(); ++i) {
                    CHECK(actual.pairs[i].start == expected.pairs[i].start);
                    CHECK(actual.pairs[i].length == expected.pairs[i].length);
                }
                check_bounds(actual.bounds, expected.bounds);
                check_bounds(actual_source, expected_source);
            }
    const auto geometry = compute_image_resize_geometry(11U, 7U, 1U, 1U, ImageResizeMode::Stretch);
    for (const auto& invalid : std::vector<std::vector<RLEPair>>{
             {{0U, 1U}, {77U, 1U}}, {{0U, 1U}, {76U, 0U}}, {{0U, 5U}, {4U, 2U}}, {{70U, 8U}}, {{std::numeric_limits<std::uint32_t>::max(), 2U}}})
        CHECK_THROWS_AS(resize_row_major_mask(invalid, source_size, {1U, 1U}, geometry, &scratch), std::runtime_error);
    const std::array<RLEPair, 1> valid{{{0U, 1U}}};
    CHECK_THROWS_AS(resize_row_major_mask(valid, source_size, {1U, 1U}, {1U, 1U, std::numeric_limits<std::uint32_t>::max(), 0U}, &scratch),
                    std::invalid_argument);
    RowMajorMaskBounds sentinel{1U, 2U, 3U, 4U, true};
    CHECK(resize_row_major_mask({}, {}, {1U, 1U}, geometry, &scratch, &sentinel).pairs.empty());
    CHECK(sentinel.min_x == 1U);
    CHECK(sentinel.has_foreground);
}
