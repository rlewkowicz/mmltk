/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsStringStream_h_
#define nsStringStream_h_

#include "nsIStringStream.h"
#include "nsString.h"
#include "nsTArray.h"

#define NS_STRINGINPUTSTREAM_CONTRACTID "@mozilla.org/io/string-input-stream;1"
#define NS_STRINGINPUTSTREAM_CID              \
  { \
   0x0abb0835,                                \
   0x5000,                                    \
   0x4790,                                    \
   {0xaf, 0x28, 0x61, 0xb3, 0xba, 0x17, 0xc2, 0x95}}

enum nsAssignmentType {
  NS_ASSIGNMENT_COPY,    
  NS_ASSIGNMENT_DEPEND,  
  NS_ASSIGNMENT_ADOPT    
};

extern nsresult NS_NewByteInputStream(nsIInputStream** aStreamResult,
                                      mozilla::Span<const char> aStringToRead,
                                      nsAssignmentType aAssignment);

extern nsresult NS_NewByteInputStream(nsIInputStream** aStreamResult,
                                      nsTArray<uint8_t>&& aArray);

extern nsresult NS_NewByteInputStream(nsIInputStream** aStreamResult,
                                      mozilla::StreamBufferSource* aSource);

extern nsresult NS_NewCStringInputStream(nsIInputStream** aStreamResult,
                                         const nsACString& aStringToRead);
extern nsresult NS_NewCStringInputStream(nsIInputStream** aStreamResult,
                                         nsCString&& aStringToRead);

#endif  // nsStringStream_h_
