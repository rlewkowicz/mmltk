/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _CHACHA20_POLY1305_H_
#define _CHACHA20_POLY1305_H_ 1

struct ChaCha20Poly1305ContextStr {
    unsigned char key[32];
    unsigned char tagLen;
};

struct ChaCha20ContextStr {
    unsigned char key[32];
    unsigned char nonce[12];
    PRUint32 counter;
};

#endif /* _CHACHA20_POLY1305_H_ */
