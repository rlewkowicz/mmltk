#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fastloader::rfdetr {

struct WeightAsset {
    std::string_view filename;
    std::string_view download_url;
    std::string_view md5_hash;
};

const std::vector<WeightAsset>& weight_catalog();
const WeightAsset* find_weight_asset(std::string_view filename);

std::optional<WeightAsset> resolve_weight_asset_for_path(const std::string& path);
bool is_registered_weight_asset(std::string_view filename);

} // namespace fastloader::rfdetr
