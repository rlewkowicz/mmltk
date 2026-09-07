/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */



#if defined(_PRVERSION_H)
#else
#define _PRVERSION_H

#include "prtypes.h"

PR_BEGIN_EXTERN_C



typedef struct {
    PRInt32    version;

    PRInt64         buildTime;      
    char *          buildTimeString;

    PRUint8   vMajor;               
    PRUint8   vMinor;               
    PRUint8   vPatch;               

    PRBool          beta;           
    PRBool          debug;          
    PRBool          special;        

    char *          filename;       
    char *          description;    
    char *          security;       
    char *          copyright;      /* The copyright for this file */
    char *          comment;        
    char *          specialString;  
} PRVersionDescription;


typedef const PRVersionDescription *(*versionEntryPointType)(void);



PR_END_EXTERN_C

#endif


