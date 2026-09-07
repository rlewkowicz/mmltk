/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _include_ipc_glue_UtilityProcessTypes_h_
#define _include_ipc_glue_UtilityProcessTypes_h_

#include <stdint.h>

namespace mozilla {

namespace ipc {

enum UtilityProcessKind : uint64_t {
  GENERIC_UTILITY,
  COUNT,
};

}  

}  

#endif  // _include_ipc_glue_UtilityProcessTypes_h_
