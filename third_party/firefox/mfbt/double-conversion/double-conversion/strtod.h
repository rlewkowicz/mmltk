// Copyright 2010 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
//     * Redistributions of source code must retain the above copyright
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//     * Neither the name of Google Inc. nor the names of its
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT

#ifndef DOUBLE_CONVERSION_STRTOD_H_
#define DOUBLE_CONVERSION_STRTOD_H_

#include "utils.h"

namespace double_conversion {

double Strtod(Vector<const char> buffer, int exponent);

float Strtof(Vector<const char> buffer, int exponent);

double StrtodTrimmed(Vector<const char> trimmed, int exponent);

float StrtofTrimmed(Vector<const char> trimmed, int exponent);

inline Vector<const char> TrimTrailingZeros(Vector<const char> buffer) {
  for (int i = buffer.length() - 1; i >= 0; --i) {
    if (buffer[i] != '0') {
      return buffer.SubVector(0, i + 1);
    }
  }
  return Vector<const char>(buffer.start(), 0);
}

}  

#endif  // DOUBLE_CONVERSION_STRTOD_H_
