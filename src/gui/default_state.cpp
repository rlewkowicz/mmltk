#include "default_state.h"

namespace mmltk::gui {

void apply_default_gui_state(TrainViewState& train, ValidateViewState& validate, PredictViewState& predict,
                             AnnotateViewState& annotate, ExportViewState& export_state) {
    train = TrainViewState{};
    validate = ValidateViewState{};
    predict = PredictViewState{};
    annotate = AnnotateViewState{};
    export_state = ExportViewState{};

    train.dataset_source_dir = "./dataset";
    train.compiled_dataset_dir = "./compiled";
    train.use_compiled_directory_defaults = true;
    train.train_compiled_path = "./compiled/train.bin";
    train.val_compiled_path = "./compiled/val.bin";
    train.test_compiled_path.clear();
    train.overwrite_compiled_dataset = false;
    train.compile_dimensions = false;
    train.output_dir = "./gui-train-output";
    train.weights_path.clear();
    train.epochs = 12;
    train.batch_size = 2;
    train.grad_accum_steps = 1;
    train.workers = 16;
    train.prefetch_factor = 3;
    train.progress_bar = true;
    train.lanes = 3;
    train.val_batch_size = 8;
    train.local_device_ids = {0};
    train.gpu_augmentation.enabled = true;

    validate.compiled_path.clear();
    validate.weights_path.clear();
    validate.onnx_path.clear();
    validate.tensorrt_path.clear();
    validate.resolution = kDefaultModelResolution;
    predict.source.compiled_path.clear();
    predict.weights_path.clear();
    predict.onnx_path.clear();
    predict.tensorrt_path.clear();
    annotate.weights_path.clear();
    annotate.onnx_path.clear();
    annotate.tensorrt_path.clear();
    export_state.weights_path.clear();
    export_state.onnx_path.clear();

    predict.source.kind = SourceKind::CompiledDataset;
    annotate.source.kind = SourceKind::ImageFolder;
    annotate.backend = "auto";
}

}  
