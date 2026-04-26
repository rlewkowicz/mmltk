// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once
#include <spdlog/cfg/helpers.h>
#include <spdlog/details/os.h>
#include <spdlog/details/registry.h>

namespace spdlog {
namespace cfg {
inline void load_env_levels(const char* var = "SPDLOG_LEVEL") {
    const auto levels_spec = details::os::getenv(var);
    if (!levels_spec.empty()) {
        helpers::load_levels(levels_spec);
    }
}

}  // namespace cfg
}  // namespace spdlog
