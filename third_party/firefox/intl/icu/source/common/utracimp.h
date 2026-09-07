// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2003-2009, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  utracimp.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2003aug06
*   created by: Markus W. Scherer
*
*   Internal header for ICU tracing/logging.
*
*
*   Various notes:
*   - using a trace level variable to only call trace functions
*     when the level is sufficient
*   - using the same variable for tracing on/off to never make a function
*     call when off
*   - the function number is put into a local variable by the entry macro
*     and used implicitly to avoid copy&paste/typing mistakes by the developer
*   - the application must call utrace_setFunctions() and pass in
*     implementations for the trace functions
*   - ICU trace macros call ICU functions that route through the function
*     pointers if they have been set;
*     this avoids an indirection at the call site
*     (which would cost more code for another check and for the indirection)
*
*   ### TODO Issues:
*   - Verify that va_list is portable among compilers for the same platform.
*     va_list should be portable because printf() would fail otherwise!
*   - Should enum values like UTraceLevel be passed into int32_t-type arguments,
*     or should enum types be used?
*/

#ifndef __UTRACIMP_H__
#define __UTRACIMP_H__

#include "unicode/utrace.h"
#include <stdarg.h>

U_CDECL_BEGIN

typedef enum UTraceExitVal {
    UTRACE_EXITV_NONE   = 0,
    UTRACE_EXITV_I32    = 1,
    UTRACE_EXITV_PTR    = 2,
    UTRACE_EXITV_BOOL   = 3,
    UTRACE_EXITV_MASK   = 0xf,
    UTRACE_EXITV_STATUS = 0x10
} UTraceExitVal;

U_CAPI void U_EXPORT2
utrace_entry(int32_t fnNumber);

U_CAPI void U_EXPORT2
utrace_exit(int32_t fnNumber, int32_t returnType, ...);


U_CAPI void U_EXPORT2
utrace_data(int32_t utraceFnNumber, int32_t level, const char *fmt, ...);

U_CDECL_END

#if U_ENABLE_TRACING

#define UTRACE_LEVEL(level) (utrace_getLevel()>=(level))

#define UTRACE_TRACED_ENTRY 0x80000000

#define UTRACE_ENTRY(fnNumber) \
    int32_t utraceFnNumber=(fnNumber); \
UPRV_BLOCK_MACRO_BEGIN { \
    if(utrace_getLevel()>=UTRACE_INFO) { \
        utrace_entry(fnNumber); \
        utraceFnNumber |= UTRACE_TRACED_ENTRY; \
    } \
} UPRV_BLOCK_MACRO_END


#define UTRACE_ENTRY_OC(fnNumber) \
    int32_t utraceFnNumber=(fnNumber); \
