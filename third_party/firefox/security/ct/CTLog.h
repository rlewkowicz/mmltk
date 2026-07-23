/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CTLog_h
#define CTLog_h

#include <stdint.h>
#include <vector>

namespace mozilla {
namespace ct {

typedef int16_t CTLogOperatorId;

typedef std::vector<CTLogOperatorId> CTLogOperatorList;

}  
}  

#endif  // CTLog_h
