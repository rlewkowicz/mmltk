/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef prcmon_h___
#define prcmon_h___

#include "prmon.h"
#include "prinrval.h"

PR_BEGIN_EXTERN_C

NSPR_API(PRMonitor*) PR_CEnterMonitor(void *address);

NSPR_API(PRStatus) PR_CExitMonitor(void *address);

NSPR_API(PRStatus) PR_CWait(void *address, PRIntervalTime timeout);

NSPR_API(PRStatus) PR_CNotify(void *address);

NSPR_API(PRStatus) PR_CNotifyAll(void *address);

NSPR_API(void) PR_CSetOnMonitorRecycle(void (PR_CALLBACK *callback)(void *address));

PR_END_EXTERN_C

#endif /* prcmon_h___ */
