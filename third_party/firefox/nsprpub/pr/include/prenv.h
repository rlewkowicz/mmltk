/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef prenv_h___
#define prenv_h___

#include "prtypes.h"


PR_BEGIN_EXTERN_C

NSPR_API(char*) PR_GetEnv(const char *var);

NSPR_API(char*) PR_GetEnvSecure(const char *var);

NSPR_API(PRStatus) PR_SetEnv(const char *string);

NSPR_API(char **) PR_DuplicateEnvironment(void);

PR_END_EXTERN_C

#endif /* prenv_h___ */
