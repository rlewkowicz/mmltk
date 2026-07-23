/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef prlock_h___
#define prlock_h___

#include "prtypes.h"

PR_BEGIN_EXTERN_C



typedef struct PRLock PRLock;


NSPR_API(PRLock*) PR_NewLock(void);

NSPR_API(void) PR_DestroyLock(PRLock *lock);

NSPR_API(void) PR_Lock(PRLock *lock);

NSPR_API(PRStatus) PR_Unlock(PRLock *lock);

#if defined(DEBUG) || defined(FORCE_PR_ASSERT)
#define PR_ASSERT_CURRENT_THREAD_OWNS_LOCK(/* PrLock* */ lock) \
    PR_AssertCurrentThreadOwnsLock(lock)
#else
#define PR_ASSERT_CURRENT_THREAD_OWNS_LOCK(/* PrLock* */ lock)
#endif

NSPR_API(void) PR_AssertCurrentThreadOwnsLock(PRLock *lock);

PR_END_EXTERN_C

#endif /* prlock_h___ */
