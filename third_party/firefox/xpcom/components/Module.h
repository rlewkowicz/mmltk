/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_Module_h
#define mozilla_Module_h

#include "nscore.h"

namespace mozilla {

namespace Module {
enum ProcessSelector {
  ANY_PROCESS = 0,
  MAIN_PROCESS_ONLY = 1 << 0,
  CONTENT_PROCESS_ONLY = 1 << 1,

  ALLOW_IN_GPU_PROCESS = 1 << 2,
  ALLOW_IN_SOCKET_PROCESS = 1 << 4,
  ALLOW_IN_RDD_PROCESS = 1 << 5,
  ALLOW_IN_UTILITY_PROCESS = 1 << 6,
  ALLOW_IN_GPU_AND_MAIN_PROCESS = ALLOW_IN_GPU_PROCESS | MAIN_PROCESS_ONLY,
  ALLOW_IN_GPU_AND_SOCKET_PROCESS =
      ALLOW_IN_GPU_PROCESS | ALLOW_IN_SOCKET_PROCESS,
  ALLOW_IN_RDD_AND_SOCKET_PROCESS =
      ALLOW_IN_RDD_PROCESS | ALLOW_IN_SOCKET_PROCESS,
  ALLOW_IN_GPU_RDD_AND_SOCKET_PROCESS =
      ALLOW_IN_GPU_PROCESS | ALLOW_IN_RDD_PROCESS | ALLOW_IN_SOCKET_PROCESS,
  ALLOW_IN_GPU_RDD_SOCKET_AND_UTILITY_PROCESS =
      ALLOW_IN_GPU_PROCESS | ALLOW_IN_RDD_PROCESS | ALLOW_IN_SOCKET_PROCESS |
      ALLOW_IN_UTILITY_PROCESS,
};

static constexpr size_t kMaxProcessSelector = size_t(
    ProcessSelector::ALLOW_IN_GPU_RDD_SOCKET_AND_UTILITY_PROCESS);

enum BackgroundTasksSelector {
  NO_TASKS = 0x0,
  ALL_TASKS = 0xFFFF,
};
};  

}  

#endif  // mozilla_Module_h
