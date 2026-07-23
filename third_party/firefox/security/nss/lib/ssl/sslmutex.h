/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(__SSLMUTEX_H_)
#define __SSLMUTEX_H_ 1


#include "prtypes.h"
#include "prlock.h"

#if defined(NETBSD)
#include <sys/param.h> /* for __NetBSD_Version__ */
#endif

#if defined(LINUX) || 0 || defined(BSDI) || \
    (defined(NETBSD) && __NetBSD_Version__ < 500000000) || defined(OPENBSD) || defined(__GLIBC__)

#include <sys/types.h>
#include "prtypes.h"

typedef struct {
    PRBool isMultiProcess;
    union {
        PRLock *sslLock;
        struct {
            int mPipes[3];
            PRInt32 nWaiters;
        } pipeStr;
    } u;
} sslMutex;
typedef pid_t sslPID;

#elif defined(XP_UNIX) && !0

#include <sys/types.h> /* for pid_t */
#include <semaphore.h> /* for sem_t, and sem_* functions */

typedef struct {
    PRBool isMultiProcess;
    union {
        PRLock *sslLock;
        sem_t sem;
    } u;
} sslMutex;

typedef pid_t sslPID;

#else


typedef struct {
    PRBool isMultiProcess;
    union {
        PRLock *sslLock;
    } u;
} sslMutex;

typedef int sslPID;

#endif

#include "seccomon.h"

SEC_BEGIN_PROTOS

extern SECStatus sslMutex_Init(sslMutex *sem, int shared);

extern SECStatus sslMutex_Destroy(sslMutex *sem, PRBool processLocal);

extern SECStatus sslMutex_Unlock(sslMutex *sem);

extern SECStatus sslMutex_Lock(sslMutex *sem);

#if defined(WINNT)

extern SECStatus sslMutex_2LevelInit(sslMutex *sem);

#endif

SEC_END_PROTOS

#endif
