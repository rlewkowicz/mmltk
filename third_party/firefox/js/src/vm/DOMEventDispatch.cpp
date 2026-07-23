/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/DOMEventDispatch.h"
#include "js/Printf.h"
#include "vm/JSContext.h"
#include "vm/JSScript.h"
#include "vm/StringType.h"

JS_PUBLIC_API void JS::SetDispatchDOMEventCallback(
    JSContext* cx, DispatchDOMEventCallback callback) {
  if (cx && cx->runtime()) {
    cx->runtime()->dispatchDOMEventCallback = callback;
  }
}

void js::TestingDispatchDOMEvent(JSContext* cx, const char* eventType) {
  if (cx->runtime()->dispatchDOMEventCallback) {
    cx->runtime()->dispatchDOMEventCallback(cx, eventType);
  }
}

void js::TestingDispatchDOMEvent(JSContext* cx, const char* eventType,
                                 JSScript* script) {
  if (!cx->runtime()->dispatchDOMEventCallback) {
    return;
  }

  if (!script || !script->function()) {
    cx->runtime()->dispatchDOMEventCallback(cx, eventType);
    return;
  }

  JS::Rooted<JSAtom*> atom(cx);
  if (!script->function()->getDisplayAtom(cx, &atom)) {
    cx->clearPendingException();
    return;
  }

  if (!atom) {
    return;
  }

  JS::UniqueChars nameStr = AtomToPrintableString(cx, atom);
  if (!nameStr) {
    cx->clearPendingException();
    return;
  }

  JS::UniqueChars eventWithName =
      JS_smprintf("%s: %s", eventType, nameStr.get());
  if (eventWithName) {
    cx->runtime()->dispatchDOMEventCallback(cx, eventWithName.get());
    return;
  }
}
