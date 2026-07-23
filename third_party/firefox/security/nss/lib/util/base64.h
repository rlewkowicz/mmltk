/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _BASE64_H_
#define _BASE64_H_

#include "utilrename.h"
#include "seccomon.h"

SEC_BEGIN_PROTOS

extern char *BTOA_DataToAscii(const unsigned char *data, unsigned int len);

extern unsigned char *ATOB_AsciiToData(const char *string, unsigned int *lenp);

extern SECStatus ATOB_ConvertAsciiToItem(SECItem *binary_item, const char *ascii);

extern char *BTOA_ConvertItemToAscii(SECItem *binary_item);

SEC_END_PROTOS

#endif /* _BASE64_H_ */
