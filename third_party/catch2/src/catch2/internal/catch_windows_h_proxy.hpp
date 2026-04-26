
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
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
