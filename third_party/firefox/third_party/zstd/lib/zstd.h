/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#if !defined(ZSTD_H_235446)
#define ZSTD_H_235446


#include <stddef.h>   /* size_t */

#include "zstd_errors.h" /* list of errors */
#if defined(ZSTD_STATIC_LINKING_ONLY) && !defined(ZSTD_H_ZSTD_STATIC_LINKING_ONLY)
#include <limits.h>   /* INT_MAX */
#endif

#if defined (__cplusplus)
extern "C" {
#endif

#if !defined(ZSTDLIB_VISIBLE)
#if defined(ZSTDLIB_VISIBILITY)
#    define ZSTDLIB_VISIBLE ZSTDLIB_VISIBILITY
#elif defined(__GNUC__) && (__GNUC__ >= 4) && !defined(__MINGW32__)
#    define ZSTDLIB_VISIBLE __attribute__ ((visibility ("default")))
#else
#    define ZSTDLIB_VISIBLE
#endif
#endif

#if !defined(ZSTDLIB_HIDDEN)
#if defined(__GNUC__) && (__GNUC__ >= 4) && !defined(__MINGW32__)
#    define ZSTDLIB_HIDDEN __attribute__ ((visibility ("hidden")))
#else
#    define ZSTDLIB_HIDDEN
#endif
#endif

#if defined(ZSTD_DLL_EXPORT) && (ZSTD_DLL_EXPORT==1)
#  define ZSTDLIB_API __declspec(dllexport) ZSTDLIB_VISIBLE
#elif defined(ZSTD_DLL_IMPORT) && (ZSTD_DLL_IMPORT==1)
#  define ZSTDLIB_API __declspec(dllimport) ZSTDLIB_VISIBLE /* It isn't required but allows to generate better code, saving a function pointer load from the IAT and an indirect jump.*/
#else
#  define ZSTDLIB_API ZSTDLIB_VISIBLE
#endif

#if defined(ZSTD_DISABLE_DEPRECATE_WARNINGS)
#  define ZSTD_DEPRECATED(message) /* disable deprecation warnings */
#else
#if defined (__cplusplus) && (__cplusplus >= 201402) /* C++14 or greater */
#    define ZSTD_DEPRECATED(message) [[deprecated(message)]]
#elif (defined(GNUC) && (GNUC > 4 || (GNUC == 4 && GNUC_MINOR >= 5))) || defined(__clang__) || defined(__IAR_SYSTEMS_ICC__)
#    define ZSTD_DEPRECATED(message) __attribute__((deprecated(message)))
#elif defined(__GNUC__) && (__GNUC__ >= 3)
#    define ZSTD_DEPRECATED(message) __attribute__((deprecated))
#elif defined(_MSC_VER)
#    define ZSTD_DEPRECATED(message) __declspec(deprecated(message))
#else
#    pragma message("WARNING: You need to implement ZSTD_DEPRECATED for this compiler")
#    define ZSTD_DEPRECATED(message)
#endif
#endif



#define ZSTD_VERSION_MAJOR    1
#define ZSTD_VERSION_MINOR    5
#define ZSTD_VERSION_RELEASE  7
#define ZSTD_VERSION_NUMBER  (ZSTD_VERSION_MAJOR *100*100 + ZSTD_VERSION_MINOR *100 + ZSTD_VERSION_RELEASE)

/*! ZSTD_versionNumber() :
 *  Return runtime library version, the value is (MAJOR*100*100 + MINOR*100 + RELEASE). */
ZSTDLIB_API unsigned ZSTD_versionNumber(void);

#define ZSTD_LIB_VERSION ZSTD_VERSION_MAJOR.ZSTD_VERSION_MINOR.ZSTD_VERSION_RELEASE
#define ZSTD_QUOTE(str) #str
#define ZSTD_EXPAND_AND_QUOTE(str) ZSTD_QUOTE(str)
#define ZSTD_VERSION_STRING ZSTD_EXPAND_AND_QUOTE(ZSTD_LIB_VERSION)

/*! ZSTD_versionString() :
 *  Return runtime library version, like "1.4.5". Requires v1.3.0+. */
ZSTDLIB_API const char* ZSTD_versionString(void);

#if !defined(ZSTD_CLEVEL_DEFAULT)
#  define ZSTD_CLEVEL_DEFAULT 3
#endif


#define ZSTD_MAGICNUMBER            0xFD2FB528    /* valid since v0.8.0 */
#define ZSTD_MAGIC_DICTIONARY       0xEC30A437    /* valid since v0.7.0 */
#define ZSTD_MAGIC_SKIPPABLE_START  0x184D2A50    /* all 16 values, from 0x184D2A50 to 0x184D2A5F, signal the beginning of a skippable frame */
#define ZSTD_MAGIC_SKIPPABLE_MASK   0xFFFFFFF0

#define ZSTD_BLOCKSIZELOG_MAX  17
#define ZSTD_BLOCKSIZE_MAX     (1<<ZSTD_BLOCKSIZELOG_MAX)


/*! ZSTD_compress() :
 *  Compresses `src` content as a single zstd compressed frame into already allocated `dst`.
 *  NOTE: Providing `dstCapacity >= ZSTD_compressBound(srcSize)` guarantees that zstd will have
 *        enough space to successfully compress the data.
 *  @return : compressed size written into `dst` (<= `dstCapacity),
 *            or an error code if it fails (which can be tested using ZSTD_isError()). */
ZSTDLIB_API size_t ZSTD_compress( void* dst, size_t dstCapacity,
                            const void* src, size_t srcSize,
                                  int compressionLevel);

/*! ZSTD_decompress() :
 * `compressedSize` : must be the _exact_ size of some number of compressed and/or skippable frames.
 *  Multiple compressed frames can be decompressed at once with this method.
 *  The result will be the concatenation of all decompressed frames, back to back.
 * `dstCapacity` is an upper bound of originalSize to regenerate.
 *  First frame's decompressed size can be extracted using ZSTD_getFrameContentSize().
 *  If maximum upper bound isn't known, prefer using streaming mode to decompress data.
 * @return : the number of bytes decompressed into `dst` (<= `dstCapacity`),
 *           or an errorCode if it fails (which can be tested using ZSTD_isError()). */
ZSTDLIB_API size_t ZSTD_decompress( void* dst, size_t dstCapacity,
                              const void* src, size_t compressedSize);



/*! ZSTD_getFrameContentSize() : requires v1.3.0+
 * `src` should point to the start of a ZSTD encoded frame.
 * `srcSize` must be at least as large as the frame header.
 *           hint : any size >= `ZSTD_frameHeaderSize_max` is large enough.
 * @return : - decompressed size of `src` frame content, if known
 *           - ZSTD_CONTENTSIZE_UNKNOWN if the size cannot be determined
 *           - ZSTD_CONTENTSIZE_ERROR if an error occurred (e.g. invalid magic number, srcSize too small)
 *  note 1 : a 0 return value means the frame is valid but "empty".
 *           When invoking this method on a skippable frame, it will return 0.
 *  note 2 : decompressed size is an optional field, it may not be present (typically in streaming mode).
 *           When `return==ZSTD_CONTENTSIZE_UNKNOWN`, data to decompress could be any size.
 *           In which case, it's necessary to use streaming mode to decompress data.
 *           Optionally, application can rely on some implicit limit,
 *           as ZSTD_decompress() only needs an upper bound of decompressed size.
 *           (For example, data could be necessarily cut into blocks <= 16 KB).
 *  note 3 : decompressed size is always present when compression is completed using single-pass functions,
 *           such as ZSTD_compress(), ZSTD_compressCCtx() ZSTD_compress_usingDict() or ZSTD_compress_usingCDict().
 *  note 4 : decompressed size can be very large (64-bits value),
 *           potentially larger than what local system can handle as a single memory segment.
 *           In which case, it's necessary to use streaming mode to decompress data.
 *  note 5 : If source is untrusted, decompressed size could be wrong or intentionally modified.
 *           Always ensure return value fits within application's authorized limits.
 *           Each application can set its own limits.
 *  note 6 : This function replaces ZSTD_getDecompressedSize() */
#define ZSTD_CONTENTSIZE_UNKNOWN (0ULL - 1)
#define ZSTD_CONTENTSIZE_ERROR   (0ULL - 2)
ZSTDLIB_API unsigned long long ZSTD_getFrameContentSize(const void *src, size_t srcSize);

/*! ZSTD_getDecompressedSize() (obsolete):
 *  This function is now obsolete, in favor of ZSTD_getFrameContentSize().
 *  Both functions work the same way, but ZSTD_getDecompressedSize() blends
 *  "empty", "unknown" and "error" results to the same return value (0),
 *  while ZSTD_getFrameContentSize() gives them separate return values.
 * @return : decompressed size of `src` frame content _if known and not empty_, 0 otherwise. */
ZSTD_DEPRECATED("Replaced by ZSTD_getFrameContentSize")
ZSTDLIB_API unsigned long long ZSTD_getDecompressedSize(const void* src, size_t srcSize);

/*! ZSTD_findFrameCompressedSize() : Requires v1.4.0+
 * `src` should point to the start of a ZSTD frame or skippable frame.
 * `srcSize` must be >= first frame size
 * @return : the compressed size of the first frame starting at `src`,
 *           suitable to pass as `srcSize` to `ZSTD_decompress` or similar,
 *           or an error code if input is invalid
 *  Note 1: this method is called _find*() because it's not enough to read the header,
 *          it may have to scan through the frame's content, to reach its end.
 *  Note 2: this method also works with Skippable Frames. In which case,
 *          it returns the size of the complete skippable frame,
 *          which is always equal to its content size + 8 bytes for headers. */
ZSTDLIB_API size_t ZSTD_findFrameCompressedSize(const void* src, size_t srcSize);



/*! ZSTD_compressBound() :
 * maximum compressed size in worst case single-pass scenario.
 * When invoking `ZSTD_compress()`, or any other one-pass compression function,
 * it's recommended to provide @dstCapacity >= ZSTD_compressBound(srcSize)
 * as it eliminates one potential failure scenario,
 * aka not enough room in dst buffer to write the compressed frame.
 * Note : ZSTD_compressBound() itself can fail, if @srcSize >= ZSTD_MAX_INPUT_SIZE .
 *        In which case, ZSTD_compressBound() will return an error code
 *        which can be tested using ZSTD_isError().
 *
 * ZSTD_COMPRESSBOUND() :
 * same as ZSTD_compressBound(), but as a macro.
 * It can be used to produce constants, which can be useful for static allocation,
 * for example to size a static array on stack.
 * Will produce constant value 0 if srcSize is too large.
 */
#define ZSTD_MAX_INPUT_SIZE ((sizeof(size_t)==8) ? 0xFF00FF00FF00FF00ULL : 0xFF00FF00U)
#define ZSTD_COMPRESSBOUND(srcSize)   (((size_t)(srcSize) >= ZSTD_MAX_INPUT_SIZE) ? 0 : (srcSize) + ((srcSize)>>8) + (((srcSize) < (128<<10)) ? (((128<<10) - (srcSize)) >> 11) /* margin, from 64 to 0 */ : 0))  /* this formula ensures that bound(A) + bound(B) <= bound(A+B) as long as A and B >= 128 KB */
ZSTDLIB_API size_t ZSTD_compressBound(size_t srcSize); /*!< maximum compressed size in worst case single-pass scenario */


ZSTDLIB_API unsigned     ZSTD_isError(size_t result);      /*!< tells if a `size_t` function result is an error code */
ZSTDLIB_API ZSTD_ErrorCode ZSTD_getErrorCode(size_t functionResult); 
ZSTDLIB_API const char*  ZSTD_getErrorName(size_t result); /*!< provides readable string from a function result */
ZSTDLIB_API int          ZSTD_minCLevel(void);             /*!< minimum negative compression level allowed, requires v1.4.0+ */
ZSTDLIB_API int          ZSTD_maxCLevel(void);             /*!< maximum compression level available */
ZSTDLIB_API int          ZSTD_defaultCLevel(void);         /*!< default compression level, specified by ZSTD_CLEVEL_DEFAULT, requires v1.5.0+ */


typedef struct ZSTD_CCtx_s ZSTD_CCtx;
ZSTDLIB_API ZSTD_CCtx* ZSTD_createCCtx(void);
ZSTDLIB_API size_t     ZSTD_freeCCtx(ZSTD_CCtx* cctx);  

/*! ZSTD_compressCCtx() :
 *  Same as ZSTD_compress(), using an explicit ZSTD_CCtx.
 *  Important : in order to mirror `ZSTD_compress()` behavior,
 *  this function compresses at the requested compression level,
 *  __ignoring any other advanced parameter__ .
 *  If any advanced parameter was set using the advanced API,
 *  they will all be reset. Only @compressionLevel remains.
 */
ZSTDLIB_API size_t ZSTD_compressCCtx(ZSTD_CCtx* cctx,
                                     void* dst, size_t dstCapacity,
                               const void* src, size_t srcSize,
                                     int compressionLevel);

typedef struct ZSTD_DCtx_s ZSTD_DCtx;
ZSTDLIB_API ZSTD_DCtx* ZSTD_createDCtx(void);
ZSTDLIB_API size_t     ZSTD_freeDCtx(ZSTD_DCtx* dctx);  

/*! ZSTD_decompressDCtx() :
 *  Same as ZSTD_decompress(),
 *  requires an allocated ZSTD_DCtx.
 *  Compatible with sticky parameters (see below).
 */
ZSTDLIB_API size_t ZSTD_decompressDCtx(ZSTD_DCtx* dctx,
                                       void* dst, size_t dstCapacity,
                                 const void* src, size_t srcSize);





typedef enum { ZSTD_fast=1,
               ZSTD_dfast=2,
               ZSTD_greedy=3,
               ZSTD_lazy=4,
               ZSTD_lazy2=5,
               ZSTD_btlazy2=6,
               ZSTD_btopt=7,
               ZSTD_btultra=8,
               ZSTD_btultra2=9
} ZSTD_strategy;

typedef enum {

    ZSTD_c_compressionLevel=100, 
    ZSTD_c_windowLog=101,    
    ZSTD_c_hashLog=102,      
    ZSTD_c_chainLog=103,     
    ZSTD_c_searchLog=104,    
    ZSTD_c_minMatch=105,     
    ZSTD_c_targetLength=106, 
    ZSTD_c_strategy=107,     

    ZSTD_c_targetCBlockSize=130, 
    ZSTD_c_enableLongDistanceMatching=160, 
    ZSTD_c_ldmHashLog=161,   
    ZSTD_c_ldmMinMatch=162,  
    ZSTD_c_ldmBucketSizeLog=163, 
    ZSTD_c_ldmHashRateLog=164, 

    ZSTD_c_contentSizeFlag=200, 
    ZSTD_c_checksumFlag=201, 
    ZSTD_c_dictIDFlag=202,   

    ZSTD_c_nbWorkers=400,    
    ZSTD_c_jobSize=401,      
    ZSTD_c_overlapLog=402,   

     ZSTD_c_experimentalParam1=500,
     ZSTD_c_experimentalParam2=10,
     ZSTD_c_experimentalParam3=1000,
     ZSTD_c_experimentalParam4=1001,
     ZSTD_c_experimentalParam5=1002,
     ZSTD_c_experimentalParam7=1004,
     ZSTD_c_experimentalParam8=1005,
     ZSTD_c_experimentalParam9=1006,
     ZSTD_c_experimentalParam10=1007,
     ZSTD_c_experimentalParam11=1008,
     ZSTD_c_experimentalParam12=1009,
     ZSTD_c_experimentalParam13=1010,
     ZSTD_c_experimentalParam14=1011,
     ZSTD_c_experimentalParam15=1012,
     ZSTD_c_experimentalParam16=1013,
     ZSTD_c_experimentalParam17=1014,
     ZSTD_c_experimentalParam18=1015,
     ZSTD_c_experimentalParam19=1016,
     ZSTD_c_experimentalParam20=1017
} ZSTD_cParameter;

typedef struct {
    size_t error;
    int lowerBound;
    int upperBound;
} ZSTD_bounds;

/*! ZSTD_cParam_getBounds() :
 *  All parameters must belong to an interval with lower and upper bounds,
 *  otherwise they will either trigger an error or be automatically clamped.
 * @return : a structure, ZSTD_bounds, which contains
 *         - an error status field, which must be tested using ZSTD_isError()
 *         - lower and upper bounds, both inclusive
 */
ZSTDLIB_API ZSTD_bounds ZSTD_cParam_getBounds(ZSTD_cParameter cParam);

/*! ZSTD_CCtx_setParameter() :
 *  Set one compression parameter, selected by enum ZSTD_cParameter.
 *  All parameters have valid bounds. Bounds can be queried using ZSTD_cParam_getBounds().
 *  Providing a value beyond bound will either clamp it, or trigger an error (depending on parameter).
 *  Setting a parameter is generally only possible during frame initialization (before starting compression).
 *  Exception : when using multi-threading mode (nbWorkers >= 1),
 *              the following parameters can be updated _during_ compression (within same frame):
 *              => compressionLevel, hashLog, chainLog, searchLog, minMatch, targetLength and strategy.
 *              new parameters will be active for next job only (after a flush()).
 * @return : an error code (which can be tested using ZSTD_isError()).
 */
ZSTDLIB_API size_t ZSTD_CCtx_setParameter(ZSTD_CCtx* cctx, ZSTD_cParameter param, int value);

/*! ZSTD_CCtx_setPledgedSrcSize() :
 *  Total input data size to be compressed as a single frame.
 *  Value will be written in frame header, unless if explicitly forbidden using ZSTD_c_contentSizeFlag.
 *  This value will also be controlled at end of frame, and trigger an error if not respected.
 * @result : 0, or an error code (which can be tested with ZSTD_isError()).
 *  Note 1 : pledgedSrcSize==0 actually means zero, aka an empty frame.
 *           In order to mean "unknown content size", pass constant ZSTD_CONTENTSIZE_UNKNOWN.
 *           ZSTD_CONTENTSIZE_UNKNOWN is default value for any new frame.
 *  Note 2 : pledgedSrcSize is only valid once, for the next frame.
 *           It's discarded at the end of the frame, and replaced by ZSTD_CONTENTSIZE_UNKNOWN.
 *  Note 3 : Whenever all input data is provided and consumed in a single round,
 *           for example with ZSTD_compress2(),
 *           or invoking immediately ZSTD_compressStream2(,,,ZSTD_e_end),
 *           this value is automatically overridden by srcSize instead.
 */
ZSTDLIB_API size_t ZSTD_CCtx_setPledgedSrcSize(ZSTD_CCtx* cctx, unsigned long long pledgedSrcSize);

typedef enum {
    ZSTD_reset_session_only = 1,
    ZSTD_reset_parameters = 2,
    ZSTD_reset_session_and_parameters = 3
} ZSTD_ResetDirective;

/*! ZSTD_CCtx_reset() :
 *  There are 2 different things that can be reset, independently or jointly :
 *  - The session : will stop compressing current frame, and make CCtx ready to start a new one.
 *                  Useful after an error, or to interrupt any ongoing compression.
 *                  Any internal data not yet flushed is cancelled.
 *                  Compression parameters and dictionary remain unchanged.
 *                  They will be used to compress next frame.
 *                  Resetting session never fails.
 *  - The parameters : changes all parameters back to "default".
 *                  This also removes any reference to any dictionary or external sequence producer.
 *                  Parameters can only be changed between 2 sessions (i.e. no compression is currently ongoing)
 *                  otherwise the reset fails, and function returns an error value (which can be tested using ZSTD_isError())
 *  - Both : similar to resetting the session, followed by resetting parameters.
 */
ZSTDLIB_API size_t ZSTD_CCtx_reset(ZSTD_CCtx* cctx, ZSTD_ResetDirective reset);

/*! ZSTD_compress2() :
 *  Behave the same as ZSTD_compressCCtx(), but compression parameters are set using the advanced API.
 *  (note that this entry point doesn't even expose a compression level parameter).
 *  ZSTD_compress2() always starts a new frame.
 *  Should cctx hold data from a previously unfinished frame, everything about it is forgotten.
 *  - Compression parameters are pushed into CCtx before starting compression, using ZSTD_CCtx_set*()
 *  - The function is always blocking, returns when compression is completed.
 *  NOTE: Providing `dstCapacity >= ZSTD_compressBound(srcSize)` guarantees that zstd will have
 *        enough space to successfully compress the data, though it is possible it fails for other reasons.
 * @return : compressed size written into `dst` (<= `dstCapacity),
 *           or an error code if it fails (which can be tested using ZSTD_isError()).
 */
ZSTDLIB_API size_t ZSTD_compress2( ZSTD_CCtx* cctx,
                                   void* dst, size_t dstCapacity,
                             const void* src, size_t srcSize);




typedef enum {

    ZSTD_d_windowLogMax=100, 

     ZSTD_d_experimentalParam1=1000,
     ZSTD_d_experimentalParam2=1001,
     ZSTD_d_experimentalParam3=1002,
     ZSTD_d_experimentalParam4=1003,
     ZSTD_d_experimentalParam5=1004,
     ZSTD_d_experimentalParam6=1005

} ZSTD_dParameter;

/*! ZSTD_dParam_getBounds() :
 *  All parameters must belong to an interval with lower and upper bounds,
 *  otherwise they will either trigger an error or be automatically clamped.
 * @return : a structure, ZSTD_bounds, which contains
 *         - an error status field, which must be tested using ZSTD_isError()
 *         - both lower and upper bounds, inclusive
 */
ZSTDLIB_API ZSTD_bounds ZSTD_dParam_getBounds(ZSTD_dParameter dParam);

/*! ZSTD_DCtx_setParameter() :
 *  Set one compression parameter, selected by enum ZSTD_dParameter.
 *  All parameters have valid bounds. Bounds can be queried using ZSTD_dParam_getBounds().
 *  Providing a value beyond bound will either clamp it, or trigger an error (depending on parameter).
 *  Setting a parameter is only possible during frame initialization (before starting decompression).
 * @return : 0, or an error code (which can be tested using ZSTD_isError()).
 */
ZSTDLIB_API size_t ZSTD_DCtx_setParameter(ZSTD_DCtx* dctx, ZSTD_dParameter param, int value);

/*! ZSTD_DCtx_reset() :
 *  Return a DCtx to clean state.
 *  Session and parameters can be reset jointly or separately.
 *  Parameters can only be reset when no active frame is being decompressed.
 * @return : 0, or an error code, which can be tested with ZSTD_isError()
 */
ZSTDLIB_API size_t ZSTD_DCtx_reset(ZSTD_DCtx* dctx, ZSTD_ResetDirective reset);



typedef struct ZSTD_inBuffer_s {
  const void* src;    
  size_t size;        
  size_t pos;         
} ZSTD_inBuffer;

typedef struct ZSTD_outBuffer_s {
  void*  dst;         
  size_t size;        
  size_t pos;         
} ZSTD_outBuffer;




typedef ZSTD_CCtx ZSTD_CStream;  
ZSTDLIB_API ZSTD_CStream* ZSTD_createCStream(void);
ZSTDLIB_API size_t ZSTD_freeCStream(ZSTD_CStream* zcs);  

typedef enum {
    ZSTD_e_continue=0, 
    ZSTD_e_flush=1,    
    ZSTD_e_end=2       
} ZSTD_EndDirective;

/*! ZSTD_compressStream2() : Requires v1.4.0+
 *  Behaves about the same as ZSTD_compressStream, with additional control on end directive.
 *  - Compression parameters are pushed into CCtx before starting compression, using ZSTD_CCtx_set*()
 *  - Compression parameters cannot be changed once compression is started (save a list of exceptions in multi-threading mode)
 *  - output->pos must be <= dstCapacity, input->pos must be <= srcSize
 *  - output->pos and input->pos will be updated. They are guaranteed to remain below their respective limit.
 *  - endOp must be a valid directive
 *  - When nbWorkers==0 (default), function is blocking : it completes its job before returning to caller.
 *  - When nbWorkers>=1, function is non-blocking : it copies a portion of input, distributes jobs to internal worker threads, flush to output whatever is available,
 *                                                  and then immediately returns, just indicating that there is some data remaining to be flushed.
 *                                                  The function nonetheless guarantees forward progress : it will return only after it reads or write at least 1+ byte.
 *  - Exception : if the first call requests a ZSTD_e_end directive and provides enough dstCapacity, the function delegates to ZSTD_compress2() which is always blocking.
 *  - @return provides a minimum amount of data remaining to be flushed from internal buffers
 *            or an error code, which can be tested using ZSTD_isError().
 *            if @return != 0, flush is not fully completed, there is still some data left within internal buffers.
 *            This is useful for ZSTD_e_flush, since in this case more flushes are necessary to empty all buffers.
 *            For ZSTD_e_end, @return == 0 when internal buffers are fully flushed and frame is completed.
 *  - after a ZSTD_e_end directive, if internal buffer is not fully flushed (@return != 0),
 *            only ZSTD_e_end or ZSTD_e_flush operations are allowed.
 *            Before starting a new compression job, or changing compression parameters,
 *            it is required to fully flush internal buffers.
 *  - note: if an operation ends with an error, it may leave @cctx in an undefined state.
 *          Therefore, it's UB to invoke ZSTD_compressStream2() of ZSTD_compressStream() on such a state.
 *          In order to be re-employed after an error, a state must be reset,
 *          which can be done explicitly (ZSTD_CCtx_reset()),
 *          or is sometimes implied by methods starting a new compression job (ZSTD_initCStream(), ZSTD_compressCCtx())
 */
ZSTDLIB_API size_t ZSTD_compressStream2( ZSTD_CCtx* cctx,
                                         ZSTD_outBuffer* output,
                                         ZSTD_inBuffer* input,
                                         ZSTD_EndDirective endOp);


ZSTDLIB_API size_t ZSTD_CStreamInSize(void);    
ZSTDLIB_API size_t ZSTD_CStreamOutSize(void);   



/*!
 * Equivalent to:
 *
 *     ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only);
 *     ZSTD_CCtx_refCDict(zcs, NULL); // clear the dictionary (if any)
 *     ZSTD_CCtx_setParameter(zcs, ZSTD_c_compressionLevel, compressionLevel);
 *
 * Note that ZSTD_initCStream() clears any previously set dictionary. Use the new API
 * to compress with a dictionary.
 */
ZSTDLIB_API size_t ZSTD_initCStream(ZSTD_CStream* zcs, int compressionLevel);
/*!
 * Alternative for ZSTD_compressStream2(zcs, output, input, ZSTD_e_continue).
 * NOTE: The return value is different. ZSTD_compressStream() returns a hint for
 * the next read size (if non-zero and not an error). ZSTD_compressStream2()
 * returns the minimum nb of bytes left to flush (if non-zero and not an error).
 */
ZSTDLIB_API size_t ZSTD_compressStream(ZSTD_CStream* zcs, ZSTD_outBuffer* output, ZSTD_inBuffer* input);
/*! Equivalent to ZSTD_compressStream2(zcs, output, &emptyInput, ZSTD_e_flush). */
ZSTDLIB_API size_t ZSTD_flushStream(ZSTD_CStream* zcs, ZSTD_outBuffer* output);
/*! Equivalent to ZSTD_compressStream2(zcs, output, &emptyInput, ZSTD_e_end). */
ZSTDLIB_API size_t ZSTD_endStream(ZSTD_CStream* zcs, ZSTD_outBuffer* output);



typedef ZSTD_DCtx ZSTD_DStream;  
ZSTDLIB_API ZSTD_DStream* ZSTD_createDStream(void);
ZSTDLIB_API size_t ZSTD_freeDStream(ZSTD_DStream* zds);  


/*! ZSTD_initDStream() :
 * Initialize/reset DStream state for new decompression operation.
 * Call before new decompression operation using same DStream.
 *
 * Note : This function is redundant with the advanced API and equivalent to:
 *     ZSTD_DCtx_reset(zds, ZSTD_reset_session_only);
 *     ZSTD_DCtx_refDDict(zds, NULL);
 */
ZSTDLIB_API size_t ZSTD_initDStream(ZSTD_DStream* zds);

/*! ZSTD_decompressStream() :
 * Streaming decompression function.
 * Call repetitively to consume full input updating it as necessary.
 * Function will update both input and output `pos` fields exposing current state via these fields:
 * - `input.pos < input.size`, some input remaining and caller should provide remaining input
 *   on the next call.
 * - `output.pos < output.size`, decoder flushed internal output buffer.
 * - `output.pos == output.size`, unflushed data potentially present in the internal buffers,
 *   check ZSTD_decompressStream() @return value,
 *   if > 0, invoke it again to flush remaining data to output.
 * Note : with no additional input, amount of data flushed <= ZSTD_BLOCKSIZE_MAX.
 *
 * @return : 0 when a frame is completely decoded and fully flushed,
 *           or an error code, which can be tested using ZSTD_isError(),
 *           or any other value > 0, which means there is some decoding or flushing to do to complete current frame.
 *
 * Note: when an operation returns with an error code, the @zds state may be left in undefined state.
 *       It's UB to invoke `ZSTD_decompressStream()` on such a state.
 *       In order to re-use such a state, it must be first reset,
 *       which can be done explicitly (`ZSTD_DCtx_reset()`),
 *       or is implied for operations starting some new decompression job (`ZSTD_initDStream`, `ZSTD_decompressDCtx()`, `ZSTD_decompress_usingDict()`)
 */
ZSTDLIB_API size_t ZSTD_decompressStream(ZSTD_DStream* zds, ZSTD_outBuffer* output, ZSTD_inBuffer* input);

ZSTDLIB_API size_t ZSTD_DStreamInSize(void);    /*!< recommended size for input buffer */
ZSTDLIB_API size_t ZSTD_DStreamOutSize(void);   /*!< recommended size for output buffer. Guarantee to successfully flush at least one complete block in all circumstances. */


/*! ZSTD_compress_usingDict() :
 *  Compression at an explicit compression level using a Dictionary.
 *  A dictionary can be any arbitrary data segment (also called a prefix),
 *  or a buffer with specified information (see zdict.h).
 *  Note : This function loads the dictionary, resulting in significant startup delay.
 *         It's intended for a dictionary used only once.
 *  Note 2 : When `dict == NULL || dictSize < 8` no dictionary is used. */
ZSTDLIB_API size_t ZSTD_compress_usingDict(ZSTD_CCtx* ctx,
                                           void* dst, size_t dstCapacity,
                                     const void* src, size_t srcSize,
                                     const void* dict,size_t dictSize,
                                           int compressionLevel);

/*! ZSTD_decompress_usingDict() :
 *  Decompression using a known Dictionary.
 *  Dictionary must be identical to the one used during compression.
 *  Note : This function loads the dictionary, resulting in significant startup delay.
 *         It's intended for a dictionary used only once.
 *  Note : When `dict == NULL || dictSize < 8` no dictionary is used. */
ZSTDLIB_API size_t ZSTD_decompress_usingDict(ZSTD_DCtx* dctx,
                                             void* dst, size_t dstCapacity,
                                       const void* src, size_t srcSize,
                                       const void* dict,size_t dictSize);


typedef struct ZSTD_CDict_s ZSTD_CDict;

/*! ZSTD_createCDict() :
 *  When compressing multiple messages or blocks using the same dictionary,
 *  it's recommended to digest the dictionary only once, since it's a costly operation.
 *  ZSTD_createCDict() will create a state from digesting a dictionary.
 *  The resulting state can be used for future compression operations with very limited startup cost.
 *  ZSTD_CDict can be created once and shared by multiple threads concurrently, since its usage is read-only.
 * @dictBuffer can be released after ZSTD_CDict creation, because its content is copied within CDict.
 *  Note 1 : Consider experimental function `ZSTD_createCDict_byReference()` if you prefer to not duplicate @dictBuffer content.
 *  Note 2 : A ZSTD_CDict can be created from an empty @dictBuffer,
 *      in which case the only thing that it transports is the @compressionLevel.
 *      This can be useful in a pipeline featuring ZSTD_compress_usingCDict() exclusively,
 *      expecting a ZSTD_CDict parameter with any data, including those without a known dictionary. */
ZSTDLIB_API ZSTD_CDict* ZSTD_createCDict(const void* dictBuffer, size_t dictSize,
                                         int compressionLevel);

/*! ZSTD_freeCDict() :
 *  Function frees memory allocated by ZSTD_createCDict().
 *  If a NULL pointer is passed, no operation is performed. */
ZSTDLIB_API size_t      ZSTD_freeCDict(ZSTD_CDict* CDict);

/*! ZSTD_compress_usingCDict() :
 *  Compression using a digested Dictionary.
 *  Recommended when same dictionary is used multiple times.
 *  Note : compression level is _decided at dictionary creation time_,
 *     and frame parameters are hardcoded (dictID=yes, contentSize=yes, checksum=no) */
ZSTDLIB_API size_t ZSTD_compress_usingCDict(ZSTD_CCtx* cctx,
                                            void* dst, size_t dstCapacity,
                                      const void* src, size_t srcSize,
                                      const ZSTD_CDict* cdict);


typedef struct ZSTD_DDict_s ZSTD_DDict;

/*! ZSTD_createDDict() :
 *  Create a digested dictionary, ready to start decompression operation without startup delay.
 *  dictBuffer can be released after DDict creation, as its content is copied inside DDict. */
ZSTDLIB_API ZSTD_DDict* ZSTD_createDDict(const void* dictBuffer, size_t dictSize);

/*! ZSTD_freeDDict() :
 *  Function frees memory allocated with ZSTD_createDDict()
 *  If a NULL pointer is passed, no operation is performed. */
ZSTDLIB_API size_t      ZSTD_freeDDict(ZSTD_DDict* ddict);

/*! ZSTD_decompress_usingDDict() :
 *  Decompression using a digested Dictionary.
 *  Recommended when same dictionary is used multiple times. */
ZSTDLIB_API size_t ZSTD_decompress_usingDDict(ZSTD_DCtx* dctx,
                                              void* dst, size_t dstCapacity,
                                        const void* src, size_t srcSize,
                                        const ZSTD_DDict* ddict);



/*! ZSTD_getDictID_fromDict() : Requires v1.4.0+
 *  Provides the dictID stored within dictionary.
 *  if @return == 0, the dictionary is not conformant with Zstandard specification.
 *  It can still be loaded, but as a content-only dictionary. */
ZSTDLIB_API unsigned ZSTD_getDictID_fromDict(const void* dict, size_t dictSize);

/*! ZSTD_getDictID_fromCDict() : Requires v1.5.0+
 *  Provides the dictID of the dictionary loaded into `cdict`.
 *  If @return == 0, the dictionary is not conformant to Zstandard specification, or empty.
 *  Non-conformant dictionaries can still be loaded, but as content-only dictionaries. */
ZSTDLIB_API unsigned ZSTD_getDictID_fromCDict(const ZSTD_CDict* cdict);

/*! ZSTD_getDictID_fromDDict() : Requires v1.4.0+
 *  Provides the dictID of the dictionary loaded into `ddict`.
 *  If @return == 0, the dictionary is not conformant to Zstandard specification, or empty.
 *  Non-conformant dictionaries can still be loaded, but as content-only dictionaries. */
ZSTDLIB_API unsigned ZSTD_getDictID_fromDDict(const ZSTD_DDict* ddict);

/*! ZSTD_getDictID_fromFrame() : Requires v1.4.0+
 *  Provides the dictID required to decompressed the frame stored within `src`.
 *  If @return == 0, the dictID could not be decoded.
 *  This could for one of the following reasons :
 *  - The frame does not require a dictionary to be decoded (most common case).
 *  - The frame was built with dictID intentionally removed. Whatever dictionary is necessary is a hidden piece of information.
 *    Note : this use case also happens when using a non-conformant dictionary.
 *  - `srcSize` is too small, and as a result, the frame header could not be decoded (only possible if `srcSize < ZSTD_FRAMEHEADERSIZE_MAX`).
 *  - This is not a Zstandard frame.
 *  When identifying the exact failure cause, it's possible to use ZSTD_getFrameHeader(), which will provide a more precise error code. */
ZSTDLIB_API unsigned ZSTD_getDictID_fromFrame(const void* src, size_t srcSize);




/*! ZSTD_CCtx_loadDictionary() : Requires v1.4.0+
 *  Create an internal CDict from `dict` buffer.
 *  Decompression will have to use same dictionary.
 * @result : 0, or an error code (which can be tested with ZSTD_isError()).
 *  Special: Loading a NULL (or 0-size) dictionary invalidates previous dictionary,
 *           meaning "return to no-dictionary mode".
 *  Note 1 : Dictionary is sticky, it will be used for all future compressed frames,
 *           until parameters are reset, a new dictionary is loaded, or the dictionary
 *           is explicitly invalidated by loading a NULL dictionary.
 *  Note 2 : Loading a dictionary involves building tables.
 *           It's also a CPU consuming operation, with non-negligible impact on latency.
 *           Tables are dependent on compression parameters, and for this reason,
 *           compression parameters can no longer be changed after loading a dictionary.
 *  Note 3 :`dict` content will be copied internally.
 *           Use experimental ZSTD_CCtx_loadDictionary_byReference() to reference content instead.
 *           In such a case, dictionary buffer must outlive its users.
 *  Note 4 : Use ZSTD_CCtx_loadDictionary_advanced()
 *           to precisely select how dictionary content must be interpreted.
 *  Note 5 : This method does not benefit from LDM (long distance mode).
 *           If you want to employ LDM on some large dictionary content,
 *           prefer employing ZSTD_CCtx_refPrefix() described below.
 */
ZSTDLIB_API size_t ZSTD_CCtx_loadDictionary(ZSTD_CCtx* cctx, const void* dict, size_t dictSize);

/*! ZSTD_CCtx_refCDict() : Requires v1.4.0+
 *  Reference a prepared dictionary, to be used for all future compressed frames.
 *  Note that compression parameters are enforced from within CDict,
 *  and supersede any compression parameter previously set within CCtx.
 *  The parameters ignored are labelled as "superseded-by-cdict" in the ZSTD_cParameter enum docs.
 *  The ignored parameters will be used again if the CCtx is returned to no-dictionary mode.
 *  The dictionary will remain valid for future compressed frames using same CCtx.
 * @result : 0, or an error code (which can be tested with ZSTD_isError()).
 *  Special : Referencing a NULL CDict means "return to no-dictionary mode".
 *  Note 1 : Currently, only one dictionary can be managed.
 *           Referencing a new dictionary effectively "discards" any previous one.
 *  Note 2 : CDict is just referenced, its lifetime must outlive its usage within CCtx. */
ZSTDLIB_API size_t ZSTD_CCtx_refCDict(ZSTD_CCtx* cctx, const ZSTD_CDict* cdict);

/*! ZSTD_CCtx_refPrefix() : Requires v1.4.0+
 *  Reference a prefix (single-usage dictionary) for next compressed frame.
 *  A prefix is **only used once**. Tables are discarded at end of frame (ZSTD_e_end).
 *  Decompression will need same prefix to properly regenerate data.
 *  Compressing with a prefix is similar in outcome as performing a diff and compressing it,
 *  but performs much faster, especially during decompression (compression speed is tunable with compression level).
 *  This method is compatible with LDM (long distance mode).
 * @result : 0, or an error code (which can be tested with ZSTD_isError()).
 *  Special: Adding any prefix (including NULL) invalidates any previous prefix or dictionary
 *  Note 1 : Prefix buffer is referenced. It **must** outlive compression.
 *           Its content must remain unmodified during compression.
 *  Note 2 : If the intention is to diff some large src data blob with some prior version of itself,
 *           ensure that the window size is large enough to contain the entire source.
 *           See ZSTD_c_windowLog.
 *  Note 3 : Referencing a prefix involves building tables, which are dependent on compression parameters.
 *           It's a CPU consuming operation, with non-negligible impact on latency.
 *           If there is a need to use the same prefix multiple times, consider loadDictionary instead.
 *  Note 4 : By default, the prefix is interpreted as raw content (ZSTD_dct_rawContent).
 *           Use experimental ZSTD_CCtx_refPrefix_advanced() to alter dictionary interpretation. */
ZSTDLIB_API size_t ZSTD_CCtx_refPrefix(ZSTD_CCtx* cctx,
                                 const void* prefix, size_t prefixSize);

/*! ZSTD_DCtx_loadDictionary() : Requires v1.4.0+
 *  Create an internal DDict from dict buffer, to be used to decompress all future frames.
 *  The dictionary remains valid for all future frames, until explicitly invalidated, or
 *  a new dictionary is loaded.
 * @result : 0, or an error code (which can be tested with ZSTD_isError()).
 *  Special : Adding a NULL (or 0-size) dictionary invalidates any previous dictionary,
 *            meaning "return to no-dictionary mode".
 *  Note 1 : Loading a dictionary involves building tables,
 *           which has a non-negligible impact on CPU usage and latency.
 *           It's recommended to "load once, use many times", to amortize the cost
 *  Note 2 :`dict` content will be copied internally, so `dict` can be released after loading.
 *           Use ZSTD_DCtx_loadDictionary_byReference() to reference dictionary content instead.
 *  Note 3 : Use ZSTD_DCtx_loadDictionary_advanced() to take control of
 *           how dictionary content is loaded and interpreted.
 */
ZSTDLIB_API size_t ZSTD_DCtx_loadDictionary(ZSTD_DCtx* dctx, const void* dict, size_t dictSize);

/*! ZSTD_DCtx_refDDict() : Requires v1.4.0+
 *  Reference a prepared dictionary, to be used to decompress next frames.
 *  The dictionary remains active for decompression of future frames using same DCtx.
 *
 *  If called with ZSTD_d_refMultipleDDicts enabled, repeated calls of this function
 *  will store the DDict references in a table, and the DDict used for decompression
 *  will be determined at decompression time, as per the dict ID in the frame.
 *  The memory for the table is allocated on the first call to refDDict, and can be
 *  freed with ZSTD_freeDCtx().
 *
 *  If called with ZSTD_d_refMultipleDDicts disabled (the default), only one dictionary
 *  will be managed, and referencing a dictionary effectively "discards" any previous one.
 *
 * @result : 0, or an error code (which can be tested with ZSTD_isError()).
 *  Special: referencing a NULL DDict means "return to no-dictionary mode".
 *  Note 2 : DDict is just referenced, its lifetime must outlive its usage from DCtx.
 */
ZSTDLIB_API size_t ZSTD_DCtx_refDDict(ZSTD_DCtx* dctx, const ZSTD_DDict* ddict);

/*! ZSTD_DCtx_refPrefix() : Requires v1.4.0+
 *  Reference a prefix (single-usage dictionary) to decompress next frame.
 *  This is the reverse operation of ZSTD_CCtx_refPrefix(),
 *  and must use the same prefix as the one used during compression.
 *  Prefix is **only used once**. Reference is discarded at end of frame.
 *  End of frame is reached when ZSTD_decompressStream() returns 0.
 * @result : 0, or an error code (which can be tested with ZSTD_isError()).
 *  Note 1 : Adding any prefix (including NULL) invalidates any previously set prefix or dictionary
 *  Note 2 : Prefix buffer is referenced. It **must** outlive decompression.
 *           Prefix buffer must remain unmodified up to the end of frame,
 *           reached when ZSTD_decompressStream() returns 0.
 *  Note 3 : By default, the prefix is treated as raw content (ZSTD_dct_rawContent).
 *           Use ZSTD_CCtx_refPrefix_advanced() to alter dictMode (Experimental section)
 *  Note 4 : Referencing a raw content prefix has almost no cpu nor memory cost.
 *           A full dictionary is more costly, as it requires building tables.
 */
ZSTDLIB_API size_t ZSTD_DCtx_refPrefix(ZSTD_DCtx* dctx,
                                 const void* prefix, size_t prefixSize);


/*! ZSTD_sizeof_*() : Requires v1.4.0+
 *  These functions give the _current_ memory usage of selected object.
 *  Note that object memory usage can evolve (increase or decrease) over time. */
ZSTDLIB_API size_t ZSTD_sizeof_CCtx(const ZSTD_CCtx* cctx);
ZSTDLIB_API size_t ZSTD_sizeof_DCtx(const ZSTD_DCtx* dctx);
ZSTDLIB_API size_t ZSTD_sizeof_CStream(const ZSTD_CStream* zcs);
ZSTDLIB_API size_t ZSTD_sizeof_DStream(const ZSTD_DStream* zds);
ZSTDLIB_API size_t ZSTD_sizeof_CDict(const ZSTD_CDict* cdict);
ZSTDLIB_API size_t ZSTD_sizeof_DDict(const ZSTD_DDict* ddict);

#if defined (__cplusplus)
}
#endif

