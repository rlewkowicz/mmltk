/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#if !defined(SPA_ENDIAN_H)
#define SPA_ENDIAN_H

#if 0 || defined(__MidnightBSD__)
#include <sys/endian.h>
#define bswap_16 bswap16
#define bswap_32 bswap32
#define bswap_64 bswap64
#else
#include <endian.h>
#include <byteswap.h>
#endif

#endif
