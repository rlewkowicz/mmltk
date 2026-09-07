// Copyright 2011 Google Inc. All Rights Reserved.
// Redistribution and use in source and binary forms, with or without
//     * Redistributions of source code must retain the above copyright
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
//     * Neither the name of Google Inc. nor the names of its
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT

#include <algorithm>
#include <string>

#include "snappy-stubs-internal.h"

namespace snappy {

void Varint::Append32(std::string* s, uint32_t value) {
  char buf[Varint::kMax32];
  const char* p = Varint::Encode32(buf, value);
  s->append(buf, p - buf);
}

}  