#endif



#if defined(ZSTD_STATIC_LINKING_ONLY) && !defined(ZSTD_H_ZSTD_STATIC_LINKING_ONLY)
#define ZSTD_H_ZSTD_STATIC_LINKING_ONLY

#if defined (__cplusplus)
extern "C" {
#endif

#if !defined(ZSTDLIB_STATIC_API)
#if defined(ZSTD_DLL_EXPORT) && (ZSTD_DLL_EXPORT==1)
#    define ZSTDLIB_STATIC_API __declspec(dllexport) ZSTDLIB_VISIBLE
#elif defined(ZSTD_DLL_IMPORT) && (ZSTD_DLL_IMPORT==1)
#    define ZSTDLIB_STATIC_API __declspec(dllimport) ZSTDLIB_VISIBLE
#else
#    define ZSTDLIB_STATIC_API ZSTDLIB_VISIBLE
#endif
#endif


#define ZSTD_FRAMEHEADERSIZE_PREFIX(format) ((format) == ZSTD_f_zstd1 ? 5 : 1)   /* minimum input size required to query frame header size */
#define ZSTD_FRAMEHEADERSIZE_MIN(format)    ((format) == ZSTD_f_zstd1 ? 6 : 2)
#define ZSTD_FRAMEHEADERSIZE_MAX   18   /* can be useful for static allocation */
#define ZSTD_SKIPPABLEHEADERSIZE    8

#define ZSTD_WINDOWLOG_MAX_32    30
#define ZSTD_WINDOWLOG_MAX_64    31
#define ZSTD_WINDOWLOG_MAX     ((int)(sizeof(size_t) == 4 ? ZSTD_WINDOWLOG_MAX_32 : ZSTD_WINDOWLOG_MAX_64))
#define ZSTD_WINDOWLOG_MIN       10
#define ZSTD_HASHLOG_MAX       ((ZSTD_WINDOWLOG_MAX < 30) ? ZSTD_WINDOWLOG_MAX : 30)
#define ZSTD_HASHLOG_MIN          6
#define ZSTD_CHAINLOG_MAX_32     29
#define ZSTD_CHAINLOG_MAX_64     30
#define ZSTD_CHAINLOG_MAX      ((int)(sizeof(size_t) == 4 ? ZSTD_CHAINLOG_MAX_32 : ZSTD_CHAINLOG_MAX_64))
#define ZSTD_CHAINLOG_MIN        ZSTD_HASHLOG_MIN
#define ZSTD_SEARCHLOG_MAX      (ZSTD_WINDOWLOG_MAX-1)
#define ZSTD_SEARCHLOG_MIN        1
#define ZSTD_MINMATCH_MAX         7   /* only for ZSTD_fast, other strategies are limited to 6 */
#define ZSTD_MINMATCH_MIN         3   /* only for ZSTD_btopt+, faster strategies are limited to 4 */
#define ZSTD_TARGETLENGTH_MAX    ZSTD_BLOCKSIZE_MAX
#define ZSTD_TARGETLENGTH_MIN     0   /* note : comparing this constant to an unsigned results in a tautological test */
#define ZSTD_STRATEGY_MIN        ZSTD_fast
#define ZSTD_STRATEGY_MAX        ZSTD_btultra2
#define ZSTD_BLOCKSIZE_MAX_MIN (1 << 10) /* The minimum valid max blocksize. Maximum blocksizes smaller than this make compressBound() inaccurate. */


#define ZSTD_OVERLAPLOG_MIN       0
#define ZSTD_OVERLAPLOG_MAX       9

#define ZSTD_WINDOWLOG_LIMIT_DEFAULT 27   /* by default, the streaming decoder will refuse any frame
                                           * requiring larger than (1<<ZSTD_WINDOWLOG_LIMIT_DEFAULT) window size,
                                           * to preserve host's memory from unreasonable requirements.
                                           * This limit can be overridden using ZSTD_DCtx_setParameter(,ZSTD_d_windowLogMax,).
                                           * The limit does not apply for one-pass decoders (such as ZSTD_decompress()), since no additional memory is allocated */


#define ZSTD_LDM_HASHLOG_MIN      ZSTD_HASHLOG_MIN
#define ZSTD_LDM_HASHLOG_MAX      ZSTD_HASHLOG_MAX
#define ZSTD_LDM_MINMATCH_MIN        4
#define ZSTD_LDM_MINMATCH_MAX     4096
#define ZSTD_LDM_BUCKETSIZELOG_MIN   1
#define ZSTD_LDM_BUCKETSIZELOG_MAX   8
#define ZSTD_LDM_HASHRATELOG_MIN     0
#define ZSTD_LDM_HASHRATELOG_MAX (ZSTD_WINDOWLOG_MAX - ZSTD_HASHLOG_MIN)

#define ZSTD_TARGETCBLOCKSIZE_MIN   1340 /* suitable to fit into an ethernet / wifi / 4G transport frame */
#define ZSTD_TARGETCBLOCKSIZE_MAX   ZSTD_BLOCKSIZE_MAX
#define ZSTD_SRCSIZEHINT_MIN        0
#define ZSTD_SRCSIZEHINT_MAX        INT_MAX



typedef struct ZSTD_CCtx_params_s ZSTD_CCtx_params;

typedef struct {
    unsigned int offset;      

    unsigned int litLength;   
    unsigned int matchLength; 


    unsigned int rep;         
} ZSTD_Sequence;

typedef struct {
    unsigned windowLog;       
    unsigned chainLog;        
    unsigned hashLog;         
    unsigned searchLog;       
    unsigned minMatch;        
    unsigned targetLength;    
    ZSTD_strategy strategy;   
} ZSTD_compressionParameters;

typedef struct {
    int contentSizeFlag; 
    int checksumFlag;    
    int noDictIDFlag;    
} ZSTD_frameParameters;

typedef struct {
    ZSTD_compressionParameters cParams;
    ZSTD_frameParameters fParams;
} ZSTD_parameters;

typedef enum {
    ZSTD_dct_auto = 0,       
    ZSTD_dct_rawContent = 1, 
    ZSTD_dct_fullDict = 2    
} ZSTD_dictContentType_e;

typedef enum {
    ZSTD_dlm_byCopy = 0,  
    ZSTD_dlm_byRef = 1    
} ZSTD_dictLoadMethod_e;

typedef enum {
    ZSTD_f_zstd1 = 0,           
    ZSTD_f_zstd1_magicless = 1  
} ZSTD_format_e;

typedef enum {
    ZSTD_d_validateChecksum = 0,
    ZSTD_d_ignoreChecksum = 1
} ZSTD_forceIgnoreChecksum_e;

typedef enum {
    ZSTD_rmd_refSingleDDict = 0,
    ZSTD_rmd_refMultipleDDicts = 1
} ZSTD_refMultipleDDicts_e;

typedef enum {
    ZSTD_dictDefaultAttach = 0, 
    ZSTD_dictForceAttach   = 1, 
    ZSTD_dictForceCopy     = 2, 
    ZSTD_dictForceLoad     = 3  
} ZSTD_dictAttachPref_e;

typedef enum {
  ZSTD_lcm_auto = 0,          
  ZSTD_lcm_huffman = 1,       
  ZSTD_lcm_uncompressed = 2   
} ZSTD_literalCompressionMode_e;

typedef enum {
  ZSTD_ps_auto = 0,         
  ZSTD_ps_enable = 1,       
  ZSTD_ps_disable = 2       
} ZSTD_ParamSwitch_e;
#define ZSTD_paramSwitch_e ZSTD_ParamSwitch_e  /* old name */


/*! ZSTD_findDecompressedSize() :
 *  `src` should point to the start of a series of ZSTD encoded and/or skippable frames
 *  `srcSize` must be the _exact_ size of this series
 *       (i.e. there should be a frame boundary at `src + srcSize`)
 *  @return : - decompressed size of all data in all successive frames
 *            - if the decompressed size cannot be determined: ZSTD_CONTENTSIZE_UNKNOWN
 *            - if an error occurred: ZSTD_CONTENTSIZE_ERROR
 *
 *   note 1 : decompressed size is an optional field, that may not be present, especially in streaming mode.
 *            When `return==ZSTD_CONTENTSIZE_UNKNOWN`, data to decompress could be any size.
 *            In which case, it's necessary to use streaming mode to decompress data.
 *   note 2 : decompressed size is always present when compression is done with ZSTD_compress()
 *   note 3 : decompressed size can be very large (64-bits value),
 *            potentially larger than what local system can handle as a single memory segment.
 *            In which case, it's necessary to use streaming mode to decompress data.
 *   note 4 : If source is untrusted, decompressed size could be wrong or intentionally modified.
 *            Always ensure result fits within application's authorized limits.
 *            Each application can set its own limits.
 *   note 5 : ZSTD_findDecompressedSize handles multiple frames, and so it must traverse the input to
 *            read each contained frame header.  This is fast as most of the data is skipped,
 *            however it does mean that all frame data must be present and valid. */
ZSTDLIB_STATIC_API unsigned long long ZSTD_findDecompressedSize(const void* src, size_t srcSize);

/*! ZSTD_decompressBound() :
 *  `src` should point to the start of a series of ZSTD encoded and/or skippable frames
 *  `srcSize` must be the _exact_ size of this series
 *       (i.e. there should be a frame boundary at `src + srcSize`)
 *  @return : - upper-bound for the decompressed size of all data in all successive frames
 *            - if an error occurred: ZSTD_CONTENTSIZE_ERROR
 *
 *  note 1  : an error can occur if `src` contains an invalid or incorrectly formatted frame.
 *  note 2  : the upper-bound is exact when the decompressed size field is available in every ZSTD encoded frame of `src`.
 *            in this case, `ZSTD_findDecompressedSize` and `ZSTD_decompressBound` return the same value.
 *  note 3  : when the decompressed size field isn't available, the upper-bound for that frame is calculated by:
 *              upper-bound = # blocks * min(128 KB, Window_Size)
 */
ZSTDLIB_STATIC_API unsigned long long ZSTD_decompressBound(const void* src, size_t srcSize);

/*! ZSTD_frameHeaderSize() :
 *  srcSize must be large enough, aka >= ZSTD_FRAMEHEADERSIZE_PREFIX.
 * @return : size of the Frame Header,
 *           or an error code (if srcSize is too small) */
ZSTDLIB_STATIC_API size_t ZSTD_frameHeaderSize(const void* src, size_t srcSize);

typedef enum { ZSTD_frame, ZSTD_skippableFrame } ZSTD_FrameType_e;
#define ZSTD_frameType_e ZSTD_FrameType_e /* old name */
typedef struct {
    unsigned long long frameContentSize; 
    unsigned long long windowSize;       
    unsigned blockSizeMax;
    ZSTD_FrameType_e frameType;          
    unsigned headerSize;
    unsigned dictID;                     
    unsigned checksumFlag;
    unsigned _reserved1;
    unsigned _reserved2;
} ZSTD_FrameHeader;
#define ZSTD_frameHeader ZSTD_FrameHeader /* old name */

/*! ZSTD_getFrameHeader() :
 *  decode Frame Header into `zfhPtr`, or requires larger `srcSize`.
 * @return : 0 => header is complete, `zfhPtr` is correctly filled,
 *          >0 => `srcSize` is too small, @return value is the wanted `srcSize` amount, `zfhPtr` is not filled,
 *           or an error code, which can be tested using ZSTD_isError() */
ZSTDLIB_STATIC_API size_t ZSTD_getFrameHeader(ZSTD_FrameHeader* zfhPtr, const void* src, size_t srcSize);
/*! ZSTD_getFrameHeader_advanced() :
 *  same as ZSTD_getFrameHeader(),
 *  with added capability to select a format (like ZSTD_f_zstd1_magicless) */
ZSTDLIB_STATIC_API size_t ZSTD_getFrameHeader_advanced(ZSTD_FrameHeader* zfhPtr, const void* src, size_t srcSize, ZSTD_format_e format);

/*! ZSTD_decompressionMargin() :
 * Zstd supports in-place decompression, where the input and output buffers overlap.
 * In this case, the output buffer must be at least (Margin + Output_Size) bytes large,
 * and the input buffer must be at the end of the output buffer.
 *
 *  _______________________ Output Buffer ________________________
 * |                                                              |
 * |                                        ____ Input Buffer ____|
 * |                                       |                      |
 * v                                       v                      v
 * |---------------------------------------|-----------|----------|
 * ^                                                   ^          ^
 * |___________________ Output_Size ___________________|_ Margin _|
 *
 * NOTE: See also ZSTD_DECOMPRESSION_MARGIN().
 * NOTE: This applies only to single-pass decompression through ZSTD_decompress() or
 * ZSTD_decompressDCtx().
 * NOTE: This function supports multi-frame input.
 *
 * @param src The compressed frame(s)
 * @param srcSize The size of the compressed frame(s)
 * @returns The decompression margin or an error that can be checked with ZSTD_isError().
 */
ZSTDLIB_STATIC_API size_t ZSTD_decompressionMargin(const void* src, size_t srcSize);

/*! ZSTD_DECOMPRESS_MARGIN() :
 * Similar to ZSTD_decompressionMargin(), but instead of computing the margin from
 * the compressed frame, compute it from the original size and the blockSizeLog.
 * See ZSTD_decompressionMargin() for details.
 *
 * WARNING: This macro does not support multi-frame input, the input must be a single
 * zstd frame. If you need that support use the function, or implement it yourself.
 *
 * @param originalSize The original uncompressed size of the data.
 * @param blockSize    The block size == MIN(windowSize, ZSTD_BLOCKSIZE_MAX).
 *                     Unless you explicitly set the windowLog smaller than
 *                     ZSTD_BLOCKSIZELOG_MAX you can just use ZSTD_BLOCKSIZE_MAX.
 */
#define ZSTD_DECOMPRESSION_MARGIN(originalSize, blockSize) ((size_t)(                                              \
        ZSTD_FRAMEHEADERSIZE_MAX                                                               + \
        4                                                                                          + \
        ((originalSize) == 0 ? 0 : 3 * (((originalSize) + (blockSize) - 1) / blockSize))  + \
        (blockSize)                                                                       \
    ))

