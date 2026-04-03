#include "default_state.h"

namespace mmltk::gui {

void apply_default_gui_state(std::string& selected_preset_name,
                             TrainViewState& train,
                             ValidateViewState& validate,
                             PredictViewState& predict,
                             AnnotateViewState& annotate,
                             ExportViewState&) {
    selected_preset_name = kDefaultGuiPresetName;

    train.train_compiled_path = "./compiled-seg-medium-synth/train.bin";
    train.val_compiled_path = "./compiled-seg-medium-synth/val.bin";
    train.output_dir = "./engines/output-seg-medium/train-local";
    train.weights_path = "./engines/output-seg-medium/train-local/checkpoint_best_regular.pt";
    train.epochs = 12;
    train.batch_size = 2;
    train.grad_accum_steps = 1;
    train.workers = 16;
    train.prefetch_factor = 3;
    train.progress_bar = true;
    train.lanes = 3;
    train.val_batch_size = 8;
    train.local_device_ids = {0};

    validate.compiled_path = "./compiled-seg-medium-synth/val.bin";
    predict.source.compiled_path = "./compiled-seg-medium-synth/val.bin";
    predict.weights_path = train.weights_path;
    annotate.weights_path = train.weights_path;

    predict.source.kind = SourceKind::CompiledDataset;
    annotate.source.kind = SourceKind::ImageFolder;
    annotate.backend = "auto";
}

} // namespace mmltk::gui
