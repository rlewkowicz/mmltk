/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_DoubleToString_h
#define util_DoubleToString_h


struct DtoaState;

namespace js {

extern DtoaState* NewDtoaState();

extern void DestroyDtoaState(DtoaState* state);

}  

#define DTOSTR_STANDARD_BUFFER_SIZE 26

char* js_dtobasestr(DtoaState* state, int base, double d);

#endif /* util_DoubleToString_h */
