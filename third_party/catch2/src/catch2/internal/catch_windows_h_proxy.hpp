

#ifndef CATCH_WINDOWS_H_PROXY_HPP_INCLUDED
#define CATCH_WINDOWS_H_PROXY_HPP_INCLUDED

#include <catch2/internal/catch_platform.hpp>

#if defined(CATCH_PLATFORM_WINDOWS)

#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#endif  // defined(CATCH_PLATFORM_WINDOWS)

#endif  // CATCH_WINDOWS_H_PROXY_HPP_INCLUDED
