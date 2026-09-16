module;

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include "src/backend/ml/torch/archive.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include <torch/types.h>
#include <torch/serialize.h>
#include "detail/checkpoint_private.h"
module mmltk.backend.models.rfdetr.training.checkpoint;
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::models::rfdetr {
namespace {
[[nodiscard]] std::filesystem::path canonical_checkpoint_path(const std::filesystem::path& checkpoint_path) {
    return std::filesystem::absolute(checkpoint_path).lexically_normal();
}
}  // namespace
ModelStateLoadSummary load_model_weights(NativeRfDetrModel& model, const std::filesystem::path& weights_path, const bool strict) {
    const auto checkpoint = decode_model_state(weights_path);
    return apply_checkpoint_to_module(model, checkpoint, strict);
}
ModelStateLoadSummary apply_checkpoint_to_module(NativeRfDetrModel& module, const DecodedNativeModelState& checkpoint, const bool strict) {
    return module.load_normalized_state(checkpoint.entries(), strict, &checkpoint.metadata.class_layout);
}
ModelStateLoadSummary apply_checkpoint_to_module(NativeRfDetrModel& module, const std::filesystem::path& checkpoint_path, bool strict) {
    const auto checkpoint = decode_model_state(checkpoint_path);
    return apply_checkpoint_to_module(module, checkpoint, strict);
}
namespace {
void save_checkpoint_state(const std::filesystem::path& checkpoint_path, const DecodedNativeModelState& checkpoint, bool complete,
                           const std::filesystem::path& explicit_descriptor) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_checkpoint_save_total{"rfdetr.checkpoint.save.total"};
    const std::filesystem::path canonical_path = canonical_checkpoint_path(checkpoint_path);
    std::filesystem::create_directories(canonical_path.parent_path());
    validate_decoded_model_state(checkpoint);
    mmltk::backend::ml::cuda::TensorReadbackBuffers readback;
    readback.Begin();
    detail::reserve_state_archive(checkpoint.entries(), readback, 0);
    torch::serialize::OutputArchive archive;
    detail::write_native_checkpoint_metadata(archive, checkpoint.metadata);
    if (complete)
        detail::write_resume_state_archive(archive, "state", checkpoint.entries(), readback, 0);
    else
        detail::write_state_archive(archive, "state", checkpoint.entries(), readback, 0);
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_checkpoint_save_archive_save_to{"rfdetr.checkpoint.save.archive_save_to"};
        readback.Complete();
        detail::publish_native_checkpoint_archive(archive, canonical_path, explicit_descriptor);
    }
}
}  // namespace
void save_native_checkpoint(const std::filesystem::path& checkpoint_path, const DecodedNativeModelState& checkpoint,
                            const std::filesystem::path& explicit_descriptor) {
    save_checkpoint_state(checkpoint_path, checkpoint, false, explicit_descriptor);
}
DecodedNativeModelState normalize_checkpoint_to_native(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                                                       const std::filesystem::path& class_layout_path) {
    auto checkpoint = decode_model_state(input_path, {}, class_layout_path);
    save_checkpoint_state(output_path, checkpoint, true, class_layout_path);
    return checkpoint;
}
}  // namespace mmltk::backend::models::rfdetr
