
#pragma once
#include <spdlog/tweakme.h>

// The bundled fmtlib copy was removed: this build always defines SPDLOG_USE_STD_FORMAT, so it
// was unreachable. Only std::format and an external fmtlib remain supported.
#if !defined(SPDLOG_USE_STD_FORMAT)
#if !defined(SPDLOG_FMT_EXTERNAL)
#error "spdlog: define SPDLOG_USE_STD_FORMAT or SPDLOG_FMT_EXTERNAL; the bundled fmtlib was removed"
#endif
#include <fmt/ranges.h>
#endif