typedef enum {
  ZSTD_sf_noBlockDelimiters = 0,         
  ZSTD_sf_explicitBlockDelimiters = 1    
} ZSTD_SequenceFormat_e;
#define ZSTD_sequenceFormat_e ZSTD_SequenceFormat_e /* old name */

/*! ZSTD_sequenceBound() :
 * `srcSize` : size of the input buffer
 *  @return : upper-bound for the number of sequences that can be generated
 *            from a buffer of srcSize bytes
 *
 *  note : returns number of sequences - to get bytes, multiply by sizeof(ZSTD_Sequence).
 */
ZSTDLIB_STATIC_API size_t ZSTD_sequenceBound(size_t srcSize);

/*! ZSTD_generateSequences() :
 * WARNING: This function is meant for debugging and informational purposes ONLY!
 * Its implementation is flawed, and it will be deleted in a future version.
 * It is not guaranteed to succeed, as there are several cases where it will give
 * up and fail. You should NOT use this function in production code.
 *
 * This function is deprecated, and will be removed in a future version.
 *
 * Generate sequences using ZSTD_compress2(), given a source buffer.
 *
 * @param zc The compression context to be used for ZSTD_compress2(). Set any
 *           compression parameters you need on this context.
 * @param outSeqs The output sequences buffer of size @p outSeqsSize
 * @param outSeqsCapacity The size of the output sequences buffer.
 *                    ZSTD_sequenceBound(srcSize) is an upper bound on the number
 *                    of sequences that can be generated.
 * @param src The source buffer to generate sequences from of size @p srcSize.
 * @param srcSize The size of the source buffer.
 *
 * Each block will end with a dummy sequence
 * with offset == 0, matchLength == 0, and litLength == length of last literals.
 * litLength may be == 0, and if so, then the sequence of (of: 0 ml: 0 ll: 0)
 * simply acts as a block delimiter.
 *
 * @returns The number of sequences generated, necessarily less than
 *          ZSTD_sequenceBound(srcSize), or an error code that can be checked
 *          with ZSTD_isError().
 */
