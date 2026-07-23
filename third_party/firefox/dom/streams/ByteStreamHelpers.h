/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ByteStreamHelpers_h
#define mozilla_dom_ByteStreamHelpers_h

#include "UnderlyingSourceCallbackHelpers.h"
#include "js/TypeDecls.h"
#include "mozilla/ErrorResult.h"

namespace mozilla::dom {

class ReadableStream;
class BodyStreamHolder;

JSObject* TransferArrayBuffer(JSContext* aCx, JS::Handle<JSObject*> aObject);

bool CanTransferArrayBuffer(JSContext* aCx, JS::Handle<JSObject*> aObject,
                            ErrorResult& aRv);

JSObject* CloneAsUint8Array(JSContext* aCx, JS::Handle<JSObject*> aObject);

MOZ_CAN_RUN_SCRIPT void
SetUpReadableByteStreamControllerFromBodyStreamUnderlyingSource(
    JSContext* aCx, ReadableStream* aStream,
    BodyStreamHolder* aUnderlyingSource, ErrorResult& aRv);

}  

#endif
