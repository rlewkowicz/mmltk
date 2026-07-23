

#ifndef CATCH_DEPRECATION_MACRO_HPP_INCLUDED
#define CATCH_DEPRECATION_MACRO_HPP_INCLUDED

#include <catch2/catch_user_config.hpp>

#if !defined(CATCH_CONFIG_NO_DEPRECATION_ANNOTATIONS)
#define CATCH_DEPRECATED(msg) [[deprecated(msg)]]
#else
#define CATCH_DEPRECATED(msg)
#endif

#endif  // CATCH_DEPRECATION_MACRO_HPP_INCLUDED
