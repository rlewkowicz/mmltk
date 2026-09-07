/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_FileDescriptorShuffle_h
#define mozilla_ipc_FileDescriptorShuffle_h

#include "mozilla/Span.h"
#include "nsTArray.h"

#include <utility>


namespace mozilla {
namespace ipc {

class FileDescriptorShuffle {
 public:
  FileDescriptorShuffle() = default;
  ~FileDescriptorShuffle();

  FileDescriptorShuffle(const FileDescriptorShuffle&) = delete;
  void operator=(const FileDescriptorShuffle&) = delete;

  using MappingRef = mozilla::Span<const std::pair<int, int>>;

  bool Init(MappingRef aMapping);

  MappingRef Dup2Sequence() const { return mMapping; }

  bool MapsTo(int aFd) const;

  void Forget() { mTempFds.Clear(); }

 private:
  nsTArray<std::pair<int, int>> mMapping;
  nsTArray<int> mTempFds;
  int mMaxDst;
};

}  
}  

#endif  // mozilla_ipc_FileDescriptorShuffle_h
