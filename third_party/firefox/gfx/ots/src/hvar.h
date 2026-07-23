// Copyright (c) 2018 The OTS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(OTS_HVAR_H_)
#define OTS_HVAR_H_

#include "ots.h"

namespace ots {


class OpenTypeHVAR : public Table {
 public:
  explicit OpenTypeHVAR(Font* font, uint32_t tag)
      : Table(font, tag, tag) { }

  bool Parse(const uint8_t* data, size_t length);
  bool Serialize(OTSStream* out);

 private:
  const uint8_t *m_data;
  size_t m_length;
};

}  

#endif
