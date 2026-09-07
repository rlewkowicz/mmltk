module;
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "archive_utils.h"
#include "model_state_technical.h"
#include "model_technical.h"
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "torch_api.h"
#include "detail/checkpoint_private.h"

module mmltk.backend.models.rfdetr.training.checkpoint;

import mmltk.backend.models.rfdetr.core.artifact_resolution;

import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;

#include "model_access.h"
#include "model_state_access.h"

namespace mmltk::backend::models::rfdetr {

namespace torch_api = mmltk::backend::ml::torch_api;

namespace {

[[nodiscard]] std::filesystem::path canonical_checkpoint_path(const std::filesystem::path& checkpoint_path) {
    return std::filesystem::absolute(checkpoint_path).lexically_normal();
}

}  // namespace

ResolvedModelArtifacts resolve_training_artifacts(const std::filesystem::path& weights_path, const std::string_view preset_name,
                                                  const int resolution) {
    return resolve_model_artifacts(weights_path, preset_name, resolution);
}

ModelStateLoadSummary load_model_weights(NativeRfDetrModel& model, const std::filesystem::path& weights_path, const bool strict) {
    const auto checkpoint = decode_model_state(weights_path);
    return detail::native_model_owner(model).load_normalized_state(detail::model_state_owner(checkpoint).entries, strict);
}

ModelStateLoadSummary apply_checkpoint_to_module(NativeRfDetrModel& module, const DecodedNativeModelState& checkpoint, const bool strict) {
    return detail::native_model_owner(module).load_normalized_state(detail::model_state_owner(checkpoint).entries, strict);
}

ModelStateLoadSummary apply_checkpoint_to_module(NativeRfDetrModel& module, const std::filesystem::path& checkpoint_path, bool strict) {
    const auto checkpoint = decode_model_state(checkpoint_path);
    return detail::native_model_owner(module).load_normalized_state(detail::model_state_owner(checkpoint).entries, strict);
}

void save_native_checkpoint(const std::filesystem::path& checkpoint_path, const DecodedNativeModelState& checkpoint) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_checkpoint_save_total{"rfdetr.checkpoint.save.total"};
    const std::filesystem::path canonical_path = canonical_checkpoint_path(checkpoint_path);
    const std::string canonical_path_string = canonical_path.string();
    std::filesystem::create_directories(canonical_path.parent_path());

    torch_api::OutputArchive archive;
    detail::write_native_checkpoint_metadata(archive, checkpoint.metadata);
    detail::write_state_archive(archive, "state", detail::model_state_owner(checkpoint).entries);
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_checkpoint_save_archive_save_to{"rfdetr.checkpoint.save.archive_save_to"};
        archive.save_to(canonical_path_string);
    }
}

DecodedNativeModelState normalize_checkpoint_to_native(const std::filesystem::path& input_path, const std::filesystem::path& output_path) {
    auto checkpoint = decode_model_state(input_path);
    save_native_checkpoint(output_path, checkpoint);
    return checkpoint;
}

}  // namespace mmltk::backend::models::rfdetr
