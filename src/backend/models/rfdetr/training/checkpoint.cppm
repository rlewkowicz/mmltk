module;
#include <filesystem>
#include <string_view>
#include "src/backend/models/rfdetr/core/model_state_load.h"
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/core/model_state.h"
namespace mmltk::backend::models::rfdetr { class NativeRfDetrModel; }
export module mmltk.backend.models.rfdetr.training.checkpoint;
export namespace mmltk::backend::models::rfdetr {
ModelStateLoadSummary apply_checkpoint_to_module(NativeRfDetrModel& module, const DecodedNativeModelState& checkpoint, bool strict = true);
ModelStateLoadSummary apply_checkpoint_to_module(NativeRfDetrModel& module, const std::filesystem::path& checkpoint_path, bool strict = true);
void save_native_checkpoint(const std::filesystem::path& checkpoint_path, const DecodedNativeModelState& checkpoint,
                            const std::filesystem::path& explicit_descriptor = {});
DecodedNativeModelState normalize_checkpoint_to_native(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                                                       const std::filesystem::path& class_layout_path = {});
ModelStateLoadSummary load_model_weights(NativeRfDetrModel& model, const std::filesystem::path& weights_path, bool strict = false);
}  // namespace mmltk::backend::models::rfdetr
