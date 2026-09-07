
#pragma once

#include <spdlog/tweakme.h>

#if defined(SPDLOG_USE_STD_FORMAT)  // SPDLOG_USE_STD_FORMAT is defined - use std::format
#include <format>
#elif !defined(SPDLOG_FMT_EXTERNAL)
// The bundled fmtlib copy was removed: this build always defines SPDLOG_USE_STD_FORMAT, so it
// was unreachable. Only std::format and an external fmtlib remain supported.
#error "spdlog: define SPDLOG_USE_STD_FORMAT or SPDLOG_FMT_EXTERNAL; the bundled fmtlib was removed"
#else  // SPDLOG_FMT_EXTERNAL is defined - use external fmtlib
#include <fmt/format.h>
#endif
