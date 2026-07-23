/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef prerror_h___
#define prerror_h___

#include "prtypes.h"

PR_BEGIN_EXTERN_C

typedef PRInt32 PRErrorCode;

#define PR_NSPR_ERROR_BASE -6000

#include "prerr.h"

NSPR_API(void) PR_SetError(PRErrorCode errorCode, PRInt32 oserr);

NSPR_API(void) PR_SetErrorText(
    PRIntn textLength, const char *text);

NSPR_API(PRErrorCode) PR_GetError(void);

NSPR_API(PRInt32) PR_GetOSError(void);

NSPR_API(PRInt32) PR_GetErrorTextLength(void);

NSPR_API(PRInt32) PR_GetErrorText(char *text);


/*
Copyright (C) 1987, 1988 Student Information Processing Board of the
Massachusetts Institute of Technology.

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted, provided
that the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation, and that the names of M.I.T. and the M.I.T. S.I.P.B. not be
used in advertising or publicity pertaining to distribution of the software
without specific, written prior permission.  M.I.T. and the M.I.T. S.I.P.B.
make no representations about the suitability of this software for any
purpose.  It is provided "as is" without express or implied warranty.
*/





typedef PRUint32 PRLanguageCode;
#define PR_LANGUAGE_I_DEFAULT 0 /* i-default, the default language */
#define PR_LANGUAGE_EN 1 /* English, explicitly negotiated */

struct PRErrorMessage {
    const char * name;    
    const char * en_text; 
};

struct PRErrorTable {
    const struct PRErrorMessage * msgs; 
    const char *name; 
    PRErrorCode base; 
    int n_msgs; 
};

struct PRErrorCallbackPrivate;

struct PRErrorCallbackTablePrivate;

typedef const char *
PRErrorCallbackLookupFn(PRErrorCode code, PRLanguageCode language,
                        const struct PRErrorTable *table,
                        struct PRErrorCallbackPrivate *cb_private,
                        struct PRErrorCallbackTablePrivate *table_private);

typedef struct PRErrorCallbackTablePrivate *
PRErrorCallbackNewTableFn(const struct PRErrorTable *table,
                          struct PRErrorCallbackPrivate *cb_private);


NSPR_API(const char *) PR_ErrorToString(PRErrorCode code,
                                        PRLanguageCode language);


NSPR_API(const char *) PR_ErrorToName(PRErrorCode code);


NSPR_API(const char * const *) PR_ErrorLanguages(void);


NSPR_API(PRErrorCode) PR_ErrorInstallTable(const struct PRErrorTable *table);


NSPR_API(void) PR_ErrorInstallCallback(const char * const * languages,
                                       PRErrorCallbackLookupFn *lookup,
                                       PRErrorCallbackNewTableFn *newtable,
                                       struct PRErrorCallbackPrivate *cb_private);

PR_END_EXTERN_C

#endif /* prerror_h___ */
