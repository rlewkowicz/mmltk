/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_friend_ErrorMessages_h
#define js_friend_ErrorMessages_h

#include "jstypes.h"  // JS_PUBLIC_API

struct JSErrorFormatString;

enum JSErrNum {
#define MSG_DEF(name, count, exception, format) name,
#include "js/friend/ErrorNumbers.msg"
#undef MSG_DEF
  JSErr_Limit
};

namespace js {

extern JS_PUBLIC_API const JSErrorFormatString* GetErrorMessage(
    void* userRef, unsigned errorNumber);

}  

#endif  // js_friend_ErrorMessages_h
