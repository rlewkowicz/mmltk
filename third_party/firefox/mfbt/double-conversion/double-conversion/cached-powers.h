// Copyright 2010 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
//     * Redistributions of source code must retain the above copyright
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//     * Neither the name of Google Inc. nor the names of its
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT

#ifndef DOUBLE_CONVERSION_CACHED_POWERS_H_
#define DOUBLE_CONVERSION_CACHED_POWERS_H_

#include "diy-fp.h"

namespace double_conversion {

namespace PowersOfTenCache {

  static const int kDecimalExponentDistance = 8;

  static const int kMinDecimalExponent = -348;
  static const int kMaxDecimalExponent = 340;

  void GetCachedPowerForBinaryExponentRange(int min_exponent,
                                            int max_exponent,
                                            DiyFp* power,
                                            int* decimal_exponent);

  void GetCachedPowerForDecimalExponent(int requested_exponent,
                                        DiyFp* power,
                                        int* found_exponent);

}  

}  

#endif  // DOUBLE_CONVERSION_CACHED_POWERS_H_
