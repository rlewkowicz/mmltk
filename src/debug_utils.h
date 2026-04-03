#pragma once

#include "mmltk_logging.h"

#if defined(NDEBUG)
#define MMLTK_DEBUG_LOG(...) ((void)0)
#else
#define MMLTK_DEBUG_LOG(...) ::mmltk::logging::debug(__VA_ARGS__)
#endif
