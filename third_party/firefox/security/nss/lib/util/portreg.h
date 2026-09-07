/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef SHEXP_H
#define SHEXP_H

#include "utilrename.h"



#define NON_SXP -1
#define INVALID_SXP -2
#define VALID_SXP 1

SEC_BEGIN_PROTOS

extern int PORT_RegExpValid(const char *exp);

extern int PORT_RegExpSearch(const char *str, const char *exp);

extern int PORT_RegExpCaseSearch(const char *str, const char *exp);

SEC_END_PROTOS

#endif
