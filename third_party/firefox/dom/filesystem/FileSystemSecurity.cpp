/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FileSystemSecurity.h"

#include "FileSystemUtils.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/ipc/BackgroundParent.h"

namespace mozilla::dom {

namespace {

StaticRefPtr<FileSystemSecurity> gFileSystemSecurity;

constexpr char16_t kPosixPathSeparator = '/';
constexpr char16_t kPlatformPathSeparator = kPosixPathSeparator;

bool IsDescendantPath(const nsAString& aAuthorizedRoot,
                      const nsAString& aRequestedDescendant) {
  if (aRequestedDescendant.Equals(aAuthorizedRoot)) {
    return true;
  }

  if (!StringBeginsWith( aRequestedDescendant,
                         aAuthorizedRoot)) {
    return false;
  }

  const uint32_t prefixLen = aAuthorizedRoot.Length();
  if (prefixLen > 0 && aAuthorizedRoot.Last() == kPlatformPathSeparator) {
    return true;
  }

  if (aRequestedDescendant.Length() <= prefixLen ||
      aRequestedDescendant.CharAt(prefixLen) != kPlatformPathSeparator) {
    return false;
  }

  return true;
}

}  

already_AddRefed<FileSystemSecurity> FileSystemSecurity::Get() {
  MOZ_ASSERT(NS_IsMainThread());
  mozilla::ipc::AssertIsInMainProcess();

  RefPtr<FileSystemSecurity> service = gFileSystemSecurity.get();
  return service.forget();
}

already_AddRefed<FileSystemSecurity> FileSystemSecurity::GetOrCreate() {
  MOZ_ASSERT(NS_IsMainThread());
  mozilla::ipc::AssertIsInMainProcess();

  if (!gFileSystemSecurity) {
    gFileSystemSecurity = new FileSystemSecurity();
    ClearOnShutdown(&gFileSystemSecurity);
  }

  RefPtr<FileSystemSecurity> service = gFileSystemSecurity.get();
  return service.forget();
}

FileSystemSecurity::FileSystemSecurity() {
  MOZ_ASSERT(NS_IsMainThread());
  mozilla::ipc::AssertIsInMainProcess();
}

FileSystemSecurity::~FileSystemSecurity() {
  MOZ_ASSERT(NS_IsMainThread());
  mozilla::ipc::AssertIsInMainProcess();
}

void FileSystemSecurity::GrantAccessToContentProcess(
    ContentParentId aId, const nsAString& aDirectoryPath) {
  MOZ_ASSERT(NS_IsMainThread());
  mozilla::ipc::AssertIsInMainProcess();

  mPaths.WithEntryHandle(aId, [&](auto&& entry) {
    if (entry && entry.Data()->Contains(aDirectoryPath)) {
      return;
    }

    entry.OrInsertWith([] { return MakeUnique<nsTArray<nsString>>(); })
        ->AppendElement(aDirectoryPath);
  });
}

void FileSystemSecurity::Forget(ContentParentId aId) {
  MOZ_ASSERT(NS_IsMainThread());
  mozilla::ipc::AssertIsInMainProcess();

  mPaths.Remove(aId);
}

bool FileSystemSecurity::ContentProcessHasAccessTo(ContentParentId aId,
                                                   const nsAString& aPath) {
  MOZ_ASSERT(NS_IsMainThread());
  mozilla::ipc::AssertIsInMainProcess();

  if (StringBeginsWith(aPath, u"../"_ns) || FindInReadable(u"/../"_ns, aPath) ||
      StringEndsWith(aPath, u"/.."_ns) || aPath.EqualsLiteral("..")) {
    return false;
  }

  nsTArray<nsString>* paths;
  if (!mPaths.Get(aId, &paths)) {
    return false;
  }

  MOZ_DIAGNOSTIC_ASSERT(paths);
  for (const auto& authorizedRoot : *paths) {
    if (IsDescendantPath(authorizedRoot, aPath)) {
      return true;
    }
  }

  return false;
}

}  
