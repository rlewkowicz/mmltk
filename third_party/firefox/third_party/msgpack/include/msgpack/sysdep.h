/*
 * MessagePack system dependencies
 *
 * Copyright (C) 2008-2010 FURUHASHI Sadayuki
 *
 *    Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *    http://www.boost.org/LICENSE_1_0.txt)
 */
#if !defined(MSGPACK_SYSDEP_H)
#define MSGPACK_SYSDEP_H

#include <arpa/inet.h>
#include <byteswap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if !defined(MSGPACK_DLLEXPORT)
#define MSGPACK_DLLEXPORT
#endif

typedef unsigned int _msgpack_atomic_counter_t;
#define _msgpack_sync_decr_and_fetch(ptr) __sync_sub_and_fetch(ptr, 1)
#define _msgpack_sync_incr_and_fetch(ptr) __sync_add_and_fetch(ptr, 1)

#define _msgpack_be16(x) ntohs(x)
#define _msgpack_be32(x) ntohl(x)
#define _msgpack_be64(x) bswap_64(x)
#define MSGPACK_ENDIAN_LITTLE_BYTE 1
#define MSGPACK_ENDIAN_BIG_BYTE 0

#define _msgpack_load16(cast, from, to) do {       \
        memcpy((cast*)(to), (from), sizeof(cast)); \
        *(to) = _msgpack_be16(*(to));              \
    } while (0);

#define _msgpack_load32(cast, from, to) do {       \
        memcpy((cast*)(to), (from), sizeof(cast)); \
        *(to) = _msgpack_be32(*(to));              \
    } while (0);
#define _msgpack_load64(cast, from, to) do {       \
        memcpy((cast*)(to), (from), sizeof(cast)); \
        *(to) = _msgpack_be64(*(to));              \
    } while (0);

#define _msgpack_store16(to, num) \
    do { uint16_t val = _msgpack_be16(num); memcpy(to, &val, 2); } while(0)
#define _msgpack_store32(to, num) \
    do { uint32_t val = _msgpack_be32(num); memcpy(to, &val, 4); } while(0)
#define _msgpack_store64(to, num) \
    do { uint64_t val = _msgpack_be64(num); memcpy(to, &val, 8); } while(0)
#endif
