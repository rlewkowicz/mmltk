
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0

#ifndef CATCH_CONFIG_STATIC_ANALYSIS_SUPPORT_HPP_INCLUDED
#define CATCH_CONFIG_STATIC_ANALYSIS_SUPPORT_HPP_INCLUDED

#include <catch2/catch_user_config.hpp>

#if defined(__clang_analyzer__) || defined(__COVERITY__)
#define CATCH_INTERNAL_CONFIG_STATIC_ANALYSIS_SUPPORT
#endif

#if defined(CATCH_INTERNAL_CONFIG_STATIC_ANALYSIS_SUPPORT) &&         \
    !defined(CATCH_CONFIG_NO_EXPERIMENTAL_STATIC_ANALYSIS_SUPPORT) && \
    !defined(CATCH_CONFIG_EXPERIMENTAL_STATIC_ANALYSIS_SUPPORT)
#define CATCH_CONFIG_EXPERIMENTAL_STATIC_ANALYSIS_SUPPORT
#endif

#endif  // CATCH_CONFIG_STATIC_ANALYSIS_SUPPORT_HPP_INCLUDED
