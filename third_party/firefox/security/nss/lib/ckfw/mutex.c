/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef CK_T
#include "ck.h"
#endif /* CK_T */


struct NSSCKFWMutexStr {
    PRLock *lock;
};

#ifdef DEBUG

static CK_RV
mutex_add_pointer(
    const NSSCKFWMutex *fwMutex)
{
    return CKR_OK;
}

static CK_RV
mutex_remove_pointer(
    const NSSCKFWMutex *fwMutex)
{
    return CKR_OK;
}

NSS_IMPLEMENT CK_RV
nssCKFWMutex_verifyPointer(
    const NSSCKFWMutex *fwMutex)
{
    return CKR_OK;
}

#endif /* DEBUG */

NSS_EXTERN NSSCKFWMutex *
nssCKFWMutex_Create(
    CK_C_INITIALIZE_ARGS_PTR pInitArgs,
    CryptokiLockingState LockingState,
    NSSArena *arena,
    CK_RV *pError)
{
    NSSCKFWMutex *mutex;

    mutex = nss_ZNEW(arena, NSSCKFWMutex);
    if (!mutex) {
        *pError = CKR_HOST_MEMORY;
        return (NSSCKFWMutex *)NULL;
    }
    *pError = CKR_OK;
    mutex->lock = NULL;
    if (LockingState == MultiThreaded) {
        mutex->lock = PR_NewLock();
        if (!mutex->lock) {
            *pError = CKR_HOST_MEMORY; 
        }
    }

    if (CKR_OK != *pError) {
        (void)nss_ZFreeIf(mutex);
        return (NSSCKFWMutex *)NULL;
    }

#ifdef DEBUG
    *pError = mutex_add_pointer(mutex);
    if (CKR_OK != *pError) {
        if (mutex->lock) {
            PR_DestroyLock(mutex->lock);
        }
        (void)nss_ZFreeIf(mutex);
        return (NSSCKFWMutex *)NULL;
    }
#endif /* DEBUG */

    return mutex;
}

NSS_EXTERN CK_RV
nssCKFWMutex_Destroy(
    NSSCKFWMutex *mutex)
{
    CK_RV rv = CKR_OK;

#ifdef NSSDEBUG
    rv = nssCKFWMutex_verifyPointer(mutex);
    if (CKR_OK != rv) {
        return rv;
    }
#endif /* NSSDEBUG */

    if (mutex->lock) {
        PR_DestroyLock(mutex->lock);
    }

#ifdef DEBUG
    (void)mutex_remove_pointer(mutex);
#endif /* DEBUG */

    (void)nss_ZFreeIf(mutex);
    return rv;
}

NSS_EXTERN CK_RV
nssCKFWMutex_Lock(
    NSSCKFWMutex *mutex)
{
#ifdef NSSDEBUG
    CK_RV rv = nssCKFWMutex_verifyPointer(mutex);
    if (CKR_OK != rv) {
        return rv;
    }
#endif /* NSSDEBUG */
    if (mutex->lock) {
        PR_Lock(mutex->lock);
    }

    return CKR_OK;
}

NSS_EXTERN CK_RV
nssCKFWMutex_Unlock(
    NSSCKFWMutex *mutex)
{
    PRStatus nrv;
#ifdef NSSDEBUG
    CK_RV rv = nssCKFWMutex_verifyPointer(mutex);

    if (CKR_OK != rv) {
        return rv;
    }
#endif /* NSSDEBUG */

    if (!mutex->lock)
        return CKR_OK;

    nrv = PR_Unlock(mutex->lock);

    return nrv == PR_SUCCESS ? CKR_OK : CKR_DEVICE_ERROR;
}

NSS_EXTERN CK_RV
NSSCKFWMutex_Destroy(
    NSSCKFWMutex *mutex)
{
#ifdef DEBUG
    CK_RV rv = nssCKFWMutex_verifyPointer(mutex);
    if (CKR_OK != rv) {
        return rv;
    }
#endif /* DEBUG */

    return nssCKFWMutex_Destroy(mutex);
}

NSS_EXTERN CK_RV
NSSCKFWMutex_Lock(
    NSSCKFWMutex *mutex)
{
#ifdef DEBUG
    CK_RV rv = nssCKFWMutex_verifyPointer(mutex);
    if (CKR_OK != rv) {
        return rv;
    }
#endif /* DEBUG */

    return nssCKFWMutex_Lock(mutex);
}

NSS_EXTERN CK_RV
NSSCKFWMutex_Unlock(
    NSSCKFWMutex *mutex)
{
#ifdef DEBUG
    CK_RV rv = nssCKFWMutex_verifyPointer(mutex);
    if (CKR_OK != rv) {
        return rv;
    }
#endif /* DEBUG */

    return nssCKFWMutex_Unlock(mutex);
}
