// License & terms of use: http://www.unicode.org/copyright.html
/*
******************************************************************************
*
* Copyright (C) 2009-2011, International Business Machines
*                Corporation and others. All Rights Reserved.
*
******************************************************************************
*   file name:  ucln_imp.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   This file contains the platform specific implementation of per-library cleanup.
*
*/


#if !defined(__UCLN_IMP_H__)
#define __UCLN_IMP_H__

#include "ucln.h"
#include <stdlib.h>


#if !UCLN_NO_AUTO_CLEANUP


#if defined(UCLN_TYPE_IS_COMMON)
#   define UCLN_CLEAN_ME_UP u_cleanup()
#else
#   define UCLN_CLEAN_ME_UP ucln_cleanupOne(UCLN_TYPE)
#endif

#if defined(UCLN_AUTO_LOCAL)
#include "ucln_local_hook.c"

#elif defined(UCLN_AUTO_ATEXIT)
static UBool gAutoCleanRegistered = false;

static void ucln_atexit_handler()
{
    UCLN_CLEAN_ME_UP;
}

static void ucln_registerAutomaticCleanup()
{
    if(!gAutoCleanRegistered) {
        gAutoCleanRegistered = true;
        atexit(&ucln_atexit_handler);
    }
}

static void ucln_unRegisterAutomaticCleanup () {
}

#elif defined (UCLN_FINI)
U_CAPI void U_EXPORT2 UCLN_FINI (void);

U_CAPI void U_EXPORT2 UCLN_FINI ()
{
     UCLN_CLEAN_ME_UP;
}

#elif defined(__GNUC__)
static void ucln_destructor()   __attribute__((destructor)) ;

static void ucln_destructor() 
{
    UCLN_CLEAN_ME_UP;
}

#endif

#endif

#else
#error This file can only be included once.
#endif
