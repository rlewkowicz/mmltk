/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "xpcpublic.h"

using namespace JS;

const XPCStringConvert::LiteralExternalString
    XPCStringConvert::sLiteralExternalString;

void XPCStringConvert::LiteralExternalString::finalize(
    JS::Latin1Char* aChars) const {
}

void XPCStringConvert::LiteralExternalString::finalize(char16_t* aChars) const {
}

size_t XPCStringConvert::LiteralExternalString::sizeOfBuffer(
    const JS::Latin1Char* aChars, mozilla::MallocSizeOf aMallocSizeOf) const {
  return 0;
}

size_t XPCStringConvert::LiteralExternalString::sizeOfBuffer(
    const char16_t* aChars, mozilla::MallocSizeOf aMallocSizeOf) const {
  return 0;
}
