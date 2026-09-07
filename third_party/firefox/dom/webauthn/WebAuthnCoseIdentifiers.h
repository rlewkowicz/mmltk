/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_WebAuthnCoseIdentifiers_h
#define mozilla_dom_WebAuthnCoseIdentifiers_h

#include "mozilla/dom/WebCryptoCommon.h"

namespace mozilla::dom {

enum class CoseAlgorithmIdentifier : int32_t {
  ES256 = -7,
  RS256 = -257,
};

}  

#endif  // mozilla_dom_WebAuthnCoseIdentifiers_h
