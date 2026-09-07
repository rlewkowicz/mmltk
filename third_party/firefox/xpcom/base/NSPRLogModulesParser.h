/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef XPCOM_BASE_NSPRLOGMODULESPARSER_H_
#define XPCOM_BASE_NSPRLOGMODULESPARSER_H_

#include "mozilla/Logging.h"

#include <functional>

namespace mozilla {

void NSPRLogModulesParser(
    const char* aLogModules,
    const std::function<void(const char*, LogLevel, int32_t)>& aCallback);

}  

#endif  // XPCOM_BASE_NSPRLOGMODULESPARSER_H_
