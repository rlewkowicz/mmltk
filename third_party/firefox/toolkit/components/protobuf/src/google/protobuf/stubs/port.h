// Copyright 2008 Google Inc.  All rights reserved.
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#if !defined(GOOGLE_PROTOBUF_STUBS_PORT_H_)
#define GOOGLE_PROTOBUF_STUBS_PORT_H_

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "google/protobuf/stubs/platform_macros.h"

#include "google/protobuf/port_def.inc"  // NOLINT

#if defined(_MSC_VER)
#include <stdlib.h>  // NOLINT(build/include)
#include <intrin.h>
#elif defined(__linux__) || 0 || 0
#include <byteswap.h>  // IWYU pragma: export
#endif

#if defined(_MSC_VER) && defined(PROTOBUF_USE_DLLS)
  #if defined(LIBPROTOBUF_EXPORTS)
    #define LIBPROTOBUF_EXPORT __declspec(dllexport)
  #else
    #define LIBPROTOBUF_EXPORT __declspec(dllimport)
  #endif
  #if defined(LIBPROTOC_EXPORTS)
    #define LIBPROTOC_EXPORT   __declspec(dllexport)
  #else
    #define LIBPROTOC_EXPORT   __declspec(dllimport)
  #endif
#else
  #define LIBPROTOBUF_EXPORT
  #define LIBPROTOC_EXPORT
#endif

#define PROTOBUF_RUNTIME_DEPRECATED(message) [[deprecated]] (message)
#define GOOGLE_PROTOBUF_RUNTIME_DEPRECATED(message) [[deprecated]] (message)


namespace google {
namespace protobuf {

typedef unsigned int uint;

typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

static const int32 kint32max = 0x7FFFFFFF;
static const int32 kint32min = -kint32max - 1;
static const int64 kint64max = int64_t{0x7FFFFFFFFFFFFFFF};
static const int64 kint64min = -kint64max - 1;
static const uint32 kuint32max = 0xFFFFFFFFu;
static const uint64 kuint64max = uint64_t{0xFFFFFFFFFFFFFFFFu};

inline uint16_t GOOGLE_UNALIGNED_LOAD16(const void *p) {
  uint16_t t;
  memcpy(&t, p, sizeof t);
  return t;
}

inline uint32_t GOOGLE_UNALIGNED_LOAD32(const void *p) {
  uint32_t t;
  memcpy(&t, p, sizeof t);
  return t;
}

inline uint64_t GOOGLE_UNALIGNED_LOAD64(const void *p) {
  uint64_t t;
  memcpy(&t, p, sizeof t);
  return t;
}

inline void GOOGLE_UNALIGNED_STORE16(void *p, uint16_t v) {
  memcpy(p, &v, sizeof v);
}

inline void GOOGLE_UNALIGNED_STORE32(void *p, uint32_t v) {
  memcpy(p, &v, sizeof v);
}

inline void GOOGLE_UNALIGNED_STORE64(void *p, uint64_t v) {
  memcpy(p, &v, sizeof v);
}

#if defined(GOOGLE_PROTOBUF_OS_NACL) \
    || (0 && defined(__clang__) \
        && (__clang_major__ == 3 && __clang_minor__ == 8) \
        && (__clang_patchlevel__ < 275480))
# define GOOGLE_PROTOBUF_USE_PORTABLE_LOG2
#endif

#if defined(_MSC_VER)
#define bswap_16(x) _byteswap_ushort(x)
#define bswap_32(x) _byteswap_ulong(x)
#define bswap_64(x) _byteswap_uint64(x)

#elif !defined(__linux__) && !0 && !0

#if !defined(bswap_16)
static inline uint16_t bswap_16(uint16_t x) {
  return static_cast<uint16_t>(((x & 0xFF) << 8) | ((x & 0xFF00) >> 8));
}
#define bswap_16(x) bswap_16(x)
#endif

#if !defined(bswap_32)
static inline uint32_t bswap_32(uint32_t x) {
  return (((x & 0xFF) << 24) |
          ((x & 0xFF00) << 8) |
          ((x & 0xFF0000) >> 8) |
          ((x & 0xFF000000) >> 24));
}
#define bswap_32(x) bswap_32(x)
#endif

#if !defined(bswap_64)
static inline uint64_t bswap_64(uint64_t x) {
  return (((x & uint64_t{0xFFu}) << 56) | ((x & uint64_t{0xFF00u}) << 40) |
          ((x & uint64_t{0xFF0000u}) << 24) |
          ((x & uint64_t{0xFF000000u}) << 8) |
          ((x & uint64_t{0xFF00000000u}) >> 8) |
          ((x & uint64_t{0xFF0000000000u}) >> 24) |
          ((x & uint64_t{0xFF000000000000u}) >> 40) |
          ((x & uint64_t{0xFF00000000000000u}) >> 56));
}
#define bswap_64(x) bswap_64(x)
#endif

#endif

PROTOBUF_EXPORT uint32_t ghtonl(uint32_t x);

class BigEndian {
 public:
#if defined(ABSL_IS_LITTLE_ENDIAN)

  static uint16_t FromHost16(uint16_t x) { return bswap_16(x); }
  static uint16_t ToHost16(uint16_t x) { return bswap_16(x); }

  static uint32_t FromHost32(uint32_t x) { return bswap_32(x); }
  static uint32_t ToHost32(uint32_t x) { return bswap_32(x); }

  static uint64_t FromHost64(uint64_t x) { return bswap_64(x); }
  static uint64_t ToHost64(uint64_t x) { return bswap_64(x); }

  static bool IsLittleEndian() { return true; }

#else

  static uint16_t FromHost16(uint16_t x) { return x; }
  static uint16_t ToHost16(uint16_t x) { return x; }

  static uint32_t FromHost32(uint32_t x) { return x; }
  static uint32_t ToHost32(uint32_t x) { return x; }

  static uint64_t FromHost64(uint64_t x) { return x; }
  static uint64_t ToHost64(uint64_t x) { return x; }

  static bool IsLittleEndian() { return false; }

#endif

  static uint16_t Load16(const void *p) {
    return ToHost16(GOOGLE_UNALIGNED_LOAD16(p));
  }

  static void Store16(void *p, uint16_t v) {
    GOOGLE_UNALIGNED_STORE16(p, FromHost16(v));
  }

  static uint32_t Load32(const void *p) {
    return ToHost32(GOOGLE_UNALIGNED_LOAD32(p));
  }

  static void Store32(void *p, uint32_t v) {
    GOOGLE_UNALIGNED_STORE32(p, FromHost32(v));
  }

  static uint64_t Load64(const void *p) {
    return ToHost64(GOOGLE_UNALIGNED_LOAD64(p));
  }

  static void Store64(void *p, uint64_t v) {
    GOOGLE_UNALIGNED_STORE64(p, FromHost64(v));
  }
};

}  
}  

#include "google/protobuf/port_undef.inc"

#endif