ZSTD_DEPRECATED("For debugging only, will be replaced by ZSTD_extractSequences()")
ZSTDLIB_STATIC_API size_t
ZSTD_generateSequences(ZSTD_CCtx* zc,
                       ZSTD_Sequence* outSeqs, size_t outSeqsCapacity,
                       const void* src, size_t srcSize);

/*! ZSTD_mergeBlockDelimiters() :
 * Given an array of ZSTD_Sequence, remove all sequences that represent block delimiters/last literals
 * by merging them into the literals of the next sequence.
 *
 * As such, the final generated result has no explicit representation of block boundaries,
 * and the final last literals segment is not represented in the sequences.
 *
 * The output of this function can be fed into ZSTD_compressSequences() with CCtx
 * setting of ZSTD_c_blockDelimiters as ZSTD_sf_noBlockDelimiters
 * @return : number of sequences left after merging
 */
ZSTDLIB_STATIC_API size_t ZSTD_mergeBlockDelimiters(ZSTD_Sequence* sequences, size_t seqsSize);

/*! ZSTD_compressSequences() :
 * Compress an array of ZSTD_Sequence, associated with @src buffer, into dst.
 * @src contains the entire input (not just the literals).
 * If @srcSize > sum(sequence.length), the remaining bytes are considered all literals
 * If a dictionary is included, then the cctx should reference the dict (see: ZSTD_CCtx_refCDict(), ZSTD_CCtx_loadDictionary(), etc.).
 * The entire source is compressed into a single frame.
 *
 * The compression behavior changes based on cctx params. In particular:
 *    If ZSTD_c_blockDelimiters == ZSTD_sf_noBlockDelimiters, the array of ZSTD_Sequence is expected to contain
 *    no block delimiters (defined in ZSTD_Sequence). Block boundaries are roughly determined based on
 *    the block size derived from the cctx, and sequences may be split. This is the default setting.
 *
 *    If ZSTD_c_blockDelimiters == ZSTD_sf_explicitBlockDelimiters, the array of ZSTD_Sequence is expected to contain
 *    valid block delimiters (defined in ZSTD_Sequence). Behavior is undefined if no block delimiters are provided.
 *
 *    When ZSTD_c_blockDelimiters == ZSTD_sf_explicitBlockDelimiters, it's possible to decide generating repcodes
 *    using the advanced parameter ZSTD_c_repcodeResolution. Repcodes will improve compression ratio, though the benefit
 *    can vary greatly depending on Sequences. On the other hand, repcode resolution is an expensive operation.
 *    By default, it's disabled at low (<10) compression levels, and enabled above the threshold (>=10).
 *    ZSTD_c_repcodeResolution makes it possible to directly manage this processing in either direction.
 *
 *    If ZSTD_c_validateSequences == 0, this function blindly accepts the Sequences provided. Invalid Sequences cause undefined
 *    behavior. If ZSTD_c_validateSequences == 1, then the function will detect invalid Sequences (see doc/zstd_compression_format.md for
 *    specifics regarding offset/matchlength requirements) and then bail out and return an error.
 *
 *    In addition to the two adjustable experimental params, there are other important cctx params.
 *    - ZSTD_c_minMatch MUST be set as less than or equal to the smallest match generated by the match finder. It has a minimum value of ZSTD_MINMATCH_MIN.
 *    - ZSTD_c_compressionLevel accordingly adjusts the strength of the entropy coder, as it would in typical compression.
 *    - ZSTD_c_windowLog affects offset validation: this function will return an error at higher debug levels if a provided offset
 *      is larger than what the spec allows for a given window log and dictionary (if present). See: doc/zstd_compression_format.md
 *
 * Note: Repcodes are, as of now, always re-calculated within this function, ZSTD_Sequence.rep is effectively unused.
 * Dev Note: Once ability to ingest repcodes become available, the explicit block delims mode must respect those repcodes exactly,
 *         and cannot emit an RLE block that disagrees with the repcode history.
 * @return : final compressed size, or a ZSTD error code.
 */
