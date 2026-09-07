/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BASET_H
#define BASET_H


#ifndef NSSBASET_H
#include "nssbaset.h"
#endif /* NSSBASET_H */

#include "plhash.h"

PR_BEGIN_EXTERN_C


struct nssArenaMarkStr;
typedef struct nssArenaMarkStr nssArenaMark;

#ifdef DEBUG
#define ARENA_THREADMARK

#ifdef ARENA_THREADMARK
#define ARENA_DESTRUCTOR_LIST
#endif /* ARENA_THREADMARK */

#endif /* DEBUG */

typedef struct nssListStr nssList;
typedef struct nssListIteratorStr nssListIterator;
typedef PRBool (*nssListCompareFunc)(void *a, void *b);
typedef PRIntn (*nssListSortFunc)(void *a, void *b);
typedef void (*nssListElementDestructorFunc)(void *el);

typedef struct nssHashStr nssHash;
typedef void(PR_CALLBACK *nssHashIterator)(const void *key, void *value,
                                           void *arg);


#ifdef DEBUG
struct nssPointerTrackerStr {
    PRCallOnceType once;
    PRLock *lock;
    PLHashTable *table;
};
typedef struct nssPointerTrackerStr nssPointerTracker;
#endif /* DEBUG */


enum nssStringTypeEnum {
    nssStringType_DirectoryString,
    nssStringType_TeletexString, 
    nssStringType_PrintableString,
    nssStringType_UniversalString,
    nssStringType_BMPString,
    nssStringType_UTF8String,
    nssStringType_PHGString,
    nssStringType_GeneralString,

    nssStringType_Unknown = -1
};
typedef enum nssStringTypeEnum nssStringType;

PR_END_EXTERN_C

#endif /* BASET_H */
