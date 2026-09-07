/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_loader_ScriptCompression_h
#define js_loader_ScriptCompression_h

#include "ErrorList.h"
#include "mozilla/Vector.h"

namespace JS::loader {

class ScriptLoadRequest;

bool ScriptBytecodeCompress(
    mozilla::Vector<uint8_t>& aBytecodeBuf, size_t aBytecodeOffset,
    mozilla::Vector<uint8_t>& aCompressedBytecodeBufOut);

bool ScriptBytecodeDecompress(mozilla::Vector<uint8_t>& aCompressedBytecodeBuf,
                              size_t aBytecodeOffset,
                              mozilla::Vector<uint8_t>& aBytecodeBufOut);

}  

#endif  // js_loader_ScriptCompression_h
