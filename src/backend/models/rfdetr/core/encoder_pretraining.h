#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <stop_token>
#include <vector>
#include "src/backend/models/rfdetr/contract/encoder_pretraining.h"
#include "src/backend/models/rfdetr/core/model_state_load.h"
namespace mmltk::backend::models::rfdetr {
class NativeRfDetrModel;
struct EncoderPretrainingResult final {
 EncoderPretrainingProvenance provenance;
 ModelStateLoadSummary summary;
};
// The only provenance-producing entry point: admits the catalog's exact opened
// file, stages the complete private encoder, then commits through model rollback.
[[nodiscard]] EncoderPretrainingResult initialize_encoder_pretraining(NativeRfDetrModel&, const std::filesystem::path&, std::stop_token = {});
namespace detail {
// Bounded format decoding is also useful for synthetic format tests. Returned
// tensors own their admitted storage; this boundary makes no provenance claim.
[[nodiscard]] std::vector<NormalizedModelStateEntry> decode_encoder_safetensors(std::span<const std::uint8_t> bytes, std::uint64_t maximum_bytes);
[[nodiscard]] torch::Tensor resample_encoder_patch(const torch::Tensor&, std::int64_t patch_size);
[[nodiscard]] torch::Tensor resample_encoder_positions(const torch::Tensor&, std::int64_t grid);
}  // namespace detail
}  // namespace mmltk::backend::models::rfdetr