ZSTDLIB_STATIC_API size_t
ZSTD_compressSequences(ZSTD_CCtx* cctx,
                       void* dst, size_t dstCapacity,
                 const ZSTD_Sequence* inSeqs, size_t inSeqsSize,
                 const void* src, size_t srcSize);


/*! ZSTD_compressSequencesAndLiterals() :
 * This is a variant of ZSTD_compressSequences() which,
 * instead of receiving (src,srcSize) as input parameter, receives (literals,litSize),
 * aka all the literals, already extracted and laid out into a single continuous buffer.
 * This can be useful if the process generating the sequences also happens to generate the buffer of literals,
 * thus skipping an extraction + caching stage.
 * It's a speed optimization, useful when the right conditions are met,
 * but it also features the following limitations:
 * - Only supports explicit delimiter mode
 * - Currently does not support Sequences validation (so input Sequences are trusted)
 * - Not compatible with frame checksum, which must be disabled
 * - If any block is incompressible, will fail and return an error
 * - @litSize must be == sum of all @.litLength fields in @inSeqs. Any discrepancy will generate an error.
 * - @litBufCapacity is the size of the underlying buffer into which literals are written, starting at address @literals.
 *   @litBufCapacity must be at least 8 bytes larger than @litSize.
 * - @decompressedSize must be correct, and correspond to the sum of all Sequences. Any discrepancy will generate an error.
 * @return : final compressed size, or a ZSTD error code.
 */
ZSTDLIB_STATIC_API size_t
ZSTD_compressSequencesAndLiterals(ZSTD_CCtx* cctx,
                                  void* dst, size_t dstCapacity,
                            const ZSTD_Sequence* inSeqs, size_t nbSequences,
                            const void* literals, size_t litSize, size_t litBufCapacity,
                            size_t decompressedSize);


/*! ZSTD_writeSkippableFrame() :
 * Generates a zstd skippable frame containing data given by src, and writes it to dst buffer.
 *
 * Skippable frames begin with a 4-byte magic number. There are 16 possible choices of magic number,
 * ranging from ZSTD_MAGIC_SKIPPABLE_START to ZSTD_MAGIC_SKIPPABLE_START+15.
 * As such, the parameter magicVariant controls the exact skippable frame magic number variant used,
 * so the magic number used will be ZSTD_MAGIC_SKIPPABLE_START + magicVariant.
 *
 * Returns an error if destination buffer is not large enough, if the source size is not representable
 * with a 4-byte unsigned int, or if the parameter magicVariant is greater than 15 (and therefore invalid).
 *
 * @return : number of bytes written or a ZSTD error.
 */
ZSTDLIB_STATIC_API size_t ZSTD_writeSkippableFrame(void* dst, size_t dstCapacity,
                                             const void* src, size_t srcSize,
                                                   unsigned magicVariant);

/*! ZSTD_readSkippableFrame() :
 * Retrieves the content of a zstd skippable frame starting at @src, and writes it to @dst buffer.
 *
 * The parameter @magicVariant will receive the magicVariant that was supplied when the frame was written,
 * i.e. magicNumber - ZSTD_MAGIC_SKIPPABLE_START.
 * This can be NULL if the caller is not interested in the magicVariant.
 *
 * Returns an error if destination buffer is not large enough, or if the frame is not skippable.
 *
 * @return : number of bytes written or a ZSTD error.
 */
ZSTDLIB_STATIC_API size_t ZSTD_readSkippableFrame(void* dst, size_t dstCapacity,
                                                  unsigned* magicVariant,
                                                  const void* src, size_t srcSize);

/*! ZSTD_isSkippableFrame() :
 *  Tells if the content of `buffer` starts with a valid Frame Identifier for a skippable frame.
 */
ZSTDLIB_STATIC_API unsigned ZSTD_isSkippableFrame(const void* buffer, size_t size);




/*! ZSTD_estimate*() :
 *  These functions make it possible to estimate memory usage
 *  of a future {D,C}Ctx, before its creation.
 *  This is useful in combination with ZSTD_initStatic(),
 *  which makes it possible to employ a static buffer for ZSTD_CCtx* state.
 *
 *  ZSTD_estimateCCtxSize() will provide a memory budget large enough
 *  to compress data of any size using one-shot compression ZSTD_compressCCtx() or ZSTD_compress2()
 *  associated with any compression level up to max specified one.
 *  The estimate will assume the input may be arbitrarily large,
 *  which is the worst case.
 *
 *  Note that the size estimation is specific for one-shot compression,
 *  it is not valid for streaming (see ZSTD_estimateCStreamSize*())
 *  nor other potential ways of using a ZSTD_CCtx* state.
 *
 *  When srcSize can be bound by a known and rather "small" value,
 *  this knowledge can be used to provide a tighter budget estimation
 *  because the ZSTD_CCtx* state will need less memory for small inputs.
 *  This tighter estimation can be provided by employing more advanced functions
 *  ZSTD_estimateCCtxSize_usingCParams(), which can be used in tandem with ZSTD_getCParams(),
 *  and ZSTD_estimateCCtxSize_usingCCtxParams(), which can be used in tandem with ZSTD_CCtxParams_setParameter().
 *  Both can be used to estimate memory using custom compression parameters and arbitrary srcSize limits.
 *
 *  Note : only single-threaded compression is supported.
 *  ZSTD_estimateCCtxSize_usingCCtxParams() will return an error code if ZSTD_c_nbWorkers is >= 1.
 */
ZSTDLIB_STATIC_API size_t ZSTD_estimateCCtxSize(int maxCompressionLevel);
ZSTDLIB_STATIC_API size_t ZSTD_estimateCCtxSize_usingCParams(ZSTD_compressionParameters cParams);
ZSTDLIB_STATIC_API size_t ZSTD_estimateCCtxSize_usingCCtxParams(const ZSTD_CCtx_params* params);
ZSTDLIB_STATIC_API size_t ZSTD_estimateDCtxSize(void);

/*! ZSTD_estimateCStreamSize() :
 *  ZSTD_estimateCStreamSize() will provide a memory budget large enough for streaming compression
 *  using any compression level up to the max specified one.
 *  It will also consider src size to be arbitrarily "large", which is a worst case scenario.
 *  If srcSize is known to always be small, ZSTD_estimateCStreamSize_usingCParams() can provide a tighter estimation.
 *  ZSTD_estimateCStreamSize_usingCParams() can be used in tandem with ZSTD_getCParams() to create cParams from compressionLevel.
 *  ZSTD_estimateCStreamSize_usingCCtxParams() can be used in tandem with ZSTD_CCtxParams_setParameter(). Only single-threaded compression is supported. This function will return an error code if ZSTD_c_nbWorkers is >= 1.
 *  Note : CStream size estimation is only correct for single-threaded compression.
 *  ZSTD_estimateCStreamSize_usingCCtxParams() will return an error code if ZSTD_c_nbWorkers is >= 1.
 *  Note 2 : ZSTD_estimateCStreamSize* functions are not compatible with the Block-Level Sequence Producer API at this time.
 *  Size estimates assume that no external sequence producer is registered.
 *
 *  ZSTD_DStream memory budget depends on frame's window Size.
 *  This information can be passed manually, using ZSTD_estimateDStreamSize,
 *  or deducted from a valid frame Header, using ZSTD_estimateDStreamSize_fromFrame();
 *  Any frame requesting a window size larger than max specified one will be rejected.
 *  Note : if streaming is init with function ZSTD_init?Stream_usingDict(),
 *         an internal ?Dict will be created, which additional size is not estimated here.
 *         In this case, get total size by adding ZSTD_estimate?DictSize
 */
