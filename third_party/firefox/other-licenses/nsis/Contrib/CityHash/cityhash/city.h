// Copyright (c) 2011 Google, Inc.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// The above copyright notice and this permission notice shall be included in
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER

#ifndef CITY_HASH_H_
#define CITY_HASH_H_

#include <stdlib.h>  // for size_t.
#include <stdint.h>

typedef uint8_t uint8;
typedef uint32_t uint32;
typedef uint64_t uint64;

#ifdef __cplusplus

#include <utility>
typedef std::pair<uint64, uint64> uint128;
inline uint64 Uint128Low64(const uint128& x) { return x.first; }
inline uint64 Uint128High64(const uint128& x) { return x.second; }

extern "C" {
#endif

uint64 CityHash64(const char *buf, size_t len);

uint64 CityHash64WithSeed(const char *buf, size_t len, uint64 seed);

uint64 CityHash64WithSeeds(const char *buf, size_t len,
                           uint64 seed0, uint64 seed1);

#ifdef __cplusplus
}
uint128 CityHash128(const char *s, size_t len);

uint128 CityHash128WithSeed(const char *s, size_t len, uint128 seed);

inline uint64 Hash128to64(const uint128& x) {
  const uint64 kMul = 0x9ddfea08eb382d69;
  uint64 a = (Uint128Low64(x) ^ Uint128High64(x)) * kMul;
  a ^= (a >> 47);
  uint64 b = (Uint128High64(x) ^ a) * kMul;
  b ^= (b >> 47);
  b *= kMul;
  return b;
}
#endif

#endif  // CITY_HASH_H_
