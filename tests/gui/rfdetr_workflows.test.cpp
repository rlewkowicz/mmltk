#include "gui/local_train_controller.h"
#include "gui/rfdetr_workflows.h"

#include "mmltk/runtime/async_runtime.h"

#include "support/async_runtime_test_utils.h"
#include "support/catch2_compat.hpp"

#include <stdexcept>

namespace {

using namespace mmltk::gui;
using namespace mmltk::gui::rfdetr_workflows;
using namespace mmltk::runtime;
using mmltk::testsupport::drain_until;

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
    state.compile_mode = mmltk::rfdetr::CompilationMode::kSelective;
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

void test_predict_request_round_trips_through_view_state() {
    PredictViewState source = make_predict_state();
    source.source.compiled_path = "/tmp/compiled-a.bin";
    source.onnx_path = "/tmp/model.onnx";
    source.weights_path.clear();
    source.model_input = ModelInputMode::Onnx;
    source.backend = "onnx";
    source.cpu_affinity = "0-3";
    source.progress_bar = true;
    source.compile_mode = mmltk::rfdetr::CompilationMode::kFullTrace;

    const mmltk::rfdetr::PredictRequest request = build_predict_request(source);

    PredictViewState mapped;
    mapped.source.kind = SourceKind::ImageFolder;
    mapped.source.image_directory = "/sentinel";
    mapped.weights_path = "/sentinel";
    mapped.onnx_path = "/sentinel";
    mapped.tensorrt_path = "/sentinel";
    mapped.output_path = "/sentinel";
    mapped.backend = "auto";
    mapped.cpu_affinity = "";
    mapped.model_input = ModelInputMode::Weights;
    mapped.batch_size = 1;
    mapped.max_dets_per_image = 1;
    mapped.device_id = -1;
    mapped.workers = -1;
    mapped.lanes = -1;
    mapped.threshold = 0.0f;
    mapped.allow_fp16 = false;
    mapped.progress_bar = false;
    mapped.compile_mode = mmltk::rfdetr::CompilationMode::kNone;

    apply_predict_request(mapped, request);

    assert(mapped.source.kind == SourceKind::CompiledDataset);
    assert(mapped.source.compiled_path == "/tmp/compiled-a.bin");
    assert(mapped.weights_path.empty());
    assert(mapped.onnx_path == "/tmp/model.onnx");
    assert(mapped.tensorrt_path.empty());
    assert(mapped.output_path == "/tmp/predictions.json");
    assert(mapped.backend == "onnx");
    assert(mapped.cpu_affinity == "0-3");
    assert(mapped.model_input == ModelInputMode::Onnx);
    assert(mapped.batch_size == 4);
    assert(mapped.max_dets_per_image == 250);
    assert(mapped.device_id == 0);
    assert(mapped.workers == 2);
    assert(mapped.lanes == 1);
    assert(mapped.threshold == 0.5f);
    assert(mapped.allow_fp16);
    assert(mapped.progress_bar);
    assert(mapped.compile_mode == mmltk::rfdetr::CompilationMode::kFullTrace);
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
    const mmltk::rfdetr::BuildEngineRequest build_request = build_build_engine_request(export_request, state);
    assert(build_request.onnx_path == "/tmp/rf-detr-seg-medium.onnx");
    assert(build_request.output_path == "/tmp/rf-detr-seg-medium.engine");
    assert(build_request.device_id == 3);
    assert(!build_request.allow_fp16);
}

void test_validate_request_round_trips_through_view_state() {
    ValidateViewState source;
    source.compiled_path = "/tmp/compiled.bin";
    source.source_dir = "/tmp/source";
    source.onnx_path = "/tmp/model.onnx";
    source.tensorrt_path = "/tmp/model.engine";
    source.save_engine_path = "/tmp/save.engine";
    source.report_json_path = "/tmp/report.json";
    source.split = "test";
    source.eval_order = "onnx";
    source.resolution = 640;
    source.limit_images = 16;
    source.alignment_images = 8;
    source.eval_max_dets = 300;
    source.batch_size = 3;
    source.prefetch_factor = 4;
    source.device_id = 2;
    source.workers = 5;
    source.cpu_affinity = "0-7";
    source.recompile = true;
    source.profile = true;
    source.allow_fp16 = false;
    source.write_report_json = false;

    const mmltk::rfdetr::ValidateRequest request = build_validate_request(source);

    ValidateViewState mapped;
    mapped.compiled_path = "/sentinel";
    mapped.source_dir = "/sentinel";
    mapped.onnx_path = "/sentinel";
    mapped.tensorrt_path = "/sentinel";
    mapped.save_engine_path = "/sentinel";
    mapped.report_json_path = "/sentinel";
    mapped.split = "sentinel";
    mapped.eval_order = "sentinel";
    mapped.resolution = 1;
    mapped.limit_images = 1;
    mapped.alignment_images = 1;
    mapped.eval_max_dets = 1;
    mapped.batch_size = 1;
    mapped.prefetch_factor = 1;
    mapped.device_id = -1;
    mapped.workers = -1;
    mapped.cpu_affinity = "sentinel";
    mapped.recompile = false;
    mapped.profile = false;
    mapped.allow_fp16 = true;
    mapped.write_report_json = true;

    apply_validate_request(mapped, request);

    assert(mapped.compiled_path == "/tmp/compiled.bin");
    assert(mapped.source_dir == "/tmp/source");
    assert(mapped.onnx_path == "/tmp/model.onnx");
    assert(mapped.tensorrt_path == "/tmp/model.engine");
    assert(mapped.save_engine_path == "/tmp/save.engine");
    assert(mapped.report_json_path == "/tmp/report.json");
    assert(mapped.split == "test");
    assert(mapped.eval_order == "onnx");
    assert(mapped.resolution == 640);
    assert(mapped.limit_images == 16);
    assert(mapped.alignment_images == 8);
    assert(mapped.eval_max_dets == 300);
    assert(mapped.batch_size == 3);
    assert(mapped.prefetch_factor == 4);
    assert(mapped.device_id == 2);
    assert(mapped.workers == 5);
    assert(mapped.cpu_affinity == "0-7");
    assert(mapped.recompile);
    assert(mapped.profile);
    assert(!mapped.allow_fp16);
    assert(!mapped.write_report_json);
}

void test_train_request_round_trips_through_view_state() {
    TrainViewState source = make_train_state();
    source.val_compiled_path = "/tmp/val.bin";
    source.test_compiled_path = "/tmp/test.bin";
    source.output_dir = "/tmp/train-output";
    source.resume_path.clear();
    source.cpu_affinity = "0-15";
    source.batch_size = 6;
    source.val_batch_size = 2;
    source.epochs = 10;
    source.grad_accum_steps = 4;
    source.eval_max_dets = 400;
    source.lr_drop = 12;
    source.print_freq = 5;
    source.prefetch_factor = 3;
    source.seed = 123;
    source.workers = 8;
    source.lanes = 2;
    source.lr = 2.5e-4;
    source.lr_encoder = 3.0e-4;
    source.lr_component_decay = 0.6;
    source.encoder_layer_decay = 0.75;
    source.momentum = 0.9;
    source.weight_decay = 1.0e-5;
    source.warmup_epochs = 1.5;
    source.warmup_momentum = 0.1;
    source.lr_min_factor = 0.02;
    source.clip_max_norm = 0.2;
    source.lr_scheduler = "cosine";
    source.use_ema = true;
    source.amp = false;
    source.progress_bar = false;
    source.freeze_encoder = true;
    source.optimizer = TrainOptimizerMode::Muon;
    source.compile_mode = mmltk::rfdetr::CompilationMode::kFullTrace;
    source.local_device_ids = {1, 3};
    source.recipe_overrides.lr = true;
    source.recipe_overrides.lr_scheduler = true;

    const mmltk::rfdetr::TrainRequest request = build_train_request(source, {1, 3});

    TrainViewState mapped;
    mapped.execution_target = TrainExecutionTarget::Remote;
    mapped.local_device_ids = {9};
    mapped.optimizer = TrainOptimizerMode::AdamW;
    mapped.compile_mode = mmltk::rfdetr::CompilationMode::kNone;

    apply_train_request(mapped, request);

    assert(mapped.execution_target == TrainExecutionTarget::Local);
    assert(mapped.train_compiled_path == "/tmp/train.bin");
    assert(mapped.val_compiled_path == "/tmp/val.bin");
    assert(mapped.test_compiled_path == "/tmp/test.bin");
    assert(mapped.output_dir == "/tmp/train-output");
    assert(mapped.weights_path == "/tmp/weights.pt");
    assert(mapped.resume_path.empty());
    assert(mapped.cpu_affinity == "0-15");
    assert(mapped.input_mode == TrainInputMode::Weights);
    assert(mapped.batch_size == 6);
    assert(mapped.val_batch_size == 2);
    assert(mapped.epochs == 10);
    assert(mapped.grad_accum_steps == 4);
    assert(mapped.eval_max_dets == 400);
    assert(mapped.lr_drop == 12);
    assert(mapped.print_freq == 5);
    assert(mapped.prefetch_factor == 3);
    assert(mapped.seed == 123);
    assert(mapped.workers == 8);
    assert(mapped.lanes == 2);
    assert(mapped.lr == 2.5e-4);
    assert(mapped.lr_encoder == 3.0e-4);
    assert(mapped.lr_component_decay == 0.6);
    assert(mapped.encoder_layer_decay == 0.75);
    assert(mapped.momentum == 0.9);
    assert(mapped.weight_decay == 1.0e-5);
    assert(mapped.warmup_epochs == 1.5);
    assert(mapped.warmup_momentum == 0.1);
    assert(mapped.lr_min_factor == 0.02);
    assert(mapped.clip_max_norm == 0.2);
    assert(mapped.lr_scheduler == "cosine");
    assert(mapped.use_ema);
    assert(!mapped.amp);
    assert(!mapped.progress_bar);
    assert(mapped.freeze_encoder);
    assert(mapped.optimizer == TrainOptimizerMode::Muon);
    assert(mapped.compile_mode == mmltk::rfdetr::CompilationMode::kFullTrace);
    assert(mapped.local_device_ids == std::vector<int>({1, 3}));
    assert(mapped.recipe_overrides.lr);
    assert(mapped.recipe_overrides.lr_scheduler);
}

void test_local_train_controller_refresh_applies_preferred_device_selection() {
    BackgroundExecutor executor(1);
    UiCallbackQueue queue;
    LocalTrainController controller(executor, queue, [](std::string* error) {
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
    LocalTrainController controller(executor, queue, [&refresh_count](std::string* error) {
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
    LocalTrainController controller(executor, queue, [](std::string*) -> std::vector<LocalGpuInfo> {
        throw std::runtime_error("gpu refresh failed");
    });

    controller.refresh_visible_gpus({});
    drain_until(queue, [&]() { return !controller.gpu_refresh_running(); });
    executor.wait_idle();

    assert(controller.gpu_error() == "gpu refresh failed");
    assert(controller.gpus().empty());
    assert(controller.gpu_selection().empty());
}

void test_ui_callback_queue_wakes_when_callback_is_posted() {
    UiCallbackQueue queue;
    int wake_count = 0;
    int drain_count = 0;
    queue.set_wake_callback([&wake_count]() { ++wake_count; });

    queue.post([&drain_count]() { ++drain_count; });
    assert(wake_count == 1);
    assert(drain_count == 0);
    assert(queue.drain() == 1U);
    assert(drain_count == 1);

    queue.set_wake_callback({});
    queue.post([]() {});
    assert(wake_count == 1);
    assert(queue.drain() == 1U);
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_train_progress_bar_is_forwarded);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_predict_progress_bar_is_forwarded);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_predict_request_round_trips_through_view_state);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_export_request_defaults_onnx_output_from_weights);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_build_engine_request_uses_exported_onnx_output);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_validate_request_round_trips_through_view_state);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_train_request_round_trips_through_view_state);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]",
                         test_local_train_controller_refresh_applies_preferred_device_selection);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]",
                         test_local_train_controller_refresh_preserves_existing_selection_by_device_id);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_local_train_controller_refresh_reports_background_errors);
MMLTK_REGISTER_TEST_CASE("[gui][rfdetr_workflows]", test_ui_callback_queue_wakes_when_callback_is_posted);
