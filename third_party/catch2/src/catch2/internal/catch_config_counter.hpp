
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0

#ifndef CATCH_CONFIG_COUNTER_HPP_INCLUDED
#define CATCH_CONFIG_COUNTER_HPP_INCLUDED

#include <catch2/catch_user_config.hpp>

#if (!defined(__JETBRAINS_IDE__) || __JETBRAINS_IDE__ >= 20170300L)
#define CATCH_INTERNAL_CONFIG_COUNTER
#endif

#if defined(CATCH_INTERNAL_CONFIG_COUNTER) && !defined(CATCH_CONFIG_NO_COUNTER) && !defined(CATCH_CONFIG_COUNTER)
#define CATCH_CONFIG_COUNTER
#endif

#endif  // CATCH_CONFIG_COUNTER_HPP_INCLUDED
