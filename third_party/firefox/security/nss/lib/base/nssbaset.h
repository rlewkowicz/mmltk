/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NSSBASET_H
#define NSSBASET_H


#include "nspr.h"


#define DUMMY /* dummy */
#define NSS_EXTERN extern
#define NSS_EXTERN_DATA extern
#define NSS_IMPLEMENT
#define NSS_IMPLEMENT_DATA

PR_BEGIN_EXTERN_C


typedef PRInt32 NSSError;


struct NSSArenaStr;
typedef struct NSSArenaStr NSSArena;


struct NSSItemStr {
    void *data;
    PRUint32 size;
};
typedef struct NSSItemStr NSSItem;


typedef NSSItem NSSBER;


typedef NSSBER NSSDER;


typedef NSSItem NSSBitString;


typedef char NSSUTF8;


typedef char NSSASCII7;

PR_END_EXTERN_C

#endif /* NSSBASET_H */