ZSTDLIB_STATIC_API size_t ZSTD_estimateCStreamSize(int maxCompressionLevel);
ZSTDLIB_STATIC_API size_t ZSTD_estimateCStreamSize_usingCParams(ZSTD_compressionParameters cParams);
ZSTDLIB_STATIC_API size_t ZSTD_estimateCStreamSize_usingCCtxParams(const ZSTD_CCtx_params* params);
ZSTDLIB_STATIC_API size_t ZSTD_estimateDStreamSize(size_t maxWindowSize);
ZSTDLIB_STATIC_API size_t ZSTD_estimateDStreamSize_fromFrame(const void* src, size_t srcSize);

/*! ZSTD_estimate?DictSize() :
 *  ZSTD_estimateCDictSize() will bet that src size is relatively "small", and content is copied, like ZSTD_createCDict().
 *  ZSTD_estimateCDictSize_advanced() makes it possible to control compression parameters precisely, like ZSTD_createCDict_advanced().
 *  Note : dictionaries created by reference (`ZSTD_dlm_byRef`) are logically smaller.
 */
ZSTDLIB_STATIC_API size_t ZSTD_estimateCDictSize(size_t dictSize, int compressionLevel);
ZSTDLIB_STATIC_API size_t ZSTD_estimateCDictSize_advanced(size_t dictSize, ZSTD_compressionParameters cParams, ZSTD_dictLoadMethod_e dictLoadMethod);
ZSTDLIB_STATIC_API size_t ZSTD_estimateDDictSize(size_t dictSize, ZSTD_dictLoadMethod_e dictLoadMethod);

/*! ZSTD_initStatic*() :
 *  Initialize an object using a pre-allocated fixed-size buffer.
 *  workspace: The memory area to emplace the object into.
 *             Provided pointer *must be 8-bytes aligned*.
 *             Buffer must outlive object.
 *  workspaceSize: Use ZSTD_estimate*Size() to determine
 *                 how large workspace must be to support target scenario.
 * @return : pointer to object (same address as workspace, just different type),
 *           or NULL if error (size too small, incorrect alignment, etc.)
 *  Note : zstd will never resize nor malloc() when using a static buffer.
 *         If the object requires more memory than available,
 *         zstd will just error out (typically ZSTD_error_memory_allocation).
 *  Note 2 : there is no corresponding "free" function.
 *           Since workspace is allocated externally, it must be freed externally too.
 *  Note 3 : cParams : use ZSTD_getCParams() to convert a compression level
 *           into its associated cParams.
 *  Limitation 1 : currently not compatible with internal dictionary creation, triggered by
 *                 ZSTD_CCtx_loadDictionary(), ZSTD_initCStream_usingDict() or ZSTD_initDStream_usingDict().
 *  Limitation 2 : static cctx currently not compatible with multi-threading.
 *  Limitation 3 : static dctx is incompatible with legacy support.
 */
ZSTDLIB_STATIC_API ZSTD_CCtx*    ZSTD_initStaticCCtx(void* workspace, size_t workspaceSize);
ZSTDLIB_STATIC_API ZSTD_CStream* ZSTD_initStaticCStream(void* workspace, size_t workspaceSize);    

ZSTDLIB_STATIC_API ZSTD_DCtx*    ZSTD_initStaticDCtx(void* workspace, size_t workspaceSize);
ZSTDLIB_STATIC_API ZSTD_DStream* ZSTD_initStaticDStream(void* workspace, size_t workspaceSize);    

ZSTDLIB_STATIC_API const ZSTD_CDict* ZSTD_initStaticCDict(
                                        void* workspace, size_t workspaceSize,
                                        const void* dict, size_t dictSize,
                                        ZSTD_dictLoadMethod_e dictLoadMethod,
                                        ZSTD_dictContentType_e dictContentType,
                                        ZSTD_compressionParameters cParams);

ZSTDLIB_STATIC_API const ZSTD_DDict* ZSTD_initStaticDDict(
                                        void* workspace, size_t workspaceSize,
                                        const void* dict, size_t dictSize,
                                        ZSTD_dictLoadMethod_e dictLoadMethod,
                                        ZSTD_dictContentType_e dictContentType);


/*! Custom memory allocation :
 *  These prototypes make it possible to pass your own allocation/free functions.
 *  ZSTD_customMem is provided at creation time, using ZSTD_create*_advanced() variants listed below.
 *  All allocation/free operations will be completed using these custom variants instead of regular <stdlib.h> ones.
 */
typedef void* (*ZSTD_allocFunction) (void* opaque, size_t size);
typedef void  (*ZSTD_freeFunction) (void* opaque, void* address);
typedef struct { ZSTD_allocFunction customAlloc; ZSTD_freeFunction customFree; void* opaque; } ZSTD_customMem;
static
#if defined(__GNUC__)
__attribute__((__unused__))
#endif

#if defined(__clang__) && __clang_major__ >= 5
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
#endif
ZSTD_customMem const ZSTD_defaultCMem = { NULL, NULL, NULL };  
#if defined(__clang__) && __clang_major__ >= 5
#pragma clang diagnostic pop
#endif

ZSTDLIB_STATIC_API ZSTD_CCtx*    ZSTD_createCCtx_advanced(ZSTD_customMem customMem);
ZSTDLIB_STATIC_API ZSTD_CStream* ZSTD_createCStream_advanced(ZSTD_customMem customMem);
ZSTDLIB_STATIC_API ZSTD_DCtx*    ZSTD_createDCtx_advanced(ZSTD_customMem customMem);
ZSTDLIB_STATIC_API ZSTD_DStream* ZSTD_createDStream_advanced(ZSTD_customMem customMem);

ZSTDLIB_STATIC_API ZSTD_CDict* ZSTD_createCDict_advanced(const void* dict, size_t dictSize,
                                                  ZSTD_dictLoadMethod_e dictLoadMethod,
                                                  ZSTD_dictContentType_e dictContentType,
                                                  ZSTD_compressionParameters cParams,
                                                  ZSTD_customMem customMem);

/*! Thread pool :
 *  These prototypes make it possible to share a thread pool among multiple compression contexts.
 *  This can limit resources for applications with multiple threads where each one uses
 *  a threaded compression mode (via ZSTD_c_nbWorkers parameter).
 *  ZSTD_createThreadPool creates a new thread pool with a given number of threads.
 *  Note that the lifetime of such pool must exist while being used.
 *  ZSTD_CCtx_refThreadPool assigns a thread pool to a context (use NULL argument value
 *  to use an internal thread pool).
 *  ZSTD_freeThreadPool frees a thread pool, accepts NULL pointer.
 */
typedef struct POOL_ctx_s ZSTD_threadPool;
ZSTDLIB_STATIC_API ZSTD_threadPool* ZSTD_createThreadPool(size_t numThreads);
ZSTDLIB_STATIC_API void ZSTD_freeThreadPool (ZSTD_threadPool* pool);  
ZSTDLIB_STATIC_API size_t ZSTD_CCtx_refThreadPool(ZSTD_CCtx* cctx, ZSTD_threadPool* pool);


ZSTDLIB_STATIC_API ZSTD_CDict* ZSTD_createCDict_advanced2(
    const void* dict, size_t dictSize,
    ZSTD_dictLoadMethod_e dictLoadMethod,
    ZSTD_dictContentType_e dictContentType,
    const ZSTD_CCtx_params* cctxParams,
    ZSTD_customMem customMem);

ZSTDLIB_STATIC_API ZSTD_DDict* ZSTD_createDDict_advanced(
    const void* dict, size_t dictSize,
    ZSTD_dictLoadMethod_e dictLoadMethod,
    ZSTD_dictContentType_e dictContentType,
    ZSTD_customMem customMem);



/*! ZSTD_createCDict_byReference() :
 *  Create a digested dictionary for compression
 *  Dictionary content is just referenced, not duplicated.
 *  As a consequence, `dictBuffer` **must** outlive CDict,
 *  and its content must remain unmodified throughout the lifetime of CDict.
 *  note: equivalent to ZSTD_createCDict_advanced(), with dictLoadMethod==ZSTD_dlm_byRef */
ZSTDLIB_STATIC_API ZSTD_CDict* ZSTD_createCDict_byReference(const void* dictBuffer, size_t dictSize, int compressionLevel);

/*! ZSTD_getCParams() :
 * @return ZSTD_compressionParameters structure for a selected compression level and estimated srcSize.
 * `estimatedSrcSize` value is optional, select 0 if not known */
ZSTDLIB_STATIC_API ZSTD_compressionParameters ZSTD_getCParams(int compressionLevel, unsigned long long estimatedSrcSize, size_t dictSize);

/*! ZSTD_getParams() :
 *  same as ZSTD_getCParams(), but @return a full `ZSTD_parameters` object instead of sub-component `ZSTD_compressionParameters`.
 *  All fields of `ZSTD_frameParameters` are set to default : contentSize=1, checksum=0, noDictID=0 */
ZSTDLIB_STATIC_API ZSTD_parameters ZSTD_getParams(int compressionLevel, unsigned long long estimatedSrcSize, size_t dictSize);

/*! ZSTD_checkCParams() :
 *  Ensure param values remain within authorized range.
 * @return 0 on success, or an error code (can be checked with ZSTD_isError()) */
ZSTDLIB_STATIC_API size_t ZSTD_checkCParams(ZSTD_compressionParameters params);

/*! ZSTD_adjustCParams() :
 *  optimize params for a given `srcSize` and `dictSize`.
 * `srcSize` can be unknown, in which case use ZSTD_CONTENTSIZE_UNKNOWN.
 * `dictSize` must be `0` when there is no dictionary.
 *  cPar can be invalid : all parameters will be clamped within valid range in the @return struct.
 *  This function never fails (wide contract) */
ZSTDLIB_STATIC_API ZSTD_compressionParameters ZSTD_adjustCParams(ZSTD_compressionParameters cPar, unsigned long long srcSize, size_t dictSize);

/*! ZSTD_CCtx_setCParams() :
 *  Set all parameters provided within @p cparams into the working @p cctx.
 *  Note : if modifying parameters during compression (MT mode only),
 *         note that changes to the .windowLog parameter will be ignored.
 * @return 0 on success, or an error code (can be checked with ZSTD_isError()).
 *         On failure, no parameters are updated.
 */
ZSTDLIB_STATIC_API size_t ZSTD_CCtx_setCParams(ZSTD_CCtx* cctx, ZSTD_compressionParameters cparams);

/*! ZSTD_CCtx_setFParams() :
 *  Set all parameters provided within @p fparams into the working @p cctx.
 * @return 0 on success, or an error code (can be checked with ZSTD_isError()).
 */
ZSTDLIB_STATIC_API size_t ZSTD_CCtx_setFParams(ZSTD_CCtx* cctx, ZSTD_frameParameters fparams);

/*! ZSTD_CCtx_setParams() :
 *  Set all parameters provided within @p params into the working @p cctx.
 * @return 0 on success, or an error code (can be checked with ZSTD_isError()).
 */
ZSTDLIB_STATIC_API size_t ZSTD_CCtx_setParams(ZSTD_CCtx* cctx, ZSTD_parameters params);

/*! ZSTD_compress_advanced() :
 *  Note : this function is now DEPRECATED.
 *         It can be replaced by ZSTD_compress2(), in combination with ZSTD_CCtx_setParameter() and other parameter setters.
 *  This prototype will generate compilation warnings. */
ZSTD_DEPRECATED("use ZSTD_compress2")
ZSTDLIB_STATIC_API
size_t ZSTD_compress_advanced(ZSTD_CCtx* cctx,
                              void* dst, size_t dstCapacity,
                        const void* src, size_t srcSize,
                        const void* dict,size_t dictSize,
                              ZSTD_parameters params);

/*! ZSTD_compress_usingCDict_advanced() :
 *  Note : this function is now DEPRECATED.
 *         It can be replaced by ZSTD_compress2(), in combination with ZSTD_CCtx_loadDictionary() and other parameter setters.
 *  This prototype will generate compilation warnings. */
ZSTD_DEPRECATED("use ZSTD_compress2 with ZSTD_CCtx_loadDictionary")
ZSTDLIB_STATIC_API
size_t ZSTD_compress_usingCDict_advanced(ZSTD_CCtx* cctx,
                                              void* dst, size_t dstCapacity,
                                        const void* src, size_t srcSize,
                                        const ZSTD_CDict* cdict,
                                              ZSTD_frameParameters fParams);


/*! ZSTD_CCtx_loadDictionary_byReference() :
 *  Same as ZSTD_CCtx_loadDictionary(), but dictionary content is referenced, instead of being copied into CCtx.
 *  It saves some memory, but also requires that `dict` outlives its usage within `cctx` */
ZSTDLIB_STATIC_API size_t ZSTD_CCtx_loadDictionary_byReference(ZSTD_CCtx* cctx, const void* dict, size_t dictSize);

/*! ZSTD_CCtx_loadDictionary_advanced() :
 *  Same as ZSTD_CCtx_loadDictionary(), but gives finer control over
 *  how to load the dictionary (by copy ? by reference ?)
 *  and how to interpret it (automatic ? force raw mode ? full mode only ?) */
ZSTDLIB_STATIC_API size_t ZSTD_CCtx_loadDictionary_advanced(ZSTD_CCtx* cctx, const void* dict, size_t dictSize, ZSTD_dictLoadMethod_e dictLoadMethod, ZSTD_dictContentType_e dictContentType);

/*! ZSTD_CCtx_refPrefix_advanced() :
 *  Same as ZSTD_CCtx_refPrefix(), but gives finer control over
 *  how to interpret prefix content (automatic ? force raw mode (default) ? full mode only ?) */
ZSTDLIB_STATIC_API size_t ZSTD_CCtx_refPrefix_advanced(ZSTD_CCtx* cctx, const void* prefix, size_t prefixSize, ZSTD_dictContentType_e dictContentType);


 #define ZSTD_c_rsyncable ZSTD_c_experimentalParam1

#define ZSTD_c_format ZSTD_c_experimentalParam2

#define ZSTD_c_forceMaxWindow ZSTD_c_experimentalParam3

#define ZSTD_c_forceAttachDict ZSTD_c_experimentalParam4

#define ZSTD_c_literalCompressionMode ZSTD_c_experimentalParam5

#define ZSTD_c_srcSizeHint ZSTD_c_experimentalParam7

