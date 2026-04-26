#pragma once

#include "browser/host_api_protocol.h"
#include "runtime_paths.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace mmltk::gui {

struct BrowserHostAssetPaths {
    std::filesystem::path bundle_root;
    std::filesystem::path browser_root;
    std::filesystem::path index_html;
};

class BrowserSnapshotCache {
   public:
    template <typename MakeSnapshotFn>
    [[nodiscard]] std::string encode(MakeSnapshotFn&& make_snapshot) {
        mmltk::browser::StateSnapshot snapshot = std::forward<MakeSnapshotFn>(make_snapshot)();
        snapshot.state_revision = 0;
        const std::string signature = nlohmann::json(snapshot).dump();
        if (signature != last_signature_) {
            last_signature_ = signature;
            ++revision_;
        }
        snapshot.state_revision = revision_;
        return nlohmann::json(snapshot).dump();
    }

    [[nodiscard]] std::uint64_t last_revision() const noexcept {
        return revision_;
    }

   private:
    std::uint64_t revision_ = 0;
    std::string last_signature_;
};

using BrowserHostSnapshotCache = BrowserSnapshotCache;

inline BrowserHostAssetPaths resolve_browser_host_asset_paths(const std::string_view override_root) {
    const auto classify_root = [](const std::filesystem::path& root) {
        BrowserHostAssetPaths paths;
        if (std::filesystem::exists(root / "index.html")) {
            paths.bundle_root = root.parent_path();
            paths.browser_root = root;
            paths.index_html = root / "index.html";
            return paths;
        }

        paths.bundle_root = root;
        paths.browser_root = root / "browser";
        paths.index_html = paths.browser_root / "index.html";
        return paths;
    };

    if (!override_root.empty()) {
        return classify_root(std::filesystem::path(override_root));
    }

#ifdef MMLTK_BROWSER_APP_ASSET_ROOT_SOURCE
    const std::filesystem::path source_root{MMLTK_BROWSER_APP_ASSET_ROOT_SOURCE};
    if (std::filesystem::exists(source_root)) {
        return classify_root(source_root);
    }
#endif

    return classify_root(mmltk::runtime_paths::browser_app_root());
}

}  // namespace mmltk::gui
