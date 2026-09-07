/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsPrimitiveHelpers_h_
#define nsPrimitiveHelpers_h_

#include "nsError.h"
#include "nscore.h"
#include "nsString.h"

class nsISupports;

class nsPrimitiveHelpers {
 public:
  static void CreatePrimitiveForData(const nsACString& aFlavor,
                                     const void* aDataBuff, size_t aDataLen,
                                     nsISupports** aPrimitive);

  static void CreatePrimitiveForCFHTML(const void* aDataBuff,
                                       uint32_t* aDataLen,
                                       nsISupports** aPrimitive);

  static void CreateDataFromPrimitive(const nsACString& aFlavor,
                                      nsISupports* aPrimitive, void** aDataBuff,
                                      uint32_t* aDataLen);

};  

class nsLinebreakHelpers {
 public:
  static nsresult ConvertPlatformToDOMLinebreaks(bool aIsSingleByteChars,
                                                 void** ioData,
                                                 int32_t* ioLengthInBytes);

};  

#endif  // nsPrimitiveHelpers_h_