#define ZSTD_c_enableDedicatedDictSearch ZSTD_c_experimentalParam8

#define ZSTD_c_stableInBuffer ZSTD_c_experimentalParam9

#define ZSTD_c_stableOutBuffer ZSTD_c_experimentalParam10

#define ZSTD_c_blockDelimiters ZSTD_c_experimentalParam11

#define ZSTD_c_validateSequences ZSTD_c_experimentalParam12

#define ZSTD_BLOCKSPLITTER_LEVEL_MAX 6
#define ZSTD_c_blockSplitterLevel ZSTD_c_experimentalParam20

#define ZSTD_c_splitAfterSequences ZSTD_c_experimentalParam13

#define ZSTD_c_useRowMatchFinder ZSTD_c_experimentalParam14

#define ZSTD_c_deterministicRefPrefix ZSTD_c_experimentalParam15

#define ZSTD_c_prefetchCDictTables ZSTD_c_experimentalParam16

#define ZSTD_c_enableSeqProducerFallback ZSTD_c_experimentalParam17

#define ZSTD_c_maxBlockSize ZSTD_c_experimentalParam18

#define ZSTD_c_repcodeResolution ZSTD_c_experimentalParam19
#define ZSTD_c_searchForExternalRepcodes ZSTD_c_experimentalParam19 /* older name */


/*! ZSTD_CCtx_getParameter() :
 *  Get the requested compression parameter value, selected by enum ZSTD_cParameter,
 *  and store it into int* value.
 * @return : 0, or an error code (which can be tested with ZSTD_isError()).
 */
ZSTDLIB_STATIC_API size_t ZSTD_CCtx_getParameter(const ZSTD_CCtx* cctx, ZSTD_cParameter param, int* value);


/*! ZSTD_CCtx_params :
 *  Quick howto :
 *  - ZSTD_createCCtxParams() : Create a ZSTD_CCtx_params structure
 *  - ZSTD_CCtxParams_setParameter() : Push parameters one by one into
 *                                     an existing ZSTD_CCtx_params structure.
 *                                     This is similar to
 *                                     ZSTD_CCtx_setParameter().
 *  - ZSTD_CCtx_setParametersUsingCCtxParams() : Apply parameters to
 *                                    an existing CCtx.
 *                                    These parameters will be applied to
 *                                    all subsequent frames.
 *  - ZSTD_compressStream2() : Do compression using the CCtx.
 *  - ZSTD_freeCCtxParams() : Free the memory, accept NULL pointer.
 *
 *  This can be used with ZSTD_estimateCCtxSize_advanced_usingCCtxParams()
 *  for static allocation of CCtx for single-threaded compression.
 */
ZSTDLIB_STATIC_API ZSTD_CCtx_params* ZSTD_createCCtxParams(void);
ZSTDLIB_STATIC_API size_t ZSTD_freeCCtxParams(ZSTD_CCtx_params* params);  

/*! ZSTD_CCtxParams_reset() :
 *  Reset params to default values.
 */
ZSTDLIB_STATIC_API size_t ZSTD_CCtxParams_reset(ZSTD_CCtx_params* params);

/*! ZSTD_CCtxParams_init() :
 *  Initializes the compression parameters of cctxParams according to
 *  compression level. All other parameters are reset to their default values.
 */
ZSTDLIB_STATIC_API size_t ZSTD_CCtxParams_init(ZSTD_CCtx_params* cctxParams, int compressionLevel);

/*! ZSTD_CCtxParams_init_advanced() :
 *  Initializes the compression and frame parameters of cctxParams according to
 *  params. All other parameters are reset to their default values.
 */
ZSTDLIB_STATIC_API size_t ZSTD_CCtxParams_init_advanced(ZSTD_CCtx_params* cctxParams, ZSTD_parameters params);

/*! ZSTD_CCtxParams_setParameter() : Requires v1.4.0+
 *  Similar to ZSTD_CCtx_setParameter.
 *  Set one compression parameter, selected by enum ZSTD_cParameter.
 *  Parameters must be applied to a ZSTD_CCtx using
 *  ZSTD_CCtx_setParametersUsingCCtxParams().
 * @result : a code representing success or failure (which can be tested with
 *           ZSTD_isError()).
 */
ZSTDLIB_STATIC_API size_t ZSTD_CCtxParams_setParameter(ZSTD_CCtx_params* params, ZSTD_cParameter param, int value);

/*! ZSTD_CCtxParams_getParameter() :
 * Similar to ZSTD_CCtx_getParameter.
 * Get the requested value of one compression parameter, selected by enum ZSTD_cParameter.
 * @result : 0, or an error code (which can be tested with ZSTD_isError()).
 */
ZSTDLIB_STATIC_API size_t ZSTD_CCtxParams_getParameter(const ZSTD_CCtx_params* params, ZSTD_cParameter param, int* value);

/*! ZSTD_CCtx_setParametersUsingCCtxParams() :
 *  Apply a set of ZSTD_CCtx_params to the compression context.
 *  This can be done even after compression is started,
 *    if nbWorkers==0, this will have no impact until a new compression is started.
 *    if nbWorkers>=1, new parameters will be picked up at next job,
 *       with a few restrictions (windowLog, pledgedSrcSize, nbWorkers, jobSize, and overlapLog are not updated).
 */
ZSTDLIB_STATIC_API size_t ZSTD_CCtx_setParametersUsingCCtxParams(
        ZSTD_CCtx* cctx, const ZSTD_CCtx_params* params);

/*! ZSTD_compressStream2_simpleArgs() :
 *  Same as ZSTD_compressStream2(),
 *  but using only integral types as arguments.
 *  This variant might be helpful for binders from dynamic languages
 *  which have troubles handling structures containing memory pointers.
 */
ZSTDLIB_STATIC_API size_t ZSTD_compressStream2_simpleArgs (
                            ZSTD_CCtx* cctx,
                            void* dst, size_t dstCapacity, size_t* dstPos,
                      const void* src, size_t srcSize, size_t* srcPos,
                            ZSTD_EndDirective endOp);



/*! ZSTD_isFrame() :
 *  Tells if the content of `buffer` starts with a valid Frame Identifier.
 *  Note : Frame Identifier is 4 bytes. If `size < 4`, @return will always be 0.
 *  Note 2 : Legacy Frame Identifiers are considered valid only if Legacy Support is enabled.
 *  Note 3 : Skippable Frame Identifiers are considered valid. */
ZSTDLIB_STATIC_API unsigned ZSTD_isFrame(const void* buffer, size_t size);

/*! ZSTD_createDDict_byReference() :
 *  Create a digested dictionary, ready to start decompression operation without startup delay.
 *  Dictionary content is referenced, and therefore stays in dictBuffer.
 *  It is important that dictBuffer outlives DDict,
 *  it must remain read accessible throughout the lifetime of DDict */
ZSTDLIB_STATIC_API ZSTD_DDict* ZSTD_createDDict_byReference(const void* dictBuffer, size_t dictSize);

/*! ZSTD_DCtx_loadDictionary_byReference() :
 *  Same as ZSTD_DCtx_loadDictionary(),
 *  but references `dict` content instead of copying it into `dctx`.
 *  This saves memory if `dict` remains around.,
 *  However, it's imperative that `dict` remains accessible (and unmodified) while being used, so it must outlive decompression. */
ZSTDLIB_STATIC_API size_t ZSTD_DCtx_loadDictionary_byReference(ZSTD_DCtx* dctx, const void* dict, size_t dictSize);

/*! ZSTD_DCtx_loadDictionary_advanced() :
 *  Same as ZSTD_DCtx_loadDictionary(),
 *  but gives direct control over
 *  how to load the dictionary (by copy ? by reference ?)
 *  and how to interpret it (automatic ? force raw mode ? full mode only ?). */
ZSTDLIB_STATIC_API size_t ZSTD_DCtx_loadDictionary_advanced(ZSTD_DCtx* dctx, const void* dict, size_t dictSize, ZSTD_dictLoadMethod_e dictLoadMethod, ZSTD_dictContentType_e dictContentType);

/*! ZSTD_DCtx_refPrefix_advanced() :
 *  Same as ZSTD_DCtx_refPrefix(), but gives finer control over
 *  how to interpret prefix content (automatic ? force raw mode (default) ? full mode only ?) */
ZSTDLIB_STATIC_API size_t ZSTD_DCtx_refPrefix_advanced(ZSTD_DCtx* dctx, const void* prefix, size_t prefixSize, ZSTD_dictContentType_e dictContentType);

/*! ZSTD_DCtx_setMaxWindowSize() :
 *  Refuses allocating internal buffers for frames requiring a window size larger than provided limit.
 *  This protects a decoder context from reserving too much memory for itself (potential attack scenario).
 *  This parameter is only useful in streaming mode, since no internal buffer is allocated in single-pass mode.
 *  By default, a decompression context accepts all window sizes <= (1 << ZSTD_WINDOWLOG_LIMIT_DEFAULT)
 * @return : 0, or an error code (which can be tested using ZSTD_isError()).
 */
ZSTDLIB_STATIC_API size_t ZSTD_DCtx_setMaxWindowSize(ZSTD_DCtx* dctx, size_t maxWindowSize);

/*! ZSTD_DCtx_getParameter() :
 *  Get the requested decompression parameter value, selected by enum ZSTD_dParameter,
 *  and store it into int* value.
 * @return : 0, or an error code (which can be tested with ZSTD_isError()).
 */
ZSTDLIB_STATIC_API size_t ZSTD_DCtx_getParameter(ZSTD_DCtx* dctx, ZSTD_dParameter param, int* value);

#define ZSTD_d_format ZSTD_d_experimentalParam1
#define ZSTD_d_stableOutBuffer ZSTD_d_experimentalParam2

#define ZSTD_d_forceIgnoreChecksum ZSTD_d_experimentalParam3

#define ZSTD_d_refMultipleDDicts ZSTD_d_experimentalParam4

#define ZSTD_d_disableHuffmanAssembly ZSTD_d_experimentalParam5

#define ZSTD_d_maxBlockSize ZSTD_d_experimentalParam6


/*! ZSTD_DCtx_setFormat() :
 *  This function is REDUNDANT. Prefer ZSTD_DCtx_setParameter().
 *  Instruct the decoder context about what kind of data to decode next.
 *  This instruction is mandatory to decode data without a fully-formed header,
 *  such ZSTD_f_zstd1_magicless for example.
 * @return : 0, or an error code (which can be tested using ZSTD_isError()). */
ZSTD_DEPRECATED("use ZSTD_DCtx_setParameter() instead")
ZSTDLIB_STATIC_API
size_t ZSTD_DCtx_setFormat(ZSTD_DCtx* dctx, ZSTD_format_e format);

/*! ZSTD_decompressStream_simpleArgs() :
 *  Same as ZSTD_decompressStream(),
 *  but using only integral types as arguments.
 *  This can be helpful for binders from dynamic languages
 *  which have troubles handling structures containing memory pointers.
 */
ZSTDLIB_STATIC_API size_t ZSTD_decompressStream_simpleArgs (
                            ZSTD_DCtx* dctx,
                            void* dst, size_t dstCapacity, size_t* dstPos,
                      const void* src, size_t srcSize, size_t* srcPos);




/*! ZSTD_initCStream_srcSize() :
 * This function is DEPRECATED, and equivalent to:
 *     ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only);
 *     ZSTD_CCtx_refCDict(zcs, NULL); // clear the dictionary (if any)
 *     ZSTD_CCtx_setParameter(zcs, ZSTD_c_compressionLevel, compressionLevel);
 *     ZSTD_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize);
 *
 * pledgedSrcSize must be correct. If it is not known at init time, use
 * ZSTD_CONTENTSIZE_UNKNOWN. Note that, for compatibility with older programs,
 * "0" also disables frame content size field. It may be enabled in the future.
 * This prototype will generate compilation warnings.
 */
ZSTD_DEPRECATED("use ZSTD_CCtx_reset, see zstd.h for detailed instructions")
ZSTDLIB_STATIC_API
size_t ZSTD_initCStream_srcSize(ZSTD_CStream* zcs,
                         int compressionLevel,
                         unsigned long long pledgedSrcSize);

/*! ZSTD_initCStream_usingDict() :
 * This function is DEPRECATED, and is equivalent to:
 *     ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only);
 *     ZSTD_CCtx_setParameter(zcs, ZSTD_c_compressionLevel, compressionLevel);
 *     ZSTD_CCtx_loadDictionary(zcs, dict, dictSize);
 *
 * Creates of an internal CDict (incompatible with static CCtx), except if
 * dict == NULL or dictSize < 8, in which case no dict is used.
 * Note: dict is loaded with ZSTD_dct_auto (treated as a full zstd dictionary if
 * it begins with ZSTD_MAGIC_DICTIONARY, else as raw content) and ZSTD_dlm_byCopy.
 * This prototype will generate compilation warnings.
 */
ZSTD_DEPRECATED("use ZSTD_CCtx_reset, see zstd.h for detailed instructions")
ZSTDLIB_STATIC_API
size_t ZSTD_initCStream_usingDict(ZSTD_CStream* zcs,
                     const void* dict, size_t dictSize,
                           int compressionLevel);

/*! ZSTD_initCStream_advanced() :
 * This function is DEPRECATED, and is equivalent to:
 *     ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only);
 *     ZSTD_CCtx_setParams(zcs, params);
 *     ZSTD_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize);
 *     ZSTD_CCtx_loadDictionary(zcs, dict, dictSize);
 *
 * dict is loaded with ZSTD_dct_auto and ZSTD_dlm_byCopy.
 * pledgedSrcSize must be correct.
 * If srcSize is not known at init time, use value ZSTD_CONTENTSIZE_UNKNOWN.
 * This prototype will generate compilation warnings.
 */
ZSTD_DEPRECATED("use ZSTD_CCtx_reset, see zstd.h for detailed instructions")
ZSTDLIB_STATIC_API
size_t ZSTD_initCStream_advanced(ZSTD_CStream* zcs,
                    const void* dict, size_t dictSize,
                          ZSTD_parameters params,
                          unsigned long long pledgedSrcSize);

/*! ZSTD_initCStream_usingCDict() :
 * This function is DEPRECATED, and equivalent to:
 *     ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only);
 *     ZSTD_CCtx_refCDict(zcs, cdict);
 *
 * note : cdict will just be referenced, and must outlive compression session
 * This prototype will generate compilation warnings.
 */
ZSTD_DEPRECATED("use ZSTD_CCtx_reset and ZSTD_CCtx_refCDict, see zstd.h for detailed instructions")
ZSTDLIB_STATIC_API
size_t ZSTD_initCStream_usingCDict(ZSTD_CStream* zcs, const ZSTD_CDict* cdict);

