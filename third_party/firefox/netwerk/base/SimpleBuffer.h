/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef SimpleBuffer_h_
#define SimpleBuffer_h_


#include "prtypes.h"
#include "ErrorList.h"
#include "mozilla/LinkedList.h"
#include "nsISupportsImpl.h"

namespace mozilla {
namespace net {

class SimpleBufferPage : public LinkedListElement<SimpleBufferPage> {
 public:
  SimpleBufferPage() = default;
  static const size_t kSimpleBufferPageSize = 32000;

 private:
  friend class SimpleBuffer;
  char mBuffer[kSimpleBufferPageSize]{0};
  size_t mReadOffset{0};
  size_t mWriteOffset{0};
};

class SimpleBuffer {
 public:
  SimpleBuffer() = default;
  ~SimpleBuffer() = default;

  nsresult Write(char* src, size_t len);   
  size_t Read(char* dest, size_t maxLen);  
  size_t Available();
  void Clear();

 private:
  NS_DECL_OWNINGTHREAD

  nsresult mStatus{NS_OK};
  AutoCleanLinkedList<SimpleBufferPage> mBufferList;
  size_t mAvailable{0};
};

}  
}  

#endif
