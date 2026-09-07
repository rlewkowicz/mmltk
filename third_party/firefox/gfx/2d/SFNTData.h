/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_gfx_SFNTData_h
#define mozilla_gfx_SFNTData_h

#include "mozilla/UniquePtr.h"
#include "mozilla/Vector.h"

namespace mozilla {
namespace gfx {

class SFNTData final {
 public:
  static UniquePtr<SFNTData> Create(const uint8_t* aFontData,
                                    uint32_t aDataLength);

  static uint64_t GetUniqueKey(const uint8_t* aFontData, uint32_t aDataLength,
                               uint32_t aVarDataSize, const void* aVarData);

  ~SFNTData();

 private:
  SFNTData() = default;

  bool AddFont(const uint8_t* aFontData, uint32_t aDataLength,
               uint32_t aOffset);

  uint32_t HashHeadAndCmapTables();

  class Font;

  Vector<Font*> mFonts;
};

}  
}  

#endif  // mozilla_gfx_SFNTData_h
