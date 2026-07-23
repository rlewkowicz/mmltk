// Copyright 2011 Martin Gieseking <martin.gieseking@uos.de>.
// Redistribution and use in source and binary forms, with or without
//     * Redistributions of source code must retain the above copyright
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
//     * Neither the name of Google Inc. nor the names of its
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT

#include "snappy.h"
#include "snappy-c.h"

extern "C" {

snappy_status snappy_compress(const char* input,
                              size_t input_length,
                              char* compressed,
                              size_t *compressed_length) {
  if (*compressed_length < snappy_max_compressed_length(input_length)) {
    return SNAPPY_BUFFER_TOO_SMALL;
  }
  snappy::RawCompress(input, input_length, compressed, compressed_length);
  return SNAPPY_OK;
}

snappy_status snappy_uncompress(const char* compressed,
                                size_t compressed_length,
                                char* uncompressed,
                                size_t* uncompressed_length) {
  size_t real_uncompressed_length;
  if (!snappy::GetUncompressedLength(compressed,
                                     compressed_length,
                                     &real_uncompressed_length)) {
    return SNAPPY_INVALID_INPUT;
  }
  if (*uncompressed_length < real_uncompressed_length) {
    return SNAPPY_BUFFER_TOO_SMALL;
  }
  if (!snappy::RawUncompress(compressed, compressed_length, uncompressed)) {
    return SNAPPY_INVALID_INPUT;
  }
  *uncompressed_length = real_uncompressed_length;
  return SNAPPY_OK;
}

size_t snappy_max_compressed_length(size_t source_length) {
  return snappy::MaxCompressedLength(source_length);
}

snappy_status snappy_uncompressed_length(const char *compressed,
                                         size_t compressed_length,
                                         size_t *result) {
  if (snappy::GetUncompressedLength(compressed,
                                    compressed_length,
                                    result)) {
    return SNAPPY_OK;
  } else {
    return SNAPPY_INVALID_INPUT;
  }
}

snappy_status snappy_validate_compressed_buffer(const char *compressed,
                                                size_t compressed_length) {
  if (snappy::IsValidCompressedBuffer(compressed, compressed_length)) {
    return SNAPPY_OK;
  } else {
    return SNAPPY_INVALID_INPUT;
  }
}

}  
