/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(prinet_h__)
#define prinet_h__

#if defined(XP_UNIX)
#include <sys/types.h>
#include <sys/socket.h>     /* AF_INET */
#include <netinet/in.h>         /* INADDR_ANY, ..., ntohl(), ... */
#if defined(XP_UNIX)
#include <arpa/inet.h>
#endif
#include <netdb.h>

#if defined(QNX)
#include <rpc/types.h> /* the only place that defines INADDR_LOOPBACK */
#endif

#if !defined(INADDR_LOOPBACK)
#define INADDR_LOOPBACK 0x7f000001
#endif


#else

#error Unknown platform

#endif

#endif
