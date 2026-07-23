#include "rfdetr/predict_runtime_internal.h"
#include "rfdetr/predict_runtime_shared.h"
#include "rfdetr/runtime.h"

#include "support/catch2_compat.hpp"

#include <limits>
#include <string>

namespace {

using namespace mmltk::rfdetr;
using namespace mmltk::rfdetr::predict_internal;

void test_prediction_lane_slot_count_scales_with_runtime_pressure() {
    RuntimeSplit split;
    split.lane_threads = 1;
    split.cpu_threads = 1;
    assert(prediction_lane_slot_count(split) == 2U);

    split.lane_threads = 2;
    split.cpu_threads = 5;
    assert(prediction_lane_slot_count(split) == 4U);

    split.lane_threads = 4;
    split.cpu_threads = 2;
    assert(prediction_lane_slot_count(split) == 2U);
}

void test_prediction_cpu_batch_limit_counts_batches_not_images() {
    RuntimeSplit split;
    split.lane_threads = 1;
    split.cpu_threads = 15;

    assert(prediction_cpu_batch_limit(split, 1U) == 15U);
    assert(prediction_cpu_batch_limit(split, 8U) == 2U);
    assert(prediction_cpu_batch_limit(split, 32U) == 2U);
    assert(prediction_cpu_batch_limit(split, 32U) <=
           prediction_lane_slot_count(split, 32U) * static_cast<size_t>(split.lane_threads));
}

void test_effective_lane_count_prefers_explicit_lane_override() {
    assert(effective_lane_count(1U, 0) == 1);
    assert(effective_lane_count(4U, 2) == 2);
    assert(effective_lane_count(0U, -3) == 1);
}

void test_live_split_helpers_clamp_counts_and_preserve_regions() {
    const std::vector<LiveSplitRegion> splits = build_horizontal_splits(10U, 4U);
    assert(splits.size() == 4U);
    assert(splits[0].x == 0U);
    assert(splits[0].width == 3U);
    assert(splits[1].x == 3U);
    assert(splits[1].width == 3U);
    assert(splits[2].x == 6U);
    assert(splits[2].width == 2U);
    assert(splits[3].x == 8U);
    assert(splits[3].width == 2U);

    const std::vector<LiveSplitRegion> clamped = build_horizontal_splits(3U, 10U);
    assert(clamped.size() == 3U);
    assert(clamped[0].width == 1U);
    assert(clamped[2].x == 2U);

    const mmltk::live::LiveCaptureRegion full_region{100U, 50U, 10U, 6U};
    const mmltk::live::LiveCaptureRegion split_region = make_split_region(full_region, splits[1]);
    assert(split_region.x == 103U);
    assert(split_region.y == 50U);
    assert(split_region.width == 3U);
    assert(split_region.height == 6U);
}

void test_live_runtime_helpers_format_context_and_clamp_large_frame_ids() {
    const std::string context = format_live_split_context(77U, 2U, 4096U, 320U, 180U, 256U, 256U);
    assert(context.find("frame_id=77") != std::string::npos);
    assert(context.find("split_index=2") != std::string::npos);
    assert(context.find("src=320x180") != std::string::npos);
    assert(context.find("dst=256x256") != std::string::npos);

    assert(live_image_id(9U) == 9);
    assert(live_image_id(static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 99U) ==
           std::numeric_limits<int>::max());
}

void test_make_live_runner_state_allocates_single_frame_cuda_state() {
    if (!torch::cuda::is_available()) {
        SKIP("CUDA unavailable");
    }

    ResolvedModelArtifacts artifacts;
    artifacts.config.resolution = 16;

    LiveRunnerState state = make_live_runner_state(artifacts, 0, get_high_priority_cuda_stream(0),
                                                   "cudaEventCreateWithFlags for shared live runner state");

    assert(state.mean.defined());
    assert(state.std.defined());
    assert(state.input_gpu.defined());
    assert(state.input_gpu.device().is_cuda());
    assert(state.input_gpu.size(0) == 1);
    assert(state.input_gpu.size(1) == 3);
    assert(state.input_gpu.size(2) == 16);
    assert(state.input_gpu.size(3) == 16);
    assert(state.ready_event != nullptr);

    destroy_cuda_event(state.ready_event);
}

void test_make_raw_image_batch_workspace_reuses_batch_tensors() {
    if (!torch::cuda::is_available()) {
        SKIP("CUDA unavailable");
    }

    const RawImageBatchWorkspace workspace = make_raw_image_batch_workspace(3, 16, 0);
    assert(workspace.batch_cpu.defined());
    assert(workspace.batch_gpu.defined());
    assert(workspace.nested_mask.defined());
    assert(workspace.batch_cpu.size(0) == 3);
    assert(workspace.batch_gpu.size(0) == 3);
    assert(workspace.nested_mask.size(0) == 3);
    assert(workspace.resize_scratch_slots.size() == 3U);
}

}  

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][runtime_shared]",
                         test_prediction_lane_slot_count_scales_with_runtime_pressure);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][runtime_shared]", test_prediction_cpu_batch_limit_counts_batches_not_images);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][runtime_shared]", test_effective_lane_count_prefers_explicit_lane_override);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][runtime_shared]", test_live_split_helpers_clamp_counts_and_preserve_regions);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][runtime_shared]",
                         test_live_runtime_helpers_format_context_and_clamp_large_frame_ids);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][runtime_shared]",
                         test_make_live_runner_state_allocates_single_frame_cuda_state);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][runtime_shared]", test_make_raw_image_batch_workspace_reuses_batch_tensors);
