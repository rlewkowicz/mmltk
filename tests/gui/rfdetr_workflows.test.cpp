#include "gui/local_train_controller.h"
#include "gui/rfdetr_workflows.h"

#include "mmltk/runtime/async_runtime.h"

#include "support/catch2_compat.hpp"

#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>

namespace {

using namespace mmltk::gui;
using namespace mmltk::gui::rfdetr_workflows;
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

std::vector<LocalGpuInfo> make_local_gpus(std::initializer_list<int> device_ids) {
    std::vector<LocalGpuInfo> gpus;
    gpus.reserve(device_ids.size());
    for (const int device_id : device_ids) {
        LocalGpuInfo gpu;
        gpu.device_id = device_id;
        gpu.name = "GPU " + std::to_string(device_id);
        gpu.total_memory_bytes = 24ULL * 1024ULL * 1024ULL * 1024ULL;
        gpus.push_back(std::move(gpu));
    }
    return gpus;
}

TrainViewState make_train_state() {
    TrainViewState state;
    state.train_compiled_path = "/tmp/train.bin";
    state.val_compiled_path = "/tmp/val.bin";
    state.output_dir = "/tmp/train-output";
    state.weights_path = "/tmp/weights.pt";
    state.batch_size = 2;
    state.epochs = 4;
    state.grad_accum_steps = 1;
    state.prefetch_factor = 2;
    state.local_device_ids = {0};
    return state;
}

PredictViewState make_predict_state() {
    PredictViewState state;
    state.source.kind = SourceKind::CompiledDataset;
    state.source.compiled_path = "/tmp/compiled.bin";
    state.weights_path = "/tmp/weights.pt";
    state.output_path = "/tmp/predictions.json";
    state.batch_size = 4;
    state.max_dets_per_image = 250;
    state.device_id = 0;
    state.workers = 2;
    state.lanes = 1;
    state.threshold = 0.5f;
    state.compile_mode = 1;
    return state;
}

ExportViewState make_export_state() {
    ExportViewState state;
    state.weights_path = "/tmp/rf-detr-seg-medium.pt";
    state.onnx_path = "";
    state.output_path = "/tmp/rf-detr-seg-medium.engine";
    state.device_id = 3;
    state.opset_version = 19;
    state.allow_fp16 = false;
    state.simplify = true;
    state.build_tensorrt = true;
    return state;
}

void test_train_progress_bar_is_forwarded() {
    TrainViewState enabled_state = make_train_state();
    enabled_state.progress_bar = true;
    const mmltk::rfdetr::TrainRequest enabled = build_train_request(enabled_state, {0});
    assert(enabled.progress_bar);

    TrainViewState disabled_state = make_train_state();
    disabled_state.progress_bar = false;
    const mmltk::rfdetr::TrainRequest disabled = build_train_request(disabled_state, {0});
    assert(!disabled.progress_bar);
}

void test_predict_progress_bar_is_forwarded() {
    PredictViewState enabled_state = make_predict_state();
    enabled_state.progress_bar = true;
    const mmltk::rfdetr::PredictRequest enabled = build_predict_request(enabled_state);
    assert(enabled.progress_bar);
    assert(enabled.source_kind == mmltk::rfdetr::PredictSourceKind::CompiledDataset);
    assert(enabled.compiled_path == "/tmp/compiled.bin");

    PredictViewState disabled_state = make_predict_state();
    disabled_state.progress_bar = false;
    const mmltk::rfdetr::PredictRequest disabled = build_predict_request(disabled_state);
    assert(!disabled.progress_bar);
    assert(disabled.source_kind == mmltk::rfdetr::PredictSourceKind::CompiledDataset);
    assert(disabled.compiled_path == "/tmp/compiled.bin");
}

void test_export_request_defaults_onnx_output_from_weights() {
    const ExportViewState state = make_export_state();
    const mmltk::rfdetr::ExportOnnxRequest request = build_export_onnx_request(state);
    assert(request.weights_path == "/tmp/rf-detr-seg-medium.pt");
    assert(request.output_path == "/tmp/rf-detr-seg-medium.onnx");
    assert(request.device_id == 3);
    assert(request.opset_version == 19);
    assert(request.simplify);
}

void test_build_engine_request_uses_exported_onnx_output() {
    const ExportViewState state = make_export_state();
    const mmltk::rfdetr::ExportOnnxRequest export_request = build_export_onnx_request(state);
    const mmltk::rfdetr::BuildEngineRequest build_request =
        build_build_engine_request(export_request, state);
    assert(build_request.onnx_path == "/tmp/rf-detr-seg-medium.onnx");
    assert(build_request.output_path == "/tmp/rf-detr-seg-medium.engine");
    assert(build_request.device_id == 3);
    assert(!build_request.allow_fp16);
}

void test_local_train_controller_refresh_applies_preferred_device_selection() {
    BackgroundExecutor executor(1);
    UiCallbackQueue queue;
    LocalTrainController controller(
        executor,
        queue,
        [](std::string* error) {
            if (error != nullptr) {
                error->clear();
            }
            return make_local_gpus({0, 1, 2});
        });

    controller.initialize({1});
    assert(controller.gpu_refresh_running());
    drain_until(queue, [&]() { return !controller.gpu_refresh_running(); });
    executor.wait_idle();

    assert(controller.gpu_error().empty());
    assert(controller.gpus().size() == 3U);
    assert(controller.gpu_selection().size() == 3U);
    assert(!controller.gpu_selection()[0]);
    assert(controller.gpu_selection()[1]);
    assert(!controller.gpu_selection()[2]);
    assert(controller.selected_device_ids() == std::vector<int>{1});
}

void test_local_train_controller_refresh_preserves_existing_selection_by_device_id() {
    BackgroundExecutor executor(1);
    UiCallbackQueue queue;
    int refresh_count = 0;
    LocalTrainController controller(
        executor,
        queue,
        [&refresh_count](std::string* error) {
            if (error != nullptr) {
                error->clear();
            }
            ++refresh_count;
            if (refresh_count == 1) {
                return make_local_gpus({0, 1});
            }
            return make_local_gpus({1, 2, 0});
        });

    controller.refresh_visible_gpus({});
    drain_until(queue, [&]() { return !controller.gpu_refresh_running(); });
    executor.wait_idle();
    controller.set_device_selected(0, false);

    controller.refresh_visible_gpus({});
    drain_until(queue, [&]() { return !controller.gpu_refresh_running(); });
    executor.wait_idle();

    assert(controller.gpus().size() == 3U);
    assert(controller.gpu_selection().size() == 3U);
    assert(controller.gpus()[0].device_id == 1);
    assert(controller.gpu_selection()[0]);
    assert(controller.gpus()[1].device_id == 2);
    assert(controller.gpu_selection()[1]);
    assert(controller.gpus()[2].device_id == 0);
    assert(!controller.gpu_selection()[2]);
    assert(controller.selected_device_ids() == (std::vector<int>{1, 2}));
}

void test_local_train_controller_refresh_reports_background_errors() {
    BackgroundExecutor executor(1);
    UiCallbackQueue queue;
    LocalTrainController controller(
        executor,
        queue,
        [](std::string*) -> std::vector<LocalGpuInfo> {
            throw std::runtime_error("gpu refresh failed");
        });

    controller.refresh_visible_gpus({});
    drain_until(queue, [&]() { return !controller.gpu_refresh_running(); });
    executor.wait_idle();

    assert(controller.gpu_error() == "gpu refresh failed");
    assert(controller.gpus().empty());
    assert(controller.gpu_selection().empty());
}

} // namespace

MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_train_progress_bar_is_forwarded);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_predict_progress_bar_is_forwarded);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_export_request_defaults_onnx_output_from_weights);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_build_engine_request_uses_exported_onnx_output);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_local_train_controller_refresh_applies_preferred_device_selection);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_local_train_controller_refresh_preserves_existing_selection_by_device_id);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_local_train_controller_refresh_reports_background_errors);
