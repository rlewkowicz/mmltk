/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Iterator_h
#define js_Iterator_h

#include "js/TypeDecls.h"

namespace JS {

JSObject* GetIteratorObject(JSContext* cx, Handle<Value> obj, bool isAsync);

bool IteratorNext(JSContext* cx, Handle<JSObject*> iteratorRecord,
                  MutableHandle<Value> result);

bool IteratorComplete(JSContext* cx, Handle<JSObject*> iterResult, bool* done);

bool IteratorValue(JSContext* cx, Handle<JSObject*> iterResult,
                   MutableHandle<Value> value);

bool GetIteratorRecordIterator(JSContext* cx, Handle<JSObject*> iteratorRecord,
                               MutableHandle<Value> iterator);

bool GetReturnMethod(JSContext* cx, Handle<Value> iterator,
                     MutableHandle<Value> result);

}  

#endif /* js_Iterator_h */
