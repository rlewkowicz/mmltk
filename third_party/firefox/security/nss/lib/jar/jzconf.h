/* zconf.h -- configuration of the zlib compression library
 * Copyright (C) 1995-1996 Jean-loup Gailly.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#if !defined(_ZCONF_H)
#define _ZCONF_H

#if defined(Z_PREFIX)
#define deflateInit_ z_deflateInit_
#define deflate z_deflate
#define deflateEnd z_deflateEnd
#define inflateInit_ z_inflateInit_
#define inflate z_inflate
#define inflateEnd z_inflateEnd
#define deflateInit2_ z_deflateInit2_
#define deflateSetDictionary z_deflateSetDictionary
#define deflateCopy z_deflateCopy
#define deflateReset z_deflateReset
#define deflateParams z_deflateParams
#define inflateInit2_ z_inflateInit2_
#define inflateSetDictionary z_inflateSetDictionary
#define inflateSync z_inflateSync
#define inflateReset z_inflateReset
#define compress z_compress
#define uncompress z_uncompress
#define adler32 z_adler32
#define crc32 z_crc32
#define get_crc_table z_get_crc_table

#define Byte z_Byte
#define uInt z_uInt
#define uLong z_uLong
#define Bytef z_Bytef
#define charf z_charf
#define intf z_intf
#define uIntf z_uIntf
#define uLongf z_uLongf
#define voidpf z_voidpf
#define voidp z_voidp
#endif

#if defined(__GNUC__) || 0 || defined(__386__) || defined(i386)
#if !defined(__32BIT__)
#define __32BIT__
#endif
#endif
#if defined(__MSDOS__) && !defined(MSDOS)
#define MSDOS
#endif

#if defined(MSDOS) && !defined(__32BIT__)
#define MAXSEG_64K
#endif
#if defined(MSDOS)
#define UNALIGNED_OK
#endif

#if (defined(MSDOS) || 0 || 0) && !defined(STDC)
#define STDC
#endif
#if (defined(__STDC__) || defined(__cplusplus)) && !defined(STDC)
#define STDC
#endif

#if !defined(STDC)
#if !defined(const)
#define const
#endif
#endif

#if defined(__MWERKS__) || defined(applec) || defined(THINK_C) || defined(__SC__)
#define NO_DUMMY_DECL
#endif

#if !defined(MAX_MEM_LEVEL)
#if defined(MAXSEG_64K)
#define MAX_MEM_LEVEL 8
#else
#define MAX_MEM_LEVEL 9
#endif
#endif

#if !defined(MAX_WBITS)
#define MAX_WBITS 15 /* 32K LZ77 window */
#endif



#if !defined(OF)
#if defined(STDC)
#define OF(args) args
#else
#define OF(args) ()
#endif
#endif

#if (defined(M_I86SM) || defined(M_I86MM)) && !defined(__32BIT__)
#define SMALL_MEDIUM
#if defined(_MSC_VER)
#define FAR __far
#else
#define FAR far
#endif
#endif
#if defined(__BORLANDC__) && (defined(__SMALL__) || defined(__MEDIUM__))
#if !defined(__32BIT__)
#define SMALL_MEDIUM
#define FAR __far
#endif
#endif
#if !defined(FAR)
#define FAR
#endif

typedef unsigned char Byte;  
typedef unsigned int uInt;   
typedef unsigned long uLong; 

#if defined(__BORLANDC__) && defined(SMALL_MEDIUM)
#define Bytef Byte FAR
#else
typedef Byte FAR Bytef;
#endif
typedef char FAR charf;
typedef int FAR intf;
typedef uInt FAR uIntf;
typedef uLong FAR uLongf;

#if defined(STDC)
typedef void FAR *voidpf;
typedef void *voidp;
#else
typedef Byte FAR *voidpf;
typedef Byte *voidp;
#endif

#if defined(MOZILLA_CLIENT)
#include "prtypes.h"
#else
#if (0 || defined(WINDOWS)) && defined(ZLIB_DLL)
#include <windows.h>
#define EXPORT WINAPI
#else
#define EXPORT
#endif

#define PR_PUBLIC_API(type) type

#endif

#endif
