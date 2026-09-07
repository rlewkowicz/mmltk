// Copyright 2007-2010 Baptiste Lepilleur and The JsonCpp Authors
// Distributed under MIT license, or public domain if desired and
// See file LICENSE for detail or copy at http://jsoncpp.sourceforge.net/LICENSE

#ifndef JSON_ASSERTIONS_H_INCLUDED
#define JSON_ASSERTIONS_H_INCLUDED

#include <cstdlib>
#include <sstream>

#if !defined(JSON_IS_AMALGAMATION)
#include "config.h"
#endif // if !defined(JSON_IS_AMALGAMATION)

#if JSON_USE_EXCEPTION

#define JSON_ASSERT(condition)                                                 \
  do {                                                                         \
    if (!(condition)) {                                                        \
      Json::throwLogicError("assert json failed");                             \
    }                                                                          \
  } while (0)

#define JSON_FAIL_MESSAGE(message)                                             \
  do {                                                                         \
    OStringStream oss;                                                         \
    oss << message;                                                            \
    Json::throwLogicError(oss.str());                                          \
    abort();                                                                   \
  } while (0)

#else // JSON_USE_EXCEPTION

#define JSON_ASSERT(condition) assert(condition)

#define JSON_FAIL_MESSAGE(message)                                             \
  {                                                                            \
    OStringStream oss;                                                         \
    oss << message;                                                            \
    assert(false && oss.str().c_str());                                        \
    abort();                                                                   \
  }

#endif

#define JSON_ASSERT_MESSAGE(condition, message)                                \
  do {                                                                         \
    if (!(condition)) {                                                        \
      JSON_FAIL_MESSAGE(message);                                              \
    }                                                                          \
  } while (0)

#endif // JSON_ASSERTIONS_H_INCLUDED
