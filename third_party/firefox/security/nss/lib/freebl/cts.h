/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CTS_H
#define CTS_H 1

#include "blapii.h"

typedef struct CTSContextStr CTSContext;

CTSContext *CTS_CreateContext(void *context, freeblCipherFunc cipher,
                              const unsigned char *iv);

void CTS_DestroyContext(CTSContext *cts, PRBool freeit);

SECStatus CTS_EncryptUpdate(CTSContext *cts, unsigned char *outbuf,
                            unsigned int *outlen, unsigned int maxout,
                            const unsigned char *inbuf, unsigned int inlen,
                            unsigned int blocksize);
SECStatus CTS_DecryptUpdate(CTSContext *cts, unsigned char *outbuf,
                            unsigned int *outlen, unsigned int maxout,
                            const unsigned char *inbuf, unsigned int inlen,
                            unsigned int blocksize);

#endif
