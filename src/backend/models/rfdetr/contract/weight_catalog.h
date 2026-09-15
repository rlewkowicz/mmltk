#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mmltk::backend::models::rfdetr {

struct WeightAsset final {
    std::string_view filename;
    std::string_view download_url;
    std::string_view md5_hash;
    bool coco_sparse_slots = false;

    constexpr bool operator==(const WeightAsset&) const noexcept = default;
};

[[nodiscard]] std::span<const WeightAsset> weight_catalog() noexcept;
[[nodiscard]] const WeightAsset* find_weight_asset(std::string_view filename) noexcept;
[[nodiscard]] std::optional<WeightAsset> resolve_weight_asset_for_path(const std::string& path);
[[nodiscard]] bool is_registered_weight_asset(std::string_view filename) noexcept;

}  // namespace mmltk::backend::models::rfdetr
