/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef prrwlock_h___
#define prrwlock_h___

#include "prtypes.h"

PR_BEGIN_EXTERN_C



typedef struct PRRWLock PRRWLock;

#define PR_RWLOCK_RANK_NONE 0


NSPR_API(PRRWLock*) PR_NewRWLock(PRUint32 lock_rank, const char *lock_name);

NSPR_API(void) PR_DestroyRWLock(PRRWLock *lock);

NSPR_API(void) PR_RWLock_Rlock(PRRWLock *lock);

NSPR_API(void) PR_RWLock_Wlock(PRRWLock *lock);

NSPR_API(void) PR_RWLock_Unlock(PRRWLock *lock);

PR_END_EXTERN_C

#endif /* prrwlock_h___ */
