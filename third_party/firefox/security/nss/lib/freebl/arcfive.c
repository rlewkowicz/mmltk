/*
 * arcfive.c - stubs for RC5 - NOT a working implementation!
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef FREEBL_NO_DEPEND
#include "stubs.h"
#endif

#include "blapi.h"
#include "prerror.h"


RC5Context *
RC5_CreateContext(const SECItem *key, unsigned int rounds,
                  unsigned int wordSize, const unsigned char *iv, int mode)
{
    PORT_SetError(PR_NOT_IMPLEMENTED_ERROR);
    return NULL;
}

void
RC5_DestroyContext(RC5Context *cx, PRBool freeit)
{
    PORT_SetError(PR_NOT_IMPLEMENTED_ERROR);
}

SECStatus
RC5_Encrypt(RC5Context *cx, unsigned char *output, unsigned int *outputLen,
            unsigned int maxOutputLen,
            const unsigned char *input, unsigned int inputLen)
{
    PORT_SetError(PR_NOT_IMPLEMENTED_ERROR);
    return SECFailure;
}

SECStatus
RC5_Decrypt(RC5Context *cx, unsigned char *output, unsigned int *outputLen,
            unsigned int maxOutputLen,
            const unsigned char *input, unsigned int inputLen)
{
    PORT_SetError(PR_NOT_IMPLEMENTED_ERROR);
    return SECFailure;
}
