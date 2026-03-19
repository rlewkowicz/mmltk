#pragma once

#include <cstdio>

#if defined(NDEBUG)
#define FASTLOADER_DEBUG_LOG(...) ((void)0)
#else
#define FASTLOADER_DEBUG_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#endif
