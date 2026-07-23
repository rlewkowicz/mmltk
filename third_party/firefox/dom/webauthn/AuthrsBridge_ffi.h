/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_AuthrsBridge_ffi_h
#define mozilla_dom_AuthrsBridge_ffi_h


#include "nsTArray.h"

class nsIWebAuthnAttObj;
class nsIWebAuthnService;

extern "C" {

nsresult authrs_service_constructor(nsIWebAuthnService** result);

nsresult authrs_webauthn_att_obj_constructor(
    const nsTArray<uint8_t>& attestation, bool anonymize,
    nsIWebAuthnAttObj** result);

}  

#endif  // mozilla_dom_AuthrsBridge_ffi_h
