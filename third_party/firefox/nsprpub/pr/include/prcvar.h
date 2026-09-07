/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef prcvar_h___
#define prcvar_h___

#include "prlock.h"
#include "prinrval.h"

PR_BEGIN_EXTERN_C

typedef struct PRCondVar PRCondVar;

NSPR_API(PRCondVar*) PR_NewCondVar(PRLock *lock);

NSPR_API(void) PR_DestroyCondVar(PRCondVar *cvar);

NSPR_API(PRStatus) PR_WaitCondVar(PRCondVar *cvar, PRIntervalTime timeout);

NSPR_API(PRStatus) PR_NotifyCondVar(PRCondVar *cvar);

NSPR_API(PRStatus) PR_NotifyAllCondVar(PRCondVar *cvar);

PR_END_EXTERN_C

#endif /* prcvar_h___ */
