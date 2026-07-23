/*
 *  blname.c - determine the freebl library name.
 *  This Source Code Form is subject to the terms of the Mozilla Public
 *  License, v. 2.0. If a copy of the MPL was not distributed with this
 *  file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if defined(FREEBL_LOWHASH)
static const char* default_name =
    SHLIB_PREFIX "freeblpriv" SHLIB_VERSION "." SHLIB_SUFFIX;
#else
static const char* default_name =
    SHLIB_PREFIX "freebl" SHLIB_VERSION "." SHLIB_SUFFIX;
#endif


#if defined(HPUX) && !defined(NSS_USE_64) && !defined(__ia64)
#include <unistd.h>

static const char*
getLibName(void)
{
    long cpu = sysconf(_SC_CPU_VERSION);
    return (cpu == CPU_PA_RISC2_0)
               ? "libfreebl_32fpu_3.sl"
               : "libfreebl_32int_3.sl";
}
#else
static const char*
getLibName(void)
{
    return default_name;
}
#endif
