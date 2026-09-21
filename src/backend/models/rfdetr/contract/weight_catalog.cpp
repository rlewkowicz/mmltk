#include "src/backend/models/rfdetr/contract/weight_catalog.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
// CLEANUP-IGNORE: This weight-catalog module has an independent global fragment and catalog dependency boundary.
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
namespace mmltk::backend::models::rfdetr {
namespace {
consteval auto make_weight_catalog() {
 std::array<WeightAsset, kPresetCatalog.size()> assets{};
 for (std::size_t index = 0U; index < assets.size(); ++index) {
  const auto& preset = kPresetCatalog[index];
  assets[index] = {
   .filename = preset.canonical_weight_filename,
   .download_url = preset.canonical_weight_url,
   .md5_hash = preset.canonical_weight_md5,
   .coco_sparse_slots = true,
  };
 }
 return assets;
}
inline constexpr auto kWeightCatalog = make_weight_catalog();
}  // namespace
std::span<const WeightAsset> weight_catalog() noexcept { return kWeightCatalog; }
const WeightAsset* find_weight_asset(const std::string_view filename) noexcept {
 for (const auto& asset : kWeightCatalog) {
  if (asset.filename == filename) return &asset;
 }
 return nullptr;
}
std::optional<WeightAsset> resolve_weight_asset_for_path(const std::string& path) {
 if (const auto* asset = find_weight_asset(std::filesystem::path(path).filename().string())) { return *asset; }
 return std::nullopt;
}
bool is_registered_weight_asset(const std::string_view filename) noexcept { return find_weight_asset(filename) != nullptr; }
}  // namespace mmltk::backend::models::rfdetr
