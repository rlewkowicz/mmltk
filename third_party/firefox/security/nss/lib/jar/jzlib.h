/* zlib.h -- interface of the 'zlib' general purpose compression library
  version 1.0.4, Jul 24th, 1996.

  Copyright (C) 1995-1996 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  gzip@prep.ai.mit.edu    madler@alumni.caltech.edu


  The data format used by the zlib library is described by RFCs (Request for
  Comments) 1950 to 1952 in the files ftp://ds.internic.net/rfc/rfc1950.txt
  (zlib format), rfc1951.txt (deflate format) and rfc1952.txt (gzip format).
*/

#ifndef _ZLIB_H
#define _ZLIB_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef MOZILLA_CLIENT
#include "jzconf.h"
#else
#include "zconf.h"
#endif

#define ZLIB_VERSION "1.0.4"


typedef voidpf(*alloc_func) OF((voidpf opaque, uInt items, uInt size));
typedef void(*free_func) OF((voidpf opaque, voidpf address));

struct internal_state;

typedef struct z_stream_s {
    Bytef *next_in; 
    uInt avail_in;  
    uLong total_in; 

    Bytef *next_out; 
    uInt avail_out;  
    uLong total_out; 

    char *msg;                        
    struct internal_state FAR *state; 

    alloc_func zalloc; 
    free_func zfree;   
    voidpf opaque;     

    int data_type;  
    uLong adler;    
    uLong reserved; 
} z_stream;

typedef z_stream FAR *z_streamp;



#define Z_NO_FLUSH 0
#define Z_PARTIAL_FLUSH 1
#define Z_SYNC_FLUSH 2
#define Z_FULL_FLUSH 3
#define Z_FINISH 4

#define Z_OK 0
#define Z_STREAM_END 1
#define Z_NEED_DICT 2
#define Z_ERRNO (-1)
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR (-3)
#define Z_MEM_ERROR (-4)
#define Z_BUF_ERROR (-5)
#define Z_VERSION_ERROR (-6)

#define Z_NO_COMPRESSION 0
#define Z_BEST_SPEED 1
#define Z_BEST_COMPRESSION 9
#define Z_DEFAULT_COMPRESSION (-1)

#define Z_FILTERED 1
#define Z_HUFFMAN_ONLY 2
#define Z_DEFAULT_STRATEGY 0

#define Z_BINARY 0
#define Z_ASCII 1
#define Z_UNKNOWN 2

#define Z_DEFLATED 8

#define Z_NULL 0 /* for initializing zalloc, zfree, opaque */

#define zlib_version zlibVersion()


#ifdef MOZILLA_CLIENT
PR_EXTERN(const char *)
zlibVersion(void);
#else
extern const char *EXPORT zlibVersion OF((void));
#endif


#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
deflate(z_streamp strm, int flush);
#else
extern int EXPORT deflate OF((z_streamp strm, int flush));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
deflateEnd(z_streamp strm);
#else
extern int EXPORT deflateEnd OF((z_streamp strm));
#endif


#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
inflate(z_streamp strm, int flush);
#else
extern int EXPORT inflate OF((z_streamp strm, int flush));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
inflateEnd(z_streamp strm);
#else
extern int EXPORT inflateEnd OF((z_streamp strm));
#endif




#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
deflateSetDictionary(z_streamp strm,
                     const Bytef *dictionary,
                     uInt dictLength);
#else
extern int EXPORT deflateSetDictionary OF((z_streamp strm,
                                           const Bytef *dictionary,
                                           uInt dictLength));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
deflateCopy(z_streamp dest, z_streamp source);
#else
extern int EXPORT deflateCopy OF((z_streamp dest, z_streamp source));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
deflateReset(z_streamp strm);
#else
extern int EXPORT deflateReset OF((z_streamp strm));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
deflateParams(z_streamp strm, int level, int strategy);
#else
extern int EXPORT deflateParams OF((z_streamp strm, int level, int strategy));
#endif


#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
inflateSetDictionary(z_streamp strm,
                     const Bytef *dictionary,
                     uInt dictLength);
#else
extern int EXPORT inflateSetDictionary OF((z_streamp strm,
                                           const Bytef *dictionary,
                                           uInt dictLength));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
