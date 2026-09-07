/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if defined(_PR_PTHREADS)

#  include "primpl.h"

#  include <stdio.h>

#  define PT_LOG(f)

void _PR_InitCPUs(void) { PT_LOG("_PR_InitCPUs") }
void _PR_InitStacks(void){PT_LOG("_PR_InitStacks")}

PR_IMPLEMENT(void) PR_SetConcurrency(PRUintn numCPUs) {
  PT_LOG("PR_SetConcurrency");
}

PR_IMPLEMENT(void) PR_SetThreadRecycleMode(PRUint32 flag) {
  PT_LOG("PR_SetThreadRecycleMode")
}

#endif

