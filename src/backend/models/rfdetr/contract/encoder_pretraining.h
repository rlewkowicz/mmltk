#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include "src/backend/models/rfdetr/contract/model_topology.h"
#include "src/frameworks/reflection/reflected_declarations.h"
namespace mmltk::backend::models::rfdetr {
// Source geometry describes the original encoder, independently of preset geometry.
struct EncoderPretrainingGeometry final {
 std::uint16_t dimension;
 std::uint16_t feedforward_dimension;
 std::uint8_t heads;
 std::uint8_t layers;
 std::uint8_t patch_size;
 std::uint8_t position_grid;
 std::uint8_t channels = 3U;
 std::uint8_t prefix_tokens = 1U;
 std::uint8_t register_tokens = 0U;
 bool mask_token = false;
 constexpr bool operator==(const EncoderPretrainingGeometry&) const noexcept = default;
};
MMLTK_REFLECT_FIELDS(EncoderPretrainingGeometry)
struct EncoderPretrainingAsset final {
 BackboneKind backbone;
 std::string_view source_repository;
 std::string_view source_revision;
 std::string_view filename;
 std::string_view download_url;
 std::string_view sha256;
 std::uint64_t file_bytes;
 EncoderPretrainingGeometry geometry;
 std::uint32_t adaptation_revision = 1U;
 constexpr bool operator==(const EncoderPretrainingAsset&) const noexcept = default;
};
MMLTK_REFLECT_FIELDS(EncoderPretrainingAsset)
// Pins are the original publishers' safetensors upload commits and LFS metadata.
// Every inspected source tensor is F32. DINO has no registers; PE's pool and
// classification state are excluded by the encoder importer, never destinations.
inline constexpr std::array kEncoderPretrainingAssets{
 EncoderPretrainingAsset{
  BackboneKind::DinoV2Small, "facebook/dinov2-small", "150c8e7bb7cef2d30ec31b13a517af14840ee3f7", "model.safetensors",
  "https://huggingface.co/facebook/dinov2-small/resolve/150c8e7bb7cef2d30ec31b13a517af14840ee3f7/model.safetensors", "ae1e99fcefd534ed978cdeb8326f08030c96e28b7a81ffcbc98a857c84d14be1", 88249960U,
  {384U, 1536U, 6U, 12U, 14U, 37U, 3U, 1U, 0U, true}
 },
 EncoderPretrainingAsset{
  BackboneKind::DinoV2Base, "facebook/dinov2-base", "c73891fc3a90e8fe9ffead361cec7058e0319f03", "model.safetensors",
  "https://huggingface.co/facebook/dinov2-base/resolve/c73891fc3a90e8fe9ffead361cec7058e0319f03/model.safetensors", "d73036b56966966d07975d696bde331762f37297e2f095de8cea0040c3aa0841", 346345912U,
  {768U, 3072U, 12U, 12U, 14U, 37U, 3U, 1U, 0U, true}
 },
 EncoderPretrainingAsset{
  BackboneKind::PeCoreTiny, "timm/vit_pe_core_tiny_patch16_384.fb", "29e4ebb6cc0502b84ba432add45499f4ffa44238", "model.safetensors",
  "https://huggingface.co/timm/vit_pe_core_tiny_patch16_384.fb/resolve/29e4ebb6cc0502b84ba432add45499f4ffa44238/model.safetensors", "d97f3436c9bf0cf067d92954d868012df87b74d438d153e386a4d2295166c253",
  24579144U, {192U, 768U, 3U, 12U, 16U, 24U}
 },
};
[[nodiscard]] constexpr const EncoderPretrainingAsset* find_encoder_pretraining_asset(const BackboneKind backbone) noexcept {
 for (const auto& asset : kEncoderPretrainingAssets)
  if (asset.backbone == backbone) return &asset;
 return nullptr;
}
[[nodiscard]] constexpr bool encoder_pretraining_hex_valid(const std::string_view text, const std::size_t size) noexcept {
 if (text.size() != size) return false;
 for (const char byte : text)
  if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) return false;
 return true;
}
[[nodiscard]] consteval bool encoder_pretraining_catalog_is_valid() {
 constexpr auto kinds = mmltk::frameworks::reflection::enum_entries<BackboneKind>();
 if (kinds.size() != kEncoderPretrainingAssets.size()) return false;
 for (const auto kind : kinds)
  if (find_encoder_pretraining_asset(kind.value) == nullptr) return false;
 for (std::size_t index = 0; index < kEncoderPretrainingAssets.size(); ++index) {
  const auto& asset = kEncoderPretrainingAssets[index];
  const auto& geometry = asset.geometry;
  if (!mmltk::frameworks::reflection::enum_contains(asset.backbone) || asset.source_repository.empty() || !encoder_pretraining_hex_valid(asset.source_revision, 40U) ||
      !encoder_pretraining_hex_valid(asset.sha256, 64U) || asset.filename != "model.safetensors" || asset.file_bytes == 0U || asset.adaptation_revision == 0U ||
      asset.download_url != std::string("https://huggingface.co/") + std::string(asset.source_repository) + "/resolve/" + std::string(asset.source_revision) + "/" + std::string(asset.filename) ||
      geometry.dimension == 0U || geometry.feedforward_dimension == 0U || geometry.heads == 0U || geometry.dimension % geometry.heads != 0U || geometry.layers == 0U || geometry.patch_size == 0U ||
      geometry.position_grid == 0U || geometry.channels != 3U || geometry.prefix_tokens != 1U || geometry.register_tokens != 0U)
   return false;
  for (std::size_t sibling = index + 1U; sibling < kEncoderPretrainingAssets.size(); ++sibling) {
   const auto& other = kEncoderPretrainingAssets[sibling];
   if (asset.backbone == other.backbone || asset.source_repository == other.source_repository || asset.download_url == other.download_url || asset.sha256 == other.sha256) return false;
  }
 }
 return true;
}
static_assert(encoder_pretraining_catalog_is_valid());
struct EncoderPretrainingProvenance final {
 MMLTK_MIN_BYTES(1U) MMLTK_MAX_NAME_BYTES std::string asset_id;
 MMLTK_MIN_BYTES(40U) MMLTK_MAX_BYTES(40U) std::string source_revision;
 MMLTK_MIN_BYTES(64U) MMLTK_MAX_BYTES(64U) std::string sha256;
 MMLTK_MINIMUM(std::uint32_t, 1U) std::uint32_t adaptation_revision = 1U;
 bool operator==(const EncoderPretrainingProvenance&) const = default;
};
MMLTK_REFLECT_FIELDS(EncoderPretrainingProvenance)
static_assert(mmltk::frameworks::reflection::ingress_policies_are_valid<EncoderPretrainingProvenance>());
// Historical admission is structural; it never requires the current catalog or source.
[[nodiscard]] inline bool encoder_pretraining_provenance_valid(const EncoderPretrainingProvenance& value) {
 return !mmltk::frameworks::reflection::validate_reflected_fields(value) && encoder_pretraining_hex_valid(value.source_revision, 40U) && encoder_pretraining_hex_valid(value.sha256, 64U);
}
[[nodiscard]] inline EncoderPretrainingProvenance encoder_pretraining_provenance(const EncoderPretrainingAsset& asset) {
 return {std::string(asset.source_repository), std::string(asset.source_revision), std::string(asset.sha256), asset.adaptation_revision};
}
}  // namespace mmltk::backend::models::rfdetr