UPRV_BLOCK_MACRO_BEGIN { \
    if(utrace_getLevel()>=UTRACE_OPEN_CLOSE) { \
        utrace_entry(fnNumber); \
        utraceFnNumber |= UTRACE_TRACED_ENTRY; \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_EXIT() UPRV_BLOCK_MACRO_BEGIN { \
    if(utraceFnNumber & UTRACE_TRACED_ENTRY) { \
        utrace_exit(utraceFnNumber & ~UTRACE_TRACED_ENTRY, UTRACE_EXITV_NONE); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_EXIT_VALUE(val) UPRV_BLOCK_MACRO_BEGIN { \
    if(utraceFnNumber & UTRACE_TRACED_ENTRY) { \
        utrace_exit(utraceFnNumber & ~UTRACE_TRACED_ENTRY, UTRACE_EXITV_I32, val); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_EXIT_STATUS(status) UPRV_BLOCK_MACRO_BEGIN { \
    if(utraceFnNumber & UTRACE_TRACED_ENTRY) { \
        utrace_exit(utraceFnNumber & ~UTRACE_TRACED_ENTRY, UTRACE_EXITV_STATUS, status); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_EXIT_VALUE_STATUS(val, status) UPRV_BLOCK_MACRO_BEGIN { \
    if(utraceFnNumber & UTRACE_TRACED_ENTRY) { \
        utrace_exit(utraceFnNumber & ~UTRACE_TRACED_ENTRY, (UTRACE_EXITV_I32 | UTRACE_EXITV_STATUS), val, status); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_EXIT_PTR_STATUS(ptr, status) UPRV_BLOCK_MACRO_BEGIN { \
    if(utraceFnNumber & UTRACE_TRACED_ENTRY) { \
        utrace_exit(utraceFnNumber & ~UTRACE_TRACED_ENTRY, (UTRACE_EXITV_PTR | UTRACE_EXITV_STATUS), ptr, status); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_DATA0(level, fmt) UPRV_BLOCK_MACRO_BEGIN { \
    if(UTRACE_LEVEL(level)) { \
        utrace_data(utraceFnNumber & ~UTRACE_TRACED_ENTRY, (level), (fmt)); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_DATA1(level, fmt, a) UPRV_BLOCK_MACRO_BEGIN { \
    if(UTRACE_LEVEL(level)) { \
        utrace_data(utraceFnNumber & ~UTRACE_TRACED_ENTRY , (level), (fmt), (a)); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_DATA2(level, fmt, a, b) UPRV_BLOCK_MACRO_BEGIN { \
    if(UTRACE_LEVEL(level)) { \
        utrace_data(utraceFnNumber & ~UTRACE_TRACED_ENTRY , (level), (fmt), (a), (b)); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_DATA3(level, fmt, a, b, c) UPRV_BLOCK_MACRO_BEGIN { \
    if(UTRACE_LEVEL(level)) { \
        utrace_data(utraceFnNumber & ~UTRACE_TRACED_ENTRY, (level), (fmt), (a), (b), (c)); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_DATA4(level, fmt, a, b, c, d) UPRV_BLOCK_MACRO_BEGIN { \
    if(UTRACE_LEVEL(level)) { \
        utrace_data(utraceFnNumber & ~UTRACE_TRACED_ENTRY, (level), (fmt), (a), (b), (c), (d)); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_DATA5(level, fmt, a, b, c, d, e) UPRV_BLOCK_MACRO_BEGIN { \
    if(UTRACE_LEVEL(level)) { \
        utrace_data(utraceFnNumber & ~UTRACE_TRACED_ENTRY, (level), (fmt), (a), (b), (c), (d), (e)); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_DATA6(level, fmt, a, b, c, d, e, f) UPRV_BLOCK_MACRO_BEGIN { \
    if(UTRACE_LEVEL(level)) { \
        utrace_data(utraceFnNumber & ~UTRACE_TRACED_ENTRY, (level), (fmt), (a), (b), (c), (d), (e), (f)); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_DATA7(level, fmt, a, b, c, d, e, f, g) UPRV_BLOCK_MACRO_BEGIN { \
    if(UTRACE_LEVEL(level)) { \
        utrace_data(utraceFnNumber & ~UTRACE_TRACED_ENTRY, (level), (fmt), (a), (b), (c), (d), (e), (f), (g)); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_DATA8(level, fmt, a, b, c, d, e, f, g, h) UPRV_BLOCK_MACRO_BEGIN { \
    if(UTRACE_LEVEL(level)) { \
        utrace_data(utraceFnNumber & ~UTRACE_TRACED_ENTRY, (level), (fmt), (a), (b), (c), (d), (e), (f), (g), (h)); \
    } \
} UPRV_BLOCK_MACRO_END

#define UTRACE_DATA9(level, fmt, a, b, c, d, e, f, g, h, i) UPRV_BLOCK_MACRO_BEGIN { \
    if(UTRACE_LEVEL(level)) { \
        utrace_data(utraceFnNumber & ~UTRACE_TRACED_ENTRY, (level), (fmt), (a), (b), (c), (d), (e), (f), (g), (h), (i)); \
    } \
} UPRV_BLOCK_MACRO_END

#else


#define UTRACE_LEVEL(level) 0
#define UTRACE_ENTRY(fnNumber)
#define UTRACE_ENTRY_OC(fnNumber)
#define UTRACE_EXIT()
#define UTRACE_EXIT_VALUE(val)
#define UTRACE_EXIT_STATUS(status)
#define UTRACE_EXIT_VALUE_STATUS(val, status)
#define UTRACE_EXIT_PTR_STATUS(ptr, status)
#define UTRACE_DATA0(level, fmt)
#define UTRACE_DATA1(level, fmt, a)
#define UTRACE_DATA2(level, fmt, a, b)
#define UTRACE_DATA3(level, fmt, a, b, c)
#define UTRACE_DATA4(level, fmt, a, b, c, d)
#define UTRACE_DATA5(level, fmt, a, b, c, d, e)
#define UTRACE_DATA6(level, fmt, a, b, c, d, e, f)
#define UTRACE_DATA7(level, fmt, a, b, c, d, e, f, g)
#define UTRACE_DATA8(level, fmt, a, b, c, d, e, f, g, h)
#define UTRACE_DATA9(level, fmt, a, b, c, d, e, f, g, h, i)

#endif

#endif
