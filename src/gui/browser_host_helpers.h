#pragma once

#include "browser/host_api_protocol.h"
#include "runtime_paths.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace mmltk::gui {

struct BrowserHostAssetPaths {
    std::filesystem::path browser_root;
    std::filesystem::path index_html;
};

class BrowserSnapshotCache {
   public:
    template <typename MakeSnapshotFn>
    [[nodiscard]] std::string encode(MakeSnapshotFn&& make_snapshot) {
        mmltk::browser::StateSnapshot snapshot = std::forward<MakeSnapshotFn>(make_snapshot)();
        snapshot.state_revision = 0;
        nlohmann::json encoded = snapshot;
        if (!has_snapshot_ || encoded != last_snapshot_) {
            last_snapshot_ = encoded;
            has_snapshot_ = true;
            ++revision_;
        }
        encoded["state_revision"] = revision_;
        return encoded.dump();
    }

    [[nodiscard]] std::uint64_t last_revision() const noexcept {
        return revision_;
    }

   private:
    std::uint64_t revision_ = 0;
    nlohmann::json last_snapshot_;
    bool has_snapshot_ = false;
};

using BrowserHostSnapshotCache = BrowserSnapshotCache;

inline BrowserHostAssetPaths resolve_browser_host_asset_paths() {
    const auto classify_root = [](const std::filesystem::path& root) {
        BrowserHostAssetPaths paths;
        paths.browser_root = root;
        paths.index_html = root / "index.html";
        return paths;
    };

    if (const char* override_root = std::getenv("MMLTK_BROWSER_APP_ASSET_ROOT");
        override_root != nullptr && *override_root != '\0') {
        return classify_root(override_root);
    }

#ifdef MMLTK_BROWSER_APP_ASSET_ROOT_SOURCE
    const std::filesystem::path source_root{MMLTK_BROWSER_APP_ASSET_ROOT_SOURCE};
    if (std::filesystem::exists(source_root)) {
        return classify_root(source_root);
    }
#endif

    return classify_root(mmltk::runtime_paths::browser_app_root());
}

}  
