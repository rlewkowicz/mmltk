/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "mozilla/GeckoArgs.h"

namespace mozilla::geckoargs {

#if defined(XP_UNIX)
static int gInitialFileHandles[]{3,  4,  5,  6,  7,  8,  9, 10,
                                 11, 12, 13, 14, 15, 16, 17};

void SetPassedFileHandles(Span<int> aFiles) {
  MOZ_RELEASE_ASSERT(aFiles.Length() <= std::size(gInitialFileHandles));
  for (size_t i = 0; i < std::size(gInitialFileHandles); ++i) {
    if (i < aFiles.Length()) {
      gInitialFileHandles[i] = aFiles[i];
    } else {
      gInitialFileHandles[i] = -1;
    }
  }
}

void SetPassedFileHandles(std::vector<UniqueFileHandle>&& aFiles) {
  MOZ_RELEASE_ASSERT(aFiles.size() <= std::size(gInitialFileHandles));
  for (size_t i = 0; i < std::size(gInitialFileHandles); ++i) {
    if (i < aFiles.size()) {
      gInitialFileHandles[i] = aFiles[i].release();
    } else {
      gInitialFileHandles[i] = -1;
    }
  }
}

void AddToFdsToRemap(const ChildProcessArgs& aArgs,
                     std::vector<std::pair<int, int>>& aFdsToRemap) {
  MOZ_RELEASE_ASSERT(aArgs.mFiles.size() <= std::size(gInitialFileHandles));
  for (size_t i = 0; i < aArgs.mFiles.size(); ++i) {
    aFdsToRemap.push_back(
        std::pair{aArgs.mFiles[i].get(), gInitialFileHandles[i]});
  }
}
#endif


static void ParseHandleArgument(uint32_t aArg, UniqueFileHandle& aOutHandle) {
  MOZ_RELEASE_ASSERT(aArg < std::size(gInitialFileHandles));
  aOutHandle = UniqueFileHandle{std::exchange(gInitialFileHandles[aArg], -1)};
}

static Maybe<uint32_t> SerializeHandleArgument(UniqueFileHandle&& aValue,
                                               ChildProcessArgs& aArgs) {
  if (aValue) {
    uint32_t arg = static_cast<uint32_t>(aArgs.mFiles.size());
    aArgs.mFiles.push_back(std::move(aValue));
    return Some(arg);
  }
  return Nothing();
}

template <>
Maybe<UniqueFileHandle> CommandLineArg<UniqueFileHandle>::GetCommon(
    const char* aMatch, int& aArgc, char** aArgv, const CheckArgFlag aFlags) {
  if (Maybe<uint32_t> arg =
          CommandLineArg<uint32_t>::GetCommon(aMatch, aArgc, aArgv, aFlags)) {
    UniqueFileHandle h;
    ParseHandleArgument(*arg, h);
    return Some(std::move(h));
  }
  return Nothing();
}

template <>
void CommandLineArg<UniqueFileHandle>::PutCommon(const char* aName,
                                                 UniqueFileHandle aValue,
                                                 ChildProcessArgs& aArgs) {
  if (auto arg = SerializeHandleArgument(std::move(aValue), aArgs)) {
    CommandLineArg<uint32_t>::PutCommon(aName, *arg, aArgs);
  }
}


constexpr const char* kSharedMemoryHandleSeparator = ":";

template <>
Maybe<ipc::ReadOnlySharedMemoryHandle>
CommandLineArg<ipc::ReadOnlySharedMemoryHandle>::GetCommon(
    const char* aMatch, int& aArgc, char** aArgv, const CheckArgFlag aFlags) {
  auto arg =
      CommandLineArg<const char*>::GetCommon(aMatch, aArgc, aArgv, aFlags);
  if (!arg) {
    return Nothing();
  }

  std::string_view str = *arg;
  auto position = str.find(kSharedMemoryHandleSeparator);
  if (position == std::string_view::npos) {
    return Nothing();
  }

  auto handleId = ParseIntArgument(str.substr(0, position));
  auto size = ParseIntArgument(str.substr(position + 1));
  if (!handleId || !size) {
    return Nothing();
  }

  ipc::shared_memory::PlatformHandle handle;
  ParseHandleArgument(*handleId, handle);
  if (!handle) {
    return Nothing();
  }

  mozilla::ipc::ReadOnlySharedMemoryHandle rv;
  rv.mHandle = std::move(handle);
  rv.SetSize(*size);

  return Some(std::move(rv));
}

template <>
void CommandLineArg<ipc::ReadOnlySharedMemoryHandle>::PutCommon(
    const char* aName, ipc::ReadOnlySharedMemoryHandle aValue,
    ChildProcessArgs& aArgs) {
  if (!aValue) {
    return;
  }
  auto size = aValue.Size();
  auto handle = std::move(aValue).TakePlatformHandle();
  MOZ_ASSERT(handle, "shmem platform handle is invalid");

  auto handleId = SerializeHandleArgument(std::move(handle), aArgs);
  if (!handleId) {
    return;
  }

  auto arg = std::to_string(*handleId) + kSharedMemoryHandleSeparator +
             std::to_string(size);

  CommandLineArg<const char*>::PutCommon(aName, arg.c_str(), aArgs);
}

}  
