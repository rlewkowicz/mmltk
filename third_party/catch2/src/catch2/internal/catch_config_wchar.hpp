

#ifndef CATCH_CONFIG_WCHAR_HPP_INCLUDED
#define CATCH_CONFIG_WCHAR_HPP_INCLUDED

#include <catch2/catch_user_config.hpp>

#if defined(__DJGPP__)
#define CATCH_INTERNAL_CONFIG_NO_WCHAR
#endif  // __DJGPP__

#if !defined(CATCH_INTERNAL_CONFIG_NO_WCHAR) && !defined(CATCH_CONFIG_NO_WCHAR) && !defined(CATCH_CONFIG_WCHAR)
#define CATCH_CONFIG_WCHAR
#endif

#endif  // CATCH_CONFIG_WCHAR_HPP_INCLUDED
