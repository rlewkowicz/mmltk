// Copyright 2010 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
//     * Redistributions of source code must retain the above copyright
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//     * Neither the name of Google Inc. nor the names of its
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT

#ifndef DOUBLE_CONVERSION_FAST_DTOA_H_
#define DOUBLE_CONVERSION_FAST_DTOA_H_

#include "utils.h"

namespace double_conversion {

enum FastDtoaMode {
  FAST_DTOA_SHORTEST,
  FAST_DTOA_SHORTEST_SINGLE,
  FAST_DTOA_PRECISION
};

static const int kFastDtoaMaximalLength = 17;
static const int kFastDtoaMaximalSingleLength = 9;

bool FastDtoa(double d,
              FastDtoaMode mode,
              int requested_digits,
              Vector<char> buffer,
              int* length,
              int* decimal_point);

}  

#endif  // DOUBLE_CONVERSION_FAST_DTOA_H_
