/// Author        : Copyright (c) Olli Parviainen
// License :
//  Copyright (c) Olli Parviainen
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.
//  Lesser General Public License for more details.
//  License along with this library; if not, write to the Free Software

#if !defined(STTypes_H)
#define STTypes_H

typedef unsigned int    uint;
typedef unsigned long   ulong;

    typedef ulong ulongptr;


#define SOUNDTOUCH_ALIGN_POINTER_16(x)      ( ( (ulongptr)(x) + 15 ) & ~(ulongptr)15 )


#include "soundtouch_config.h"


namespace soundtouch
{
    #define SOUNDTOUCH_MAX_CHANNELS     32




    #if !(SOUNDTOUCH_INTEGER_SAMPLES || SOUNDTOUCH_FLOAT_SAMPLES)

        #define SOUNDTOUCH_FLOAT_SAMPLES       1    //< 32bit float samples

    #endif

    #if (_M_IX86 || __i386__ || __x86_64__ || _M_X64)

        #define SOUNDTOUCH_ALLOW_X86_OPTIMIZATIONS     1

        #if defined(SOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS)
            #undef SOUNDTOUCH_ALLOW_X86_OPTIMIZATIONS
        #endif
    #else
        #undef SOUNDTOUCH_ALLOW_X86_OPTIMIZATIONS

    #endif



    #if defined(SOUNDTOUCH_INTEGER_SAMPLES)
        typedef short SAMPLETYPE;
        typedef long  LONG_SAMPLETYPE;

        #if defined(SOUNDTOUCH_FLOAT_SAMPLES)
            #error "conflicting sample types defined"
        #endif

        #if defined(SOUNDTOUCH_ALLOW_X86_OPTIMIZATIONS)
            #if (!_M_X64)
                #define SOUNDTOUCH_ALLOW_MMX   1
            #endif
        #endif

    #else

        typedef float  SAMPLETYPE;
        typedef float LONG_SAMPLETYPE;

        #if defined(SOUNDTOUCH_ALLOW_X86_OPTIMIZATIONS)
            #define SOUNDTOUCH_ALLOW_SSE       1
        #endif

    #endif

    #if ((SOUNDTOUCH_ALLOW_SSE) || (__SSE__) || (SOUNDTOUCH_USE_NEON))
        #if SOUNDTOUCH_ALLOW_NONEXACT_SIMD_OPTIMIZATION
            #define ST_SIMD_AVOID_UNALIGNED
        #endif
    #endif

}

#define ST_NO_EXCEPTION_HANDLING 1
#if defined(ST_NO_EXCEPTION_HANDLING)
    #include <assert.h>
    #define ST_THROW_RT_ERROR(x)    {assert((const char *)x);}
#else
    #include <stdexcept>
    #include <string>
    #define ST_THROW_RT_ERROR(x)    {throw std::runtime_error(x);}
#endif


#endif
