/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "primpl.h"

#include <string.h>


static void _pr_ConvertSemName(char* result) {
#if defined(_PR_HAVE_POSIX_SEMAPHORES)
  return;
#elif defined(_PR_HAVE_SYSV_SEMAPHORES)
  return;
#endif
}

static void _pr_ConvertShmName(char* result) {
#if defined(PR_HAVE_POSIX_NAMED_SHARED_MEMORY)
  return;
#elif defined(PR_HAVE_SYSV_NAMED_SHARED_MEMORY)
  return;
#else
  return;
#endif
}

PRStatus _PR_MakeNativeIPCName(const char* name, char* result, PRIntn size,
                               _PRIPCType type) {
  if (strlen(name) >= (PRSize)size) {
    PR_SetError(PR_BUFFER_OVERFLOW_ERROR, 0);
    return PR_FAILURE;
  }
  strcpy(result, name);
  switch (type) {
    case _PRIPCSem:
      _pr_ConvertSemName(result);
      break;
    case _PRIPCShm:
      _pr_ConvertShmName(result);
      break;
    default:
      PR_SetError(PR_INVALID_ARGUMENT_ERROR, 0);
      return PR_FAILURE;
  }
  return PR_SUCCESS;
}
