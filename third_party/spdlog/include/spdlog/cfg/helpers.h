// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

#include <spdlog/common.h>
#include <unordered_map>

namespace spdlog {
namespace cfg {
namespace helpers {
SPDLOG_API void load_levels(const std::string& levels_spec);
}

}  // namespace cfg
}  // namespace spdlog

#ifdef SPDLOG_HEADER_ONLY
#include "helpers-inl.h"
#endif  // SPDLOG_HEADER_ONLY
