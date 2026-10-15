#pragma once
#include <optional>
#include <cstdint>
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include <span>
#include <string>
#include <string_view>
namespace mmltk::backend::models::rfdetr {
struct WeightAsset final {
 std::string_view filename;
 std::string_view download_url;
 std::string_view md5_hash;
 OfficialClassLayout class_layout = OfficialClassLayout::Unresolved;
 std::uint16_t classifier_width = 0U;
 constexpr bool operator==(const WeightAsset&) const noexcept = default;
};
[[nodiscard]] std::span<const WeightAsset> weight_catalog() noexcept;
[[nodiscard]] const WeightAsset* find_weight_asset(std::string_view filename) noexcept;
[[nodiscard]] std::optional<WeightAsset> resolve_weight_asset_for_path(const std::string& path);
[[nodiscard]] bool is_registered_weight_asset(std::string_view filename) noexcept;
}  // namespace mmltk::backend::models::rfdetr