/*! ZSTD_initCStream_usingCDict_advanced() :
 *   This function is DEPRECATED, and is equivalent to:
 *     ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only);
 *     ZSTD_CCtx_setFParams(zcs, fParams);
 *     ZSTD_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize);
 *     ZSTD_CCtx_refCDict(zcs, cdict);
 *
 * same as ZSTD_initCStream_usingCDict(), with control over frame parameters.
 * pledgedSrcSize must be correct. If srcSize is not known at init time, use
 * value ZSTD_CONTENTSIZE_UNKNOWN.
 * This prototype will generate compilation warnings.
 */
ZSTD_DEPRECATED("use ZSTD_CCtx_reset and ZSTD_CCtx_refCDict, see zstd.h for detailed instructions")
ZSTDLIB_STATIC_API
size_t ZSTD_initCStream_usingCDict_advanced(ZSTD_CStream* zcs,
                               const ZSTD_CDict* cdict,
                                     ZSTD_frameParameters fParams,
                                     unsigned long long pledgedSrcSize);

/*! ZSTD_resetCStream() :
 * This function is DEPRECATED, and is equivalent to:
 *     ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only);
 *     ZSTD_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize);
 * Note: ZSTD_resetCStream() interprets pledgedSrcSize == 0 as ZSTD_CONTENTSIZE_UNKNOWN, but
 *       ZSTD_CCtx_setPledgedSrcSize() does not do the same, so ZSTD_CONTENTSIZE_UNKNOWN must be
 *       explicitly specified.
 *
 *  start a new frame, using same parameters from previous frame.
 *  This is typically useful to skip dictionary loading stage, since it will reuse it in-place.
 *  Note that zcs must be init at least once before using ZSTD_resetCStream().
 *  If pledgedSrcSize is not known at reset time, use macro ZSTD_CONTENTSIZE_UNKNOWN.
 *  If pledgedSrcSize > 0, its value must be correct, as it will be written in header, and controlled at the end.
 *  For the time being, pledgedSrcSize==0 is interpreted as "srcSize unknown" for compatibility with older programs,
 *  but it will change to mean "empty" in future version, so use macro ZSTD_CONTENTSIZE_UNKNOWN instead.
 * @return : 0, or an error code (which can be tested using ZSTD_isError())
 *  This prototype will generate compilation warnings.
 */
ZSTD_DEPRECATED("use ZSTD_CCtx_reset, see zstd.h for detailed instructions")
ZSTDLIB_STATIC_API
size_t ZSTD_resetCStream(ZSTD_CStream* zcs, unsigned long long pledgedSrcSize);


typedef struct {
    unsigned long long ingested;   
    unsigned long long consumed;   
    unsigned long long produced;   
    unsigned long long flushed;    
    unsigned currentJobID;         
    unsigned nbActiveWorkers;      
} ZSTD_frameProgression;

ZSTDLIB_STATIC_API ZSTD_frameProgression ZSTD_getFrameProgression(const ZSTD_CCtx* cctx);

/*! ZSTD_toFlushNow() :
 *  Tell how many bytes are ready to be flushed immediately.
 *  Useful for multithreading scenarios (nbWorkers >= 1).
 *  Probe the oldest active job, defined as oldest job not yet entirely flushed,
 *  and check its output buffer.
 * @return : amount of data stored in oldest job and ready to be flushed immediately.
 *  if @return == 0, it means either :
 *  + there is no active job (could be checked with ZSTD_frameProgression()), or
 *  + oldest job is still actively compressing data,
 *    but everything it has produced has also been flushed so far,
 *    therefore flush speed is limited by production speed of oldest job
 *    irrespective of the speed of concurrent (and newer) jobs.
 */
ZSTDLIB_STATIC_API size_t ZSTD_toFlushNow(ZSTD_CCtx* cctx);



/*!
 * This function is deprecated, and is equivalent to:
 *
 *     ZSTD_DCtx_reset(zds, ZSTD_reset_session_only);
 *     ZSTD_DCtx_loadDictionary(zds, dict, dictSize);
 *
 * note: no dictionary will be used if dict == NULL or dictSize < 8
 */
ZSTD_DEPRECATED("use ZSTD_DCtx_reset + ZSTD_DCtx_loadDictionary, see zstd.h for detailed instructions")
ZSTDLIB_STATIC_API size_t ZSTD_initDStream_usingDict(ZSTD_DStream* zds, const void* dict, size_t dictSize);

/*!
 * This function is deprecated, and is equivalent to:
 *
 *     ZSTD_DCtx_reset(zds, ZSTD_reset_session_only);
 *     ZSTD_DCtx_refDDict(zds, ddict);
 *
 * note : ddict is referenced, it must outlive decompression session
 */
ZSTD_DEPRECATED("use ZSTD_DCtx_reset + ZSTD_DCtx_refDDict, see zstd.h for detailed instructions")
ZSTDLIB_STATIC_API size_t ZSTD_initDStream_usingDDict(ZSTD_DStream* zds, const ZSTD_DDict* ddict);

/*!
 * This function is deprecated, and is equivalent to:
 *
 *     ZSTD_DCtx_reset(zds, ZSTD_reset_session_only);
 *
 * reuse decompression parameters from previous init; saves dictionary loading
 */
ZSTD_DEPRECATED("use ZSTD_DCtx_reset, see zstd.h for detailed instructions")
ZSTDLIB_STATIC_API size_t ZSTD_resetDStream(ZSTD_DStream* zds);



#define ZSTD_SEQUENCE_PRODUCER_ERROR ((size_t)(-1))

typedef size_t (*ZSTD_sequenceProducer_F) (
  void* sequenceProducerState,
  ZSTD_Sequence* outSeqs, size_t outSeqsCapacity,
  const void* src, size_t srcSize,
  const void* dict, size_t dictSize,
  int compressionLevel,
  size_t windowSize
);

/*! ZSTD_registerSequenceProducer() :
 * Instruct zstd to use a block-level external sequence producer function.
 *
 * The sequenceProducerState must be initialized by the caller, and the caller is
 * responsible for managing its lifetime. This parameter is sticky across
 * compressions. It will remain set until the user explicitly resets compression
 * parameters.
 *
 * Sequence producer registration is considered to be an "advanced parameter",
 * part of the "advanced API". This means it will only have an effect on compression
 * APIs which respect advanced parameters, such as compress2() and compressStream2().
 * Older compression APIs such as compressCCtx(), which predate the introduction of
 * "advanced parameters", will ignore any external sequence producer setting.
 *
 * The sequence producer can be "cleared" by registering a NULL function pointer. This
 * removes all limitations described above in the "LIMITATIONS" section of the API docs.
 *
 * The user is strongly encouraged to read the full API documentation (above) before
 * calling this function. */
ZSTDLIB_STATIC_API void
ZSTD_registerSequenceProducer(
  ZSTD_CCtx* cctx,
  void* sequenceProducerState,
  ZSTD_sequenceProducer_F sequenceProducer
);

/*! ZSTD_CCtxParams_registerSequenceProducer() :
 * Same as ZSTD_registerSequenceProducer(), but operates on ZSTD_CCtx_params.
 * This is used for accurate size estimation with ZSTD_estimateCCtxSize_usingCCtxParams(),
 * which is needed when creating a ZSTD_CCtx with ZSTD_initStaticCCtx().
 *
 * If you are using the external sequence producer API in a scenario where ZSTD_initStaticCCtx()
 * is required, then this function is for you. Otherwise, you probably don't need it.
 *
 * See tests/zstreamtest.c for example usage. */
ZSTDLIB_STATIC_API void
ZSTD_CCtxParams_registerSequenceProducer(
  ZSTD_CCtx_params* params,
  void* sequenceProducerState,
  ZSTD_sequenceProducer_F sequenceProducer
);




ZSTD_DEPRECATED("The buffer-less API is deprecated in favor of the normal streaming API. See docs.")
ZSTDLIB_STATIC_API size_t ZSTD_compressBegin(ZSTD_CCtx* cctx, int compressionLevel);
ZSTD_DEPRECATED("The buffer-less API is deprecated in favor of the normal streaming API. See docs.")
ZSTDLIB_STATIC_API size_t ZSTD_compressBegin_usingDict(ZSTD_CCtx* cctx, const void* dict, size_t dictSize, int compressionLevel);
ZSTD_DEPRECATED("The buffer-less API is deprecated in favor of the normal streaming API. See docs.")
ZSTDLIB_STATIC_API size_t ZSTD_compressBegin_usingCDict(ZSTD_CCtx* cctx, const ZSTD_CDict* cdict); 

ZSTD_DEPRECATED("This function will likely be removed in a future release. It is misleading and has very limited utility.")
ZSTDLIB_STATIC_API
size_t ZSTD_copyCCtx(ZSTD_CCtx* cctx, const ZSTD_CCtx* preparedCCtx, unsigned long long pledgedSrcSize); 

ZSTD_DEPRECATED("The buffer-less API is deprecated in favor of the normal streaming API. See docs.")
ZSTDLIB_STATIC_API size_t ZSTD_compressContinue(ZSTD_CCtx* cctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);
ZSTD_DEPRECATED("The buffer-less API is deprecated in favor of the normal streaming API. See docs.")
ZSTDLIB_STATIC_API size_t ZSTD_compressEnd(ZSTD_CCtx* cctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);

ZSTD_DEPRECATED("use advanced API to access custom parameters")
ZSTDLIB_STATIC_API
size_t ZSTD_compressBegin_advanced(ZSTD_CCtx* cctx, const void* dict, size_t dictSize, ZSTD_parameters params, unsigned long long pledgedSrcSize); 
ZSTD_DEPRECATED("use advanced API to access custom parameters")
ZSTDLIB_STATIC_API
size_t ZSTD_compressBegin_usingCDict_advanced(ZSTD_CCtx* const cctx, const ZSTD_CDict* const cdict, ZSTD_frameParameters const fParams, unsigned long long const pledgedSrcSize);   


ZSTDLIB_STATIC_API size_t ZSTD_decodingBufferSize_min(unsigned long long windowSize, unsigned long long frameContentSize);  

ZSTDLIB_STATIC_API size_t ZSTD_decompressBegin(ZSTD_DCtx* dctx);
ZSTDLIB_STATIC_API size_t ZSTD_decompressBegin_usingDict(ZSTD_DCtx* dctx, const void* dict, size_t dictSize);
ZSTDLIB_STATIC_API size_t ZSTD_decompressBegin_usingDDict(ZSTD_DCtx* dctx, const ZSTD_DDict* ddict);

ZSTDLIB_STATIC_API size_t ZSTD_nextSrcSizeToDecompress(ZSTD_DCtx* dctx);
ZSTDLIB_STATIC_API size_t ZSTD_decompressContinue(ZSTD_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);

ZSTD_DEPRECATED("This function will likely be removed in the next minor release. It is misleading and has very limited utility.")
ZSTDLIB_STATIC_API void   ZSTD_copyDCtx(ZSTD_DCtx* dctx, const ZSTD_DCtx* preparedDCtx);
typedef enum { ZSTDnit_frameHeader, ZSTDnit_blockHeader, ZSTDnit_block, ZSTDnit_lastBlock, ZSTDnit_checksum, ZSTDnit_skippableFrame } ZSTD_nextInputType_e;
ZSTDLIB_STATIC_API ZSTD_nextInputType_e ZSTD_nextInputType(ZSTD_DCtx* dctx);





/*!

    This API is deprecated in favor of the regular compression API.
    You can get the frame header down to 2 bytes by setting:
      - ZSTD_c_format = ZSTD_f_zstd1_magicless
      - ZSTD_c_contentSizeFlag = 0
      - ZSTD_c_checksumFlag = 0
      - ZSTD_c_dictIDFlag = 0

    This API is not as well tested as our normal API, so we recommend not using it.
    We will be removing it in a future version. If the normal API doesn't provide
    the functionality you need, please open a GitHub issue.

    Block functions produce and decode raw zstd blocks, without frame metadata.
    Frame metadata cost is typically ~12 bytes, which can be non-negligible for very small blocks (< 100 bytes).
    But users will have to take in charge needed metadata to regenerate data, such as compressed and content sizes.

    A few rules to respect :
    - Compressing and decompressing require a context structure
      + Use ZSTD_createCCtx() and ZSTD_createDCtx()
    - It is necessary to init context before starting
      + compression : any ZSTD_compressBegin*() variant, including with dictionary
      + decompression : any ZSTD_decompressBegin*() variant, including with dictionary
    - Block size is limited, it must be <= ZSTD_getBlockSize() <= ZSTD_BLOCKSIZE_MAX == 128 KB
      + If input is larger than a block size, it's necessary to split input data into multiple blocks
      + For inputs larger than a single block, consider using regular ZSTD_compress() instead.
        Frame metadata is not that costly, and quickly becomes negligible as source size grows larger than a block.
    - When a block is considered not compressible enough, ZSTD_compressBlock() result will be 0 (zero) !
      ===> In which case, nothing is produced into `dst` !
      + User __must__ test for such outcome and deal directly with uncompressed data
      + A block cannot be declared incompressible if ZSTD_compressBlock() return value was != 0.
        Doing so would mess up with statistics history, leading to potential data corruption.
      + ZSTD_decompressBlock() _doesn't accept uncompressed data as input_ !!
      + In case of multiple successive blocks, should some of them be uncompressed,
        decoder must be informed of their existence in order to follow proper history.
        Use ZSTD_insertBlock() for such a case.
*/

ZSTD_DEPRECATED("The block API is deprecated in favor of the normal compression API. See docs.")
ZSTDLIB_STATIC_API size_t ZSTD_getBlockSize   (const ZSTD_CCtx* cctx);
ZSTD_DEPRECATED("The block API is deprecated in favor of the normal compression API. See docs.")
ZSTDLIB_STATIC_API size_t ZSTD_compressBlock  (ZSTD_CCtx* cctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);
ZSTD_DEPRECATED("The block API is deprecated in favor of the normal compression API. See docs.")
ZSTDLIB_STATIC_API size_t ZSTD_decompressBlock(ZSTD_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);
ZSTD_DEPRECATED("The block API is deprecated in favor of the normal compression API. See docs.")
ZSTDLIB_STATIC_API size_t ZSTD_insertBlock    (ZSTD_DCtx* dctx, const void* blockStart, size_t blockSize);  

#if defined (__cplusplus)
}
#endif

#endif
