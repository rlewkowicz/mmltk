/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if defined(__cplusplus)
extern "C" {
#endif

#if !defined(EBMLWRITER_HPP)
#define EBMLWRITER_HPP
#include <stddef.h>
#include "vpx/vpx_integer.h"
#include "EbmlBufferWriter.h"




void Ebml_WriteLen(EbmlGlobal *glob, int64_t val);
void Ebml_WriteString(EbmlGlobal *glob, const char *str);
void Ebml_WriteUTF8(EbmlGlobal *glob, const wchar_t *wstr);
void Ebml_WriteID(EbmlGlobal *glob, unsigned long class_id);
void Ebml_SerializeUnsigned32(EbmlGlobal *glob, unsigned long class_id, uint32_t ui);
void Ebml_SerializeUnsigned64(EbmlGlobal *glob, unsigned long class_id, uint64_t ui);
void Ebml_SerializeUnsigned(EbmlGlobal *glob, unsigned long class_id, unsigned long ui);
void Ebml_SerializeBinary(EbmlGlobal *glob, unsigned long class_id, unsigned long ui);
void Ebml_SerializeFloat(EbmlGlobal *glob, unsigned long class_id, double d);
void Ebml_WriteSigned16(EbmlGlobal *glob, short val);
void Ebml_SerializeString(EbmlGlobal *glob, unsigned long class_id, const char *s);
void Ebml_SerializeUTF8(EbmlGlobal *glob, unsigned long class_id, wchar_t *s);
void Ebml_SerializeData(EbmlGlobal *glob, unsigned long class_id, unsigned char *data, unsigned long data_length);
void Ebml_WriteVoid(EbmlGlobal *glob, unsigned long vSize);
#endif

#if defined(__cplusplus)
}
#endif
