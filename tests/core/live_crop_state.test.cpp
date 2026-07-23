#include "mmltk/live/crop_state.h"

#include "support/catch2_compat.hpp"

namespace {

using mmltk::live::LiveCaptureRegion;
using mmltk::live::RuntimeCropState;
using mmltk::live::UiCropSnapshot;
using mmltk::live::UiCropState;

void test_default_snapshot_is_empty() {
    UiCropState state;
    const auto snapshot = state.snapshot();
    assert(snapshot);
    assert(!snapshot->has_crop);
    assert(snapshot->generation == 0U);
    assert(snapshot->region.width == 0U);
    assert(snapshot->region.height == 0U);
}

void test_set_and_clear_publish_new_generations() {
    UiCropState state;
    state.set(LiveCaptureRegion{10U, 20U, 30U, 40U});
    const auto set_snapshot = state.snapshot();
    assert(set_snapshot);
    assert(set_snapshot->has_crop);
    assert(set_snapshot->generation == 1U);
    assert(set_snapshot->region.x == 10U);
    assert(set_snapshot->region.y == 20U);
    assert(set_snapshot->region.width == 30U);
    assert(set_snapshot->region.height == 40U);

    state.clear();
    const auto clear_snapshot = state.snapshot();
    assert(clear_snapshot);
    assert(!clear_snapshot->has_crop);
    assert(clear_snapshot->generation == 2U);
    assert(clear_snapshot->region.width == 0U);
    assert(clear_snapshot->region.height == 0U);
}

void test_runtime_state_only_advances_on_new_publications() {
    UiCropState state;
    RuntimeCropState runtime;

    runtime.sync_from(state);
    UiCropSnapshot snapshot = runtime.snapshot();
    assert(!snapshot.has_crop);
    assert(snapshot.generation == 0U);

    state.set(LiveCaptureRegion{4U, 5U, 6U, 7U});
    runtime.sync_from(state);
    snapshot = runtime.snapshot();
    assert(snapshot.has_crop);
    assert(snapshot.generation == 1U);
    assert(snapshot.region.x == 4U);
    assert(snapshot.region.y == 5U);
    assert(snapshot.region.width == 6U);
    assert(snapshot.region.height == 7U);

    runtime.sync_from(state);
    const UiCropSnapshot unchanged = runtime.snapshot();
    assert(unchanged.generation == 1U);
    assert(unchanged.region.x == 4U);
    assert(unchanged.region.y == 5U);

    state.clear();
    runtime.sync_from(state);
    const UiCropSnapshot cleared = runtime.snapshot();
    assert(!cleared.has_crop);
    assert(cleared.generation == 2U);
}

}  

MMLTK_REGISTER_TEST_CASE("[core][live_crop_state]", test_default_snapshot_is_empty);
MMLTK_REGISTER_TEST_CASE("[core][live_crop_state]", test_set_and_clear_publish_new_generations);
MMLTK_REGISTER_TEST_CASE("[core][live_crop_state]", test_runtime_state_only_advances_on_new_publications);
