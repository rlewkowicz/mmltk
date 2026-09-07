/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(pprio_h___)
#define pprio_h___

#include "prtypes.h"
#include "prio.h"

PR_BEGIN_EXTERN_C


typedef PRInt32 PROsfd;

NSPR_API(const PRIOMethods*)    PR_GetFileMethods(void);
NSPR_API(const PRIOMethods*)    PR_GetTCPMethods(void);
NSPR_API(const PRIOMethods*)    PR_GetUDPMethods(void);
NSPR_API(const PRIOMethods*)    PR_GetPipeMethods(void);

NSPR_API(PROsfd)       PR_FileDesc2NativeHandle(PRFileDesc *);
NSPR_API(void)         PR_ChangeFileDescNativeHandle(PRFileDesc *, PROsfd);
NSPR_API(PRFileDesc*)  PR_AllocFileDesc(PROsfd osfd,
                                        const PRIOMethods *methods);
NSPR_API(void)         PR_FreeFileDesc(PRFileDesc *fd);
NSPR_API(PRFileDesc*)  PR_ImportFile(PROsfd osfd);
NSPR_API(PRFileDesc*)  PR_ImportPipe(PROsfd osfd);
NSPR_API(PRFileDesc*)  PR_ImportTCPSocket(PROsfd osfd);
NSPR_API(PRFileDesc*)  PR_ImportUDPSocket(PROsfd osfd);



NSPR_API(PRFileDesc*)   PR_CreateSocketPollFd(PROsfd osfd);


NSPR_API(PRStatus) PR_DestroySocketPollFd(PRFileDesc *fd);




#define PR_SOCK_STREAM SOCK_STREAM
#define PR_SOCK_DGRAM SOCK_DGRAM


NSPR_API(PRFileDesc*)   PR_Socket(PRInt32 domain, PRInt32 type, PRInt32 proto);

NSPR_API(PRStatus) PR_LockFile(PRFileDesc *fd);

NSPR_API(PRStatus) PR_TLockFile(PRFileDesc *fd);

NSPR_API(PRStatus) PR_UnlockFile(PRFileDesc *fd);

NSPR_API(PRInt32) PR_EmulateAcceptRead(PRFileDesc *sd, PRFileDesc **nd,
                                       PRNetAddr **raddr, void *buf, PRInt32 amount, PRIntervalTime timeout);

NSPR_API(PRInt32) PR_EmulateSendFile(
    PRFileDesc *networkSocket, PRSendFileData *sendData,
    PRTransmitFileFlags flags, PRIntervalTime timeout);


PR_END_EXTERN_C

#endif
