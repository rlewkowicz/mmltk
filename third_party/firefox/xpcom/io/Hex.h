/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_Hex_h_
#define mozilla_Hex_h_

#include <cstdint>

#include "mozilla/Span.h"
#include "nscore.h"
#include "nsStringFwd.h"
#include "nsTArrayForwardDeclare.h"

namespace mozilla {

void HexEncode(Span<const uint8_t> aBytes, nsACString& aOut,
               bool aUpperCase = false);

nsresult HexDecode(const nsACString& aHex, nsTArray<uint8_t>& aOut);

}  

#endif  // mozilla_Hex_h_
