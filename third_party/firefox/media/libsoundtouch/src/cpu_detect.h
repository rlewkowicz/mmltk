/// Author        : Copyright (c) Olli Parviainen
// License :
//  Copyright (c) Olli Parviainen
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.
//  Lesser General Public License for more details.
//  License along with this library; if not, write to the Free Software

#ifndef _CPU_DETECT_H_
#define _CPU_DETECT_H_

#include "STTypes.h"

#define SUPPORT_MMX         0x0001
#define SUPPORT_3DNOW       0x0002
#define SUPPORT_ALTIVEC     0x0004
#define SUPPORT_SSE         0x0008
#define SUPPORT_SSE2        0x0010

uint detectCPUextensions(void);

void disableExtensions(uint wDisableMask);

#endif  // _CPU_DETECT_H_
