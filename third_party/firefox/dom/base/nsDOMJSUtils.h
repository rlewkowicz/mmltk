/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsDOMJSUtils_h_
#define nsDOMJSUtils_h_

#include "js/TypeDecls.h"
#include "nscore.h"

class nsIJSArgArray;

nsresult NS_CreateJSArgv(JSContext* aContext, uint32_t aArgc,
                         const JS::Value* aArgv, nsIJSArgArray** aArray);

#endif  // nsDOMJSUtils_h_
