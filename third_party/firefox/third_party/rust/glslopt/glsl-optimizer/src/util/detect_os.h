/* SPDX-License-Identifier: MIT */
/* Copyright 2008 VMware, Inc. */


#if !defined(DETECT_OS_H)
#define DETECT_OS_H

#if defined(__linux__)
#define DETECT_OS_LINUX 1
#define DETECT_OS_UNIX 1
#endif


#if 0 || defined(__FreeBSD_kernel__)
#define DETECT_OS_FREEBSD 1
#define DETECT_OS_BSD 1
#define DETECT_OS_UNIX 1
#endif




#if defined(__GNU__)
#define DETECT_OS_HURD 1
#define DETECT_OS_UNIX 1
#endif







#if !defined(DETECT_OS_ANDROID)
#define DETECT_OS_ANDROID 0
#endif
#if !defined(DETECT_OS_APPLE)
#define DETECT_OS_APPLE 0
#endif
#if !defined(DETECT_OS_BSD)
#define DETECT_OS_BSD 0
#endif
#if !defined(DETECT_OS_CYGWIN)
#define DETECT_OS_CYGWIN 0
#endif
#if !defined(DETECT_OS_DRAGONFLY)
#define DETECT_OS_DRAGONFLY 0
#endif
#if !defined(DETECT_OS_FREEBSD)
#define DETECT_OS_FREEBSD 0
#endif
#if !defined(DETECT_OS_HAIKU)
#define DETECT_OS_HAIKU 0
#endif
#if !defined(DETECT_OS_HURD)
#define DETECT_OS_HURD 0
#endif
#if !defined(DETECT_OS_LINUX)
#define DETECT_OS_LINUX 0
#endif
#if !defined(DETECT_OS_NETBSD)
#define DETECT_OS_NETBSD 0
#endif
#if !defined(DETECT_OS_OPENBSD)
#define DETECT_OS_OPENBSD 0
#endif
#if !defined(DETECT_OS_SOLARIS)
#define DETECT_OS_SOLARIS 0
#endif
#if !defined(DETECT_OS_UNIX)
#define DETECT_OS_UNIX 0
#endif
#if !defined(DETECT_OS_WINDOWS)
#define DETECT_OS_WINDOWS 0
#endif

#endif