inflateSync(z_streamp strm);
#else
extern int EXPORT inflateSync OF((z_streamp strm));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
inflateReset(z_streamp strm);
#else
extern int EXPORT inflateReset OF((z_streamp strm));
#endif



#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
compress(Bytef *dest, uLongf *destLen,
         const Bytef *source, uLong sourceLen);
#else
extern int EXPORT compress OF((Bytef * dest, uLongf *destLen,
                               const Bytef *source, uLong sourceLen));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
uncompress(Bytef *dest, uLongf *destLen,
           const Bytef *source, uLong sourceLen);
#else
extern int EXPORT uncompress OF((Bytef * dest, uLongf *destLen,
                                 const Bytef *source, uLong sourceLen));
#endif

typedef voidp gzFile;

#ifdef MOZILLA_CLIENT
PR_EXTERN(gzFile)
gzopen(const char *path, const char *mode);
#else
extern gzFile EXPORT gzopen OF((const char *path, const char *mode));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(gzFile)
gzdopen(int fd, const char *mode);
#else
extern gzFile EXPORT gzdopen OF((int fd, const char *mode));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
gzread(gzFile file, voidp buf, unsigned len);
#else
extern int EXPORT gzread OF((gzFile file, voidp buf, unsigned len));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
gzwrite(gzFile file, const voidp buf, unsigned len);
#else
extern int EXPORT gzwrite OF((gzFile file, const voidp buf, unsigned len));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
gzflush(gzFile file, int flush);
#else
extern int EXPORT gzflush OF((gzFile file, int flush));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
gzclose(gzFile file);
#else
extern int EXPORT gzclose OF((gzFile file));
#endif

#ifdef MOZILLA_CLIENT
PR_EXTERN(const char *)
gzerror(gzFile file, int *errnum);
#else
extern const char *EXPORT gzerror OF((gzFile file, int *errnum));
#endif



#ifdef MOZILLA_CLIENT
PR_EXTERN(uLong)
adler32(uLong adler, const Bytef *buf, uInt len);
#else
extern uLong EXPORT adler32 OF((uLong adler, const Bytef *buf, uInt len));
#endif


#ifdef MOZILLA_CLIENT
PR_EXTERN(uLong)
crc32(uLong crc, const Bytef *buf, uInt len);
#else
extern uLong EXPORT crc32 OF((uLong crc, const Bytef *buf, uInt len));
#endif


#ifdef MOZILLA_CLIENT
PR_EXTERN(int)
deflateInit(z_streamp strm, int level, const char *version,
            int stream_size);
PR_EXTERN(int)
inflateInit_(z_streamp strm, const char *version,
             int stream_size);
PR_EXTERN(int)
deflateInit2_(z_streamp strm, int level, int method,
              int windowBits, int memLevel, int strategy,
              const char *version, int stream_size);
PR_EXTERN(int)
inflateInit2_(z_streamp strm, int windowBits,
              const char *version, int stream_size);
#else
extern int EXPORT deflateInit_ OF((z_streamp strm, int level, const char *version,
                                   int stream_size));
extern int EXPORT inflateInit_ OF((z_streamp strm, const char *version,
                                   int stream_size));
extern int EXPORT deflateInit2_ OF((z_streamp strm, int level, int method,
                                    int windowBits, int memLevel, int strategy,
                                    const char *version, int stream_size));
extern int EXPORT inflateInit2_ OF((z_streamp strm, int windowBits,
                                    const char *version, int stream_size));
#endif /* MOZILLA_CLIENT */

#define deflateInit(strm, level) \
    deflateInit_((strm), (level), ZLIB_VERSION, sizeof(z_stream))
#define inflateInit(strm) \
    inflateInit_((strm), ZLIB_VERSION, sizeof(z_stream))
#define deflateInit2(strm, level, method, windowBits, memLevel, strategy) \
    deflateInit2_((strm), (level), (method), (windowBits), (memLevel),    \
                  (strategy), ZLIB_VERSION, sizeof(z_stream))
#define inflateInit2(strm, windowBits) \
    inflateInit2_((strm), (windowBits), ZLIB_VERSION, sizeof(z_stream))

#if !defined(_Z_UTIL_H) && !defined(NO_DUMMY_DECL)
struct internal_state {
    int dummy;
}; 
#endif

uLongf *get_crc_table OF((void)); 

#ifdef __cplusplus
}
#endif

#endif /* _ZLIB_H */
