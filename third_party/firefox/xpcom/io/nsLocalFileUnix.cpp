/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "nsLocalFile.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Sprintf.h"
#include "mozilla/FilePreferences.h"
#include "mozilla/Base64.h"
#include "mozilla/dom/Promise.h"
#include "prtime.h"

#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>


#if defined(USE_LINUX_QUOTACTL)
#  include <sys/mount.h>
#  include <sys/quota.h>
#  include <sys/sysmacros.h>
#if !defined(BLOCK_SIZE)
#    define BLOCK_SIZE 1024 /* kernel block size */
#endif
#endif

#include "nsDirectoryServiceDefs.h"
#include "nsCOMPtr.h"
#include "nsIFile.h"
#include "nsString.h"
#include "nsIDirectoryEnumerator.h"
#include "nsSimpleEnumerator.h"
#include "private/pprio.h"
#include "prlink.h"

#if defined(MOZ_WIDGET_GTK)
#  include "nsIGIOService.h"
#if defined(MOZ_ENABLE_DBUS)
#    include "mozilla/widget/AsyncDBus.h"
#    include "mozilla/WidgetUtilsGtk.h"
#endif
#endif



#include "nsNativeCharsetUtils.h"
#include "nsTraceRefcnt.h"

#if defined(HAVE_SYS_STATVFS_H)
#if defined(__osf__) && defined(__DECCXX)
extern "C" int statvfs(const char*, struct statvfs*);
#endif
#  include <sys/statvfs.h>
#endif

#if defined(HAVE_SYS_STATFS_H)
#  include <sys/statfs.h>
#endif

#if defined(HAVE_SYS_VFS_H)
#  include <sys/vfs.h>
#endif

#if defined(HAVE_STATVFS64) && (!defined(LINUX) && !defined(__osf__))
#  define STATFS statvfs64
#  define F_BSIZE f_frsize
#elif defined(HAVE_STATVFS) && (!defined(LINUX) && !defined(__osf__))
#  define STATFS statvfs
#  define F_BSIZE f_frsize
#elif defined(HAVE_STATFS64)
#  define STATFS statfs64
#  define F_BSIZE f_bsize
#elif defined(HAVE_STATFS)
#  define STATFS statfs
#  define F_BSIZE f_bsize
#endif

using namespace mozilla;

#define CHECK_mPath()                                     \
  do {                                                    \
    if (mPath.IsEmpty()) return NS_ERROR_NOT_INITIALIZED; \
    if (!FilePreferences::IsAllowedPath(mPath))           \
      return NS_ERROR_FILE_ACCESS_DENIED;                 \
  } while (0)

#if defined(MOZ_ENABLE_DBUS) && defined(MOZ_WIDGET_GTK)
static const nsCString& GetDocumentStorePath() {
  static const nsDependentCString sDocumentStorePath = [] {
    nsCString storePath = nsPrintfCString("/run/user/%d/doc/", getuid());
    return nsDependentCString(ToNewCString(storePath), storePath.Length());
  }();
  return sDocumentStorePath;
}
#endif

static PRTime TimespecToMillis(const struct timespec& aTimeSpec) {
  return PRTime(aTimeSpec.tv_sec) * PR_MSEC_PER_SEC +
         PRTime(aTimeSpec.tv_nsec) / PR_NSEC_PER_MSEC;
}

class nsDirEnumeratorUnix final : public nsSimpleEnumerator,
                                  public nsIDirectoryEnumerator {
 public:
  static nsresult Create(nsLocalFile* aParent,
                         RefPtr<nsDirEnumeratorUnix>& aResult);

  NS_DECL_ISUPPORTS_INHERITED

  NS_DECL_NSISIMPLEENUMERATOR

  NS_DECL_NSIDIRECTORYENUMERATOR

  NS_FORWARD_NSISIMPLEENUMERATORBASE(nsSimpleEnumerator::)

  const nsID& DefaultInterface() override { return NS_GET_IID(nsIFile); }

 private:
  nsDirEnumeratorUnix() : mDir(nullptr), mEntry(nullptr) {}
  ~nsDirEnumeratorUnix() override;

  NS_IMETHOD GetNextEntry();

  DIR* mDir;
  struct dirent* mEntry;
  nsCString mParentPath;
};

nsDirEnumeratorUnix::~nsDirEnumeratorUnix() { Close(); }

NS_IMPL_ISUPPORTS_INHERITED(nsDirEnumeratorUnix, nsSimpleEnumerator,
                            nsIDirectoryEnumerator)

nsresult nsDirEnumeratorUnix::Create(nsLocalFile* aParent,
                                     RefPtr<nsDirEnumeratorUnix>& aResult) {
  RefPtr<nsDirEnumeratorUnix> self = new nsDirEnumeratorUnix();

  if (NS_FAILED(aParent->GetNativePath(self->mParentPath)) ||
      self->mParentPath.IsEmpty()) {
    return NS_ERROR_FILE_INVALID_PATH;
  }

  nsAutoCString dirPathWithSlash(self->mParentPath);
  dirPathWithSlash.Append('/');
  if (!FilePreferences::IsAllowedPath(dirPathWithSlash)) {
    return NS_ERROR_FILE_ACCESS_DENIED;
  }

  self->mDir = opendir(self->mParentPath.get());
  if (!self->mDir) {
    return NSRESULT_FOR_ERRNO();
  }

  nsresult rv = self->GetNextEntry();
  if (NS_FAILED(rv)) {
    return rv;
  }

  aResult = std::move(self);
  return NS_OK;
}

NS_IMETHODIMP
nsDirEnumeratorUnix::HasMoreElements(bool* aResult) {
  *aResult = mDir && mEntry;
  if (!*aResult) {
    Close();
  }
  return NS_OK;
}

NS_IMETHODIMP
nsDirEnumeratorUnix::GetNext(nsISupports** aResult) {
  nsCOMPtr<nsIFile> file;
  nsresult rv = GetNextFile(getter_AddRefs(file));
  if (NS_FAILED(rv)) {
    return rv;
  }
  if (!file) {
    return NS_ERROR_FAILURE;
  }
  file.forget(aResult);
  return NS_OK;
}

NS_IMETHODIMP
nsDirEnumeratorUnix::GetNextEntry() {
  do {
    errno = 0;
    mEntry = readdir(mDir);

    if (!mEntry) {
      return NSRESULT_FOR_ERRNO();
    }

  } while (mEntry->d_name[0] == '.' &&
           (mEntry->d_name[1] == '\0' ||  
            (mEntry->d_name[1] == '.' && mEntry->d_name[2] == '\0')));  
  return NS_OK;
}

NS_IMETHODIMP
nsDirEnumeratorUnix::GetNextFile(nsIFile** aResult) {
  nsresult rv;
  if (!mDir || !mEntry) {
    *aResult = nullptr;
    return NS_OK;
  }

  nsCOMPtr<nsIFile> file = new nsLocalFile();

  if (NS_FAILED(rv = file->InitWithNativePath(mParentPath)) ||
      NS_FAILED(rv = file->AppendNative(nsDependentCString(mEntry->d_name)))) {
    return rv;
  }

  file.forget(aResult);
  return GetNextEntry();
}

NS_IMETHODIMP
nsDirEnumeratorUnix::Close() {
  if (mDir) {
    closedir(mDir);
    mDir = nullptr;
  }
  return NS_OK;
}

nsLocalFile::nsLocalFile() = default;

nsLocalFile::nsLocalFile(const nsLocalFile& aOther) : mPath(aOther.mPath) {}

NS_IMPL_ISUPPORTS(nsLocalFile, nsIFile)

nsresult nsLocalFile::nsLocalFileConstructor(const nsIID& aIID,
                                             void** aInstancePtr) {
  if (NS_WARN_IF(!aInstancePtr)) {
    return NS_ERROR_INVALID_ARG;
  }

  *aInstancePtr = nullptr;

  nsCOMPtr<nsIFile> inst = new nsLocalFile();
  return inst->QueryInterface(aIID, aInstancePtr);
}

nsresult nsLocalFile::StatFile(struct STAT* statInfo) {
  if (!FilePreferences::IsAllowedPath(mPath)) {
    return NS_ERROR_FILE_ACCESS_DENIED;
  }

  if (STAT(mPath.get(), statInfo) == -1) {
    if (LSTAT(mPath.get(), statInfo) == -1) {
      return NSRESULT_FOR_ERRNO();
    }
  }

  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::Clone(nsIFile** aFile) {
  RefPtr<nsLocalFile> copy = new nsLocalFile(*this);
  copy.forget(aFile);
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::InitWithNativePath(const nsACString& aFilePath) {
  if (aFilePath.IsEmpty()) {
    return NS_ERROR_FILE_UNRECOGNIZED_PATH;
  }

  if (aFilePath.First() == '~') {
    if (aFilePath.Length() == 1 || aFilePath.CharAt(1) == '/') {

      nsCOMPtr<nsIFile> homeDir;
      nsAutoCString homePath;
      if (NS_FAILED(NS_GetSpecialDirectory(NS_OS_HOME_DIR,
                                           getter_AddRefs(homeDir))) ||
          NS_FAILED(homeDir->GetNativePath(homePath))) {
        return NS_ERROR_FAILURE;
      }

      mPath = homePath;
      if (aFilePath.Length() > 2) {
        mPath.Append(Substring(aFilePath, 1));
      }
    } else {

      mPath =
          "/home/"_ns
          + Substring(aFilePath, 1);
    }
  } else {
    if (aFilePath.First() != '/') {
      return NS_ERROR_FILE_UNRECOGNIZED_PATH;
    }
    mPath = aFilePath;
  }

  if (!FilePreferences::IsAllowedPath(mPath)) {
    mPath.Truncate();
    return NS_ERROR_FILE_ACCESS_DENIED;
  }

  ssize_t len = mPath.Length();
  while ((len > 1) && (mPath[len - 1] == '/')) {
    --len;
  }
  mPath.SetLength(len);

  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::CreateAllAncestors(uint32_t aPermissions) {
  if (!FilePreferences::IsAllowedPath(mPath)) {
    return NS_ERROR_FILE_ACCESS_DENIED;
  }

  char* buffer = mPath.BeginWriting();
  char* slashp = buffer;
  int mkdir_result = 0;
  int mkdir_errno;

#if defined(DEBUG_NSIFILE)
  fprintf(stderr, "nsIFile: before: %s\n", buffer);
#endif

  while ((slashp = strchr(slashp + 1, '/'))) {
    if (slashp[1] == '/') {
      continue;
    }

    if (slashp[1] == '\0') {
      break;
    }

    *slashp = '\0';
#if defined(DEBUG_NSIFILE)
    fprintf(stderr, "nsIFile: mkdir(\"%s\")\n", buffer);
#endif
    mkdir_result = mkdir(buffer, aPermissions);
    if (mkdir_result == -1) {
      mkdir_errno = errno;
      if (mkdir_errno != EEXIST && access(buffer, F_OK) == 0) {
        mkdir_errno = EEXIST;
      }
#if defined(DEBUG_NSIFILE)
      fprintf(stderr, "nsIFile: errno: %d\n", mkdir_errno);
#endif
    }

    *slashp = '/';
  }

  if (mkdir_result == -1 && mkdir_errno != EEXIST) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::OpenNSPRFileDesc(int32_t aFlags, int32_t aMode,
                              PRFileDesc** aResult) {
  if (!FilePreferences::IsAllowedPath(mPath)) {
    return NS_ERROR_FILE_ACCESS_DENIED;
  }
  *aResult = PR_Open(mPath.get(), aFlags, aMode);
  if (!*aResult) {
    return NS_ErrorAccordingToNSPR();
  }

  if (aFlags & DELETE_ON_CLOSE) {
    PR_Delete(mPath.get());
  }

#if defined(HAVE_POSIX_FADVISE)
  if (aFlags & OS_READAHEAD) {
    posix_fadvise(PR_FileDesc2NativeHandle(*aResult), 0, 0,
                  POSIX_FADV_SEQUENTIAL);
  }
#endif
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::OpenANSIFileDesc(const char* aMode, FILE** aResult) {
  if (!FilePreferences::IsAllowedPath(mPath)) {
    return NS_ERROR_FILE_ACCESS_DENIED;
  }
  *aResult = fopen(mPath.get(), aMode);
  if (!*aResult) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

static int do_create(const char* aPath, int aFlags, mode_t aMode,
                     PRFileDesc** aResult) {
  *aResult = PR_Open(aPath, aFlags, aMode);
  return *aResult ? 0 : -1;
}

static int do_mkdir(const char* aPath, int aFlags, mode_t aMode,
                    PRFileDesc** aResult) {
  *aResult = nullptr;
  return mkdir(aPath, aMode);
}

nsresult nsLocalFile::CreateAndKeepOpen(uint32_t aType, int aFlags,
                                        uint32_t aPermissions,
                                        bool aSkipAncestors,
                                        PRFileDesc** aResult) {
  if (!FilePreferences::IsAllowedPath(mPath)) {
    return NS_ERROR_FILE_ACCESS_DENIED;
  }

  if (aType != NORMAL_FILE_TYPE && aType != DIRECTORY_TYPE) {
    return NS_ERROR_FILE_UNKNOWN_TYPE;
  }

  int (*createFunc)(const char*, int, mode_t, PRFileDesc**) =
      (aType == NORMAL_FILE_TYPE) ? do_create : do_mkdir;

  int result = createFunc(mPath.get(), aFlags, aPermissions, aResult);
  if (result == -1 && errno == ENOENT && !aSkipAncestors) {
    int dirperm = aPermissions;
    if (aPermissions & S_IRUSR) {
      dirperm |= S_IXUSR;
    }
    if (aPermissions & S_IRGRP) {
      dirperm |= S_IXGRP;
    }
    if (aPermissions & S_IROTH) {
      dirperm |= S_IXOTH;
    }

#if defined(DEBUG_NSIFILE)
    fprintf(stderr, "nsIFile: perm = %o, dirperm = %o\n", aPermissions,
            dirperm);
#endif

    if (NS_FAILED(CreateAllAncestors(dirperm))) {
      return NS_ERROR_FAILURE;
    }

#if defined(DEBUG_NSIFILE)
    fprintf(stderr, "nsIFile: Create(\"%s\") again\n", mPath.get());
#endif
    result = createFunc(mPath.get(), aFlags, aPermissions, aResult);
  }
  return NSRESULT_FOR_RETURN(result);
}

NS_IMETHODIMP
nsLocalFile::Create(uint32_t aType, uint32_t aPermissions,
                    bool aSkipAncestors) {
  if (!FilePreferences::IsAllowedPath(mPath)) {
    return NS_ERROR_FILE_ACCESS_DENIED;
  }

  PRFileDesc* junk = nullptr;
  nsresult rv = CreateAndKeepOpen(
      aType, PR_WRONLY | PR_CREATE_FILE | PR_TRUNCATE | PR_EXCL, aPermissions,
      aSkipAncestors, &junk);
  if (junk) {
    PR_Close(junk);
  }
  return rv;
}

NS_IMETHODIMP
nsLocalFile::AppendNative(const nsACString& aFragment) {
  if (aFragment.IsEmpty()) {
    return NS_OK;
  }

  nsACString::const_iterator begin, end;
  if (aFragment.EqualsASCII("..") ||
      FindCharInReadable('/', aFragment.BeginReading(begin),
                         aFragment.EndReading(end))) {
    return NS_ERROR_FILE_UNRECOGNIZED_PATH;
  }

  return AppendRelativeNativePath(aFragment);
}

NS_IMETHODIMP
nsLocalFile::AppendRelativeNativePath(const nsACString& aFragment) {
  if (aFragment.IsEmpty()) {
    return NS_OK;
  }

  if (aFragment.First() == '/' || aFragment.EqualsASCII("..")) {
    return NS_ERROR_FILE_UNRECOGNIZED_PATH;
  }

  if (aFragment.Contains('/')) {
    constexpr auto doubleDot = "/.."_ns;
    nsACString::const_iterator start, end, offset;
    aFragment.BeginReading(start);
    aFragment.EndReading(end);
    offset = end;
    while (FindInReadable(doubleDot, start, offset)) {
      if (offset == end || *offset == '/') {
        return NS_ERROR_FILE_UNRECOGNIZED_PATH;
      }
      start = offset;
      offset = end;
    }

    if (StringBeginsWith(aFragment, "../"_ns)) {
      return NS_ERROR_FILE_UNRECOGNIZED_PATH;
    }
  }

  if (!mPath.EqualsLiteral("/")) {
    mPath.Append('/');
  }
  mPath.Append(aFragment);

  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::Normalize() {
  char resolved_path[PATH_MAX] = "";
  char* resolved_path_ptr = nullptr;

  if (!FilePreferences::IsAllowedPath(mPath)) {
    return NS_ERROR_FILE_ACCESS_DENIED;
  }

  resolved_path_ptr = realpath(mPath.get(), resolved_path);

  if (!resolved_path_ptr) {
    return NSRESULT_FOR_ERRNO();
  }

  mPath = resolved_path;
  return NS_OK;
}

void nsLocalFile::LocateNativeLeafName(nsACString::const_iterator& aBegin,
                                       nsACString::const_iterator& aEnd) {
  mPath.BeginReading(aBegin);
  mPath.EndReading(aEnd);

  nsACString::const_iterator it = aEnd;
  nsACString::const_iterator stop = aBegin;
  --stop;
  while (--it != stop) {
    if (*it == '/') {
      aBegin = ++it;
      return;
    }
  }

  MOZ_ASSERT_UNREACHABLE("nsLocalFile path should be absolute but is not!");
}

NS_IMETHODIMP
nsLocalFile::GetNativeLeafName(nsACString& aLeafName) {
  nsACString::const_iterator begin, end;
  LocateNativeLeafName(begin, end);
  aLeafName = Substring(begin, end);
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::SetNativeLeafName(const nsACString& aLeafName) {
  nsACString::const_iterator begin, end;
  LocateNativeLeafName(begin, end);
  mPath.Replace(begin.get() - mPath.get(), Distance(begin, end), aLeafName);
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetDisplayName(nsAString& aLeafName) {
  return GetLeafName(aLeafName);
}

NS_IMETHODIMP
nsLocalFile::HostPath(JSContext* aCx, dom::Promise** aPromise) {
  MOZ_ASSERT(aCx);
  MOZ_ASSERT(aPromise);

  nsIGlobalObject* globalObject = xpc::CurrentNativeGlobal(aCx);
  if (NS_WARN_IF(!globalObject)) {
    return NS_ERROR_FAILURE;
  }

  ErrorResult result;
  RefPtr<dom::Promise> retPromise = dom::Promise::Create(globalObject, result);
  if (NS_WARN_IF(result.Failed())) {
    return result.StealNSResult();
  }

#if defined(MOZ_ENABLE_DBUS) && defined(MOZ_WIDGET_GTK)
  if (!widget::IsRunningUnderFlatpak() ||
      !StringBeginsWith(mPath, GetDocumentStorePath())) {
    retPromise->MaybeResolve(mPath);
    retPromise.forget(aPromise);
    return NS_OK;
  }

  nsCString docId = [this] {
    auto subPath = Substring(mPath, GetDocumentStorePath().Length());
    if (auto idx = subPath.Find("/"); idx > 0) {
      subPath.Truncate(idx);
    }
    return nsCString(subPath);
  }();

  const char kServiceName[] = "org.freedesktop.portal.Documents";
  const char kDBusPath[] = "/org/freedesktop/portal/documents";
  const char kInterfaceName[] = "org.freedesktop.portal.Documents";

  widget::CreateDBusProxyForBus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE,
                                 nullptr, kServiceName,
                                kDBusPath, kInterfaceName)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [this, self = RefPtr(this), docId,
           retPromise](RefPtr<GDBusProxy>&& aProxy) {
            RefPtr<GVariant> version = dont_AddRef(
                g_dbus_proxy_get_cached_property(aProxy, "version"));
            if (!version ||
                !g_variant_is_of_type(version, G_VARIANT_TYPE_UINT32)) {
              g_printerr(
                  "nsIFile: failed to get host path for %s: Invalid value.\n",
                  mPath.get());
              retPromise->MaybeReject(NS_ERROR_FAILURE);
              return;
            }

            if (g_variant_get_uint32(version) < 5) {
              g_printerr(
                  "nsIFile: failed to get host path for %s: Document "
                  "portal in version 5 is required.\n",
                  mPath.get());
              retPromise->MaybeReject(NS_ERROR_NOT_AVAILABLE);
              return;
            }

            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("(as)"));
            g_variant_builder_open(&builder, G_VARIANT_TYPE("as"));
            g_variant_builder_add(&builder, "s", docId.get());
            g_variant_builder_close(&builder);

            RefPtr<GVariant> args = dont_AddRef(
                g_variant_ref_sink(g_variant_builder_end(&builder)));

            if (!args) {
              g_printerr(
                  "nsIFile: failed to get host path for %s: "
                  "Invalid value.\n",
                  mPath.get());
              retPromise->MaybeReject(NS_ERROR_FAILURE);
              return;
            }

            widget::DBusProxyCall(aProxy, "GetHostPaths", args,
                                  G_DBUS_CALL_FLAGS_NONE, -1,
                                   nullptr)
                ->Then(
                    GetCurrentSerialEventTarget(), __func__,
                    [this, self = RefPtr(this), docId,
                     retPromise](RefPtr<GVariant>&& aResult) {
                      RefPtr<GVariant> result = dont_AddRef(
                          g_variant_get_child_value(aResult.get(), 0));
                      if (!g_variant_is_of_type(result,
                                                G_VARIANT_TYPE("a{say}"))) {
                        g_printerr(
                            "nsIFile: failed to get host path for %s: "
                            "Invalid value.\n",
                            mPath.get());
                        retPromise->MaybeReject(NS_ERROR_FAILURE);
                        return;
                      }

                      const gchar* key = nullptr;
                      const gchar* path = nullptr;
                      GVariantIter* iter = g_variant_iter_new(result);

                      while (
                          g_variant_iter_loop(iter, "{&s^&ay}", &key, &path)) {
                        if (g_strcmp0(key, docId.get()) == 0) {
                          retPromise->MaybeResolve(nsDependentCString(path));
                          g_variant_iter_free(iter);
                          return;
                        }
                      }

                      g_variant_iter_free(iter);
                      g_printerr(
                          "nsIFile: failed to get host path for %s: "
                          "Invalid value.\n",
                          mPath.get());
                      retPromise->MaybeReject(NS_ERROR_FAILURE);
                    },
                    [this, self = RefPtr(this),
                     retPromise](GUniquePtr<GError>&& aError) {
                      g_printerr(
                          "nsIFile: failed to get host path for %s: %s.\n",
                          mPath.get(), aError->message);
                      retPromise->MaybeReject(NS_ERROR_FAILURE);
                    });
          },
          [this, self = RefPtr(this), retPromise](GUniquePtr<GError>&& aError) {
            g_printerr("nsIFile: failed to get host path for %s: %s.\n",
                       mPath.get(), aError->message);
            retPromise->MaybeReject(NS_ERROR_NOT_AVAILABLE);
          });
#else
  retPromise->MaybeResolve(mPath);
#endif
  retPromise.forget(aPromise);
  return NS_OK;
}

nsCString nsLocalFile::NativePath() { return mPath; }

nsresult nsIFile::GetNativePath(nsACString& aResult) {
  aResult = NativePath();
  return NS_OK;
}

nsCString nsIFile::HumanReadablePath() {
  nsCString path;
  DebugOnly<nsresult> rv = GetNativePath(path);
  MOZ_ASSERT(NS_SUCCEEDED(rv));
  return path;
}

nsresult nsLocalFile::GetNativeTargetPathName(nsIFile* aNewParent,
                                              const nsACString& aNewName,
                                              nsACString& aResult) {
  nsresult rv;
  nsCOMPtr<nsIFile> oldParent;

  if (!aNewParent) {
    if (NS_FAILED(rv = GetParent(getter_AddRefs(oldParent)))) {
      return rv;
    }
    aNewParent = oldParent.get();
  } else {
    bool targetExists;
    if (NS_FAILED(rv = aNewParent->Exists(&targetExists))) {
      return rv;
    }

    if (!targetExists) {
      rv = aNewParent->Create(DIRECTORY_TYPE, 0755);
      if (NS_FAILED(rv)) {
        return rv;
      }
    } else {
      bool targetIsDirectory;
      if (NS_FAILED(rv = aNewParent->IsDirectory(&targetIsDirectory))) {
        return rv;
      }
      if (!targetIsDirectory) {
        return NS_ERROR_FILE_DESTINATION_NOT_DIR;
      }
    }
  }

  nsACString::const_iterator nameBegin, nameEnd;
  if (!aNewName.IsEmpty()) {
    aNewName.BeginReading(nameBegin);
    aNewName.EndReading(nameEnd);
  } else {
    LocateNativeLeafName(nameBegin, nameEnd);
  }

  nsAutoCString dirName;
  if (NS_FAILED(rv = aNewParent->GetNativePath(dirName))) {
    return rv;
  }

  aResult = dirName + "/"_ns + Substring(nameBegin, nameEnd);
  return NS_OK;
}

nsresult nsLocalFile::CopyDirectoryTo(nsIFile* aNewParent) {
  nsresult rv;
  bool dirCheck;  
  bool isSymlink;
  uint32_t oldPerms;

  if (NS_FAILED(rv = IsDirectory(&dirCheck))) {
    return rv;
  }
  if (!dirCheck) {
    return CopyToNative(aNewParent, ""_ns);
  }

  if (NS_FAILED(rv = Equals(aNewParent, &dirCheck))) {
    return rv;
  }
  if (dirCheck) {
    return NS_ERROR_INVALID_ARG;
  }

  if (NS_FAILED(rv = aNewParent->Exists(&dirCheck))) {
    return rv;
  }
  if (NS_FAILED(rv = GetPermissions(&oldPerms))) {
    return rv;
  }
  if (!dirCheck) {
    if (NS_FAILED(rv = aNewParent->Create(DIRECTORY_TYPE, oldPerms))) {
      return rv;
    }
  } else {  
    nsAutoCString leafName;
    if (NS_FAILED(rv = GetNativeLeafName(leafName))) {
      return rv;
    }
    if (NS_FAILED(rv = aNewParent->AppendNative(leafName))) {
      return rv;
    }
    if (NS_FAILED(rv = aNewParent->Exists(&dirCheck))) {
      return rv;
    }
    if (dirCheck) {
      return NS_ERROR_FILE_ALREADY_EXISTS;  
    }
    if (NS_FAILED(rv = aNewParent->Create(DIRECTORY_TYPE, oldPerms))) {
      return rv;
    }
  }

  nsCOMPtr<nsIDirectoryEnumerator> dirIterator;
  if (NS_FAILED(rv = GetDirectoryEntries(getter_AddRefs(dirIterator)))) {
    return rv;
  }

  nsCOMPtr<nsIFile> entry;
  while (NS_SUCCEEDED(dirIterator->GetNextFile(getter_AddRefs(entry))) &&
         entry) {
    if (NS_FAILED(rv = entry->IsSymlink(&isSymlink))) {
      return rv;
    }
    if (NS_FAILED(rv = entry->IsDirectory(&dirCheck))) {
      return rv;
    }
    if (dirCheck && !isSymlink) {
      nsCOMPtr<nsIFile> destClone;
      rv = aNewParent->Clone(getter_AddRefs(destClone));
      if (NS_SUCCEEDED(rv)) {
        if (NS_FAILED(rv = entry->CopyToNative(destClone, ""_ns))) {
#if defined(DEBUG)
          nsresult rv2;
          nsAutoCString pathName;
          if (NS_FAILED(rv2 = entry->GetNativePath(pathName))) {
            return rv2;
          }
          printf("Operation not supported: %s\n", pathName.get());
#endif
          if (rv == NS_ERROR_OUT_OF_MEMORY) {
            return rv;
          }
          continue;
        }
      }
    } else {
      if (NS_FAILED(rv = entry->CopyToNative(aNewParent, ""_ns))) {
#if defined(DEBUG)
        nsresult rv2;
        nsAutoCString pathName;
        if (NS_FAILED(rv2 = entry->GetNativePath(pathName))) {
          return rv2;
        }
        printf("Operation not supported: %s\n", pathName.get());
#endif
        if (rv == NS_ERROR_OUT_OF_MEMORY) {
          return rv;
        }
        continue;
      }
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::CopyToNative(nsIFile* aNewParent, const nsACString& aNewName) {
  nsresult rv;
  CHECK_mPath();

  nsCOMPtr<nsIFile> workParent;
  if (aNewParent) {
    if (NS_FAILED(rv = aNewParent->Clone(getter_AddRefs(workParent)))) {
      return rv;
    }
  } else {
    if (NS_FAILED(rv = GetParent(getter_AddRefs(workParent)))) {
      return rv;
    }
  }

  bool isDirectory;
  if (NS_FAILED(rv = IsDirectory(&isDirectory))) {
    return rv;
  }

  nsAutoCString newPathName;
  if (isDirectory) {
    if (!aNewName.IsEmpty()) {
      if (NS_FAILED(rv = workParent->AppendNative(aNewName))) {
        return rv;
      }
    } else {
      if (NS_FAILED(rv = GetNativeLeafName(newPathName))) {
        return rv;
      }
      if (NS_FAILED(rv = workParent->AppendNative(newPathName))) {
        return rv;
      }
    }
    if (NS_FAILED(rv = CopyDirectoryTo(workParent))) {
      return rv;
    }
  } else {
    rv = GetNativeTargetPathName(workParent, aNewName, newPathName);
    if (NS_FAILED(rv)) {
      return rv;
    }

#if defined(DEBUG_blizzard)
    printf("nsLocalFile::CopyTo() %s -> %s\n", mPath.get(), newPathName.get());
#endif

    auto* newFile = new nsLocalFile();
    nsCOMPtr<nsIFile> fileRef(newFile);  

    rv = newFile->InitWithNativePath(newPathName);
    if (NS_FAILED(rv)) {
      return rv;
    }

    uint32_t myPerms = 0;
    rv = GetPermissions(&myPerms);
    if (NS_FAILED(rv)) {
      return rv;
    }


    PRFileDesc* newFD;
    rv = newFile->CreateAndKeepOpen(
        NORMAL_FILE_TYPE, PR_WRONLY | PR_CREATE_FILE | PR_TRUNCATE, myPerms,
         false, &newFD);
    if (NS_FAILED(rv)) {
      return rv;
    }

    bool specialFile;
    if (NS_FAILED(rv = IsSpecial(&specialFile))) {
      PR_Close(newFD);
      return rv;
    }
    if (specialFile) {
#if defined(DEBUG)
      printf("Operation not supported: %s\n", mPath.get());
#endif
      PR_Close(newFD);
      return NS_OK;
    }


    PRFileDesc* oldFD;
    rv = OpenNSPRFileDesc(PR_RDONLY, myPerms, &oldFD);
    if (NS_FAILED(rv)) {
      PR_Close(newFD);
      return rv;
    }

#if defined(DEBUG_blizzard)
    int32_t totalRead = 0;
    int32_t totalWritten = 0;
#endif
    char buf[BUFSIZ];
    int32_t bytesRead;

    nsresult saved_write_error = NS_OK;
    nsresult saved_read_error = NS_OK;
    nsresult saved_read_close_error = NS_OK;
    nsresult saved_write_close_error = NS_OK;


    while ((bytesRead = PR_Read(oldFD, buf, BUFSIZ)) > 0) {
#if defined(DEBUG_blizzard)
      totalRead += bytesRead;
#endif

      int32_t bytesWritten = PR_Write(newFD, buf, bytesRead);
      if (bytesWritten < 0) {
        saved_write_error = NSRESULT_FOR_ERRNO();
        bytesRead = -1;
        break;
      }
      NS_ASSERTION(bytesWritten == bytesRead, "short PR_Write?");

#if defined(DEBUG_blizzard)
      totalWritten += bytesWritten;
#endif
    }


    if (bytesRead < 0 && saved_write_error == NS_OK) {
      saved_read_error = NSRESULT_FOR_ERRNO();
    }

#if defined(DEBUG_blizzard)
    printf("read %d bytes, wrote %d bytes\n", totalRead, totalWritten);
#endif


    if (PR_Close(newFD) < 0) {
      saved_write_close_error = NSRESULT_FOR_ERRNO();
#if DEBUG
      fprintf(stderr, "ERROR: PR_Close(newFD) returned error. errno = %d\n",
              errno);
#endif
    }

    if (PR_Close(oldFD) < 0) {
      saved_read_close_error = NSRESULT_FOR_ERRNO();
#if DEBUG
      fprintf(stderr, "ERROR: PR_Close(oldFD) returned error. errno = %d\n",
              errno);
#endif
    }

    if (bytesRead < 0) {
      if (saved_write_error != NS_OK) {
        return saved_write_error;
      }
      if (saved_read_error != NS_OK) {
        return saved_read_error;
      }
#if DEBUG
      MOZ_ASSERT(0);
#endif
    }

    if (saved_write_close_error != NS_OK) {
      return saved_write_close_error;
    }
    if (saved_read_close_error != NS_OK) {
      return saved_read_close_error;
    }
  }
  return rv;
}

NS_IMETHODIMP
nsLocalFile::CopyToFollowingLinksNative(nsIFile* aNewParent,
                                        const nsACString& aNewName) {
  return CopyToNative(aNewParent, aNewName);
}

NS_IMETHODIMP
nsLocalFile::MoveToNative(nsIFile* aNewParent, const nsACString& aNewName) {
  nsresult rv;

  CHECK_mPath();

  nsAutoCString newPathName;
  rv = GetNativeTargetPathName(aNewParent, aNewName, newPathName);
  if (NS_FAILED(rv)) {
    return rv;
  }

  if (!FilePreferences::IsAllowedPath(newPathName)) {
    return NS_ERROR_FILE_ACCESS_DENIED;
  }

  if (rename(mPath.get(), newPathName.get()) < 0) {
    if (errno == EXDEV) {
      rv = CopyToNative(aNewParent, aNewName);
      if (NS_SUCCEEDED(rv)) {
        rv = Remove(true);
      }
    } else {
      rv = NSRESULT_FOR_ERRNO();
    }
  }

  if (NS_SUCCEEDED(rv)) {
    mPath = newPathName;
  }
  return rv;
}

NS_IMETHODIMP
nsLocalFile::MoveToFollowingLinksNative(nsIFile* aNewParent,
                                        const nsACString& aNewName) {
  return MoveToNative(aNewParent, aNewName);
}

NS_IMETHODIMP
nsLocalFile::Remove(bool aRecursive, uint32_t* aRemoveCount) {
  CHECK_mPath();

  bool isLink = false;
  nsresult rv = IsSymlink(&isLink);
  if (NS_FAILED(rv)) {
    return rv;
  }

  bool isDir = false;
  if (!isLink) {
    rv = IsDirectory(&isDir);
    if (NS_FAILED(rv)) {
      return rv;
    }
  }

  if (isDir) {
    if (aRecursive) {
      RefPtr<nsDirEnumeratorUnix> dirEnum;
      rv = nsDirEnumeratorUnix::Create(this, dirEnum);
      if (NS_FAILED(rv)) {
        return rv;
      }

      nsCOMPtr<nsIFile> file;
      while (NS_SUCCEEDED(dirEnum->GetNextFile(getter_AddRefs(file))) && file) {
        file->Remove(aRecursive, aRemoveCount);
      }
    }

    rv = NSRESULT_FOR_RETURN(rmdir(mPath.get()));
    if (NS_FAILED(rv)) {
      return rv;
    }
  } else {
    rv = NSRESULT_FOR_RETURN(unlink(mPath.get()));
    if (NS_FAILED(rv)) {
      return rv;
    }
  }

  if (aRemoveCount) {
    *aRemoveCount += 1;
  }

  return rv;
}

nsresult nsLocalFile::GetTimeImpl(PRTime* aTime,
                                  nsLocalFile::TimeField aTimeField,
                                  bool aFollowLinks) {
  CHECK_mPath();
  if (NS_WARN_IF(!aTime)) {
    return NS_ERROR_INVALID_ARG;
  }

  using StatFn = int (*)(const char*, struct STAT*);
  StatFn statFn = aFollowLinks ? &STAT : &LSTAT;

  struct STAT fileStats {};
  if (statFn(mPath.get(), &fileStats) < 0) {
    return NSRESULT_FOR_ERRNO();
  }

  struct timespec* timespec;
  switch (aTimeField) {
    case TimeField::AccessedTime:
      timespec = &fileStats.st_atim;
      break;

    case TimeField::ModifiedTime:
      timespec = &fileStats.st_mtim;
      break;

    default:
      MOZ_CRASH("Unknown TimeField");
  }

  *aTime = TimespecToMillis(*timespec);

  return NS_OK;
}

nsresult nsLocalFile::SetTimeImpl(PRTime aTime,
                                  nsLocalFile::TimeField aTimeField,
                                  bool aFollowLinks) {
  CHECK_mPath();

  using UtimesFn = int (*)(const char*, const timeval*);
  UtimesFn utimesFn = &utimes;

#if HAVE_LUTIMES
  if (!aFollowLinks) {
    utimesFn = &lutimes;
  }
#endif

  if (aTime == 0) {
    aTime = PR_Now();
  }


  timeval times[2];

  const size_t writeIndex = aTimeField == TimeField::AccessedTime ? 0 : 1;
  const size_t copyIndex = aTimeField == TimeField::AccessedTime ? 1 : 0;

  struct STAT statInfo {};
  nsresult rv = StatFile(&statInfo);
  if (NS_FAILED(rv)) {
    return rv;
  }

  auto* copyFrom = aTimeField == TimeField::AccessedTime ? &statInfo.st_mtim
                                                         : &statInfo.st_atim;

  times[copyIndex].tv_sec = copyFrom->tv_sec;
  times[copyIndex].tv_usec = copyFrom->tv_nsec / 1000;

  times[writeIndex].tv_sec = aTime / PR_MSEC_PER_SEC;
  times[writeIndex].tv_usec = (aTime % PR_MSEC_PER_SEC) * PR_USEC_PER_MSEC;

  int result = utimesFn(mPath.get(), times);
  return NSRESULT_FOR_RETURN(result);
}

NS_IMETHODIMP
nsLocalFile::GetLastAccessedTime(PRTime* aLastAccessedTime) {
  return GetTimeImpl(aLastAccessedTime, TimeField::AccessedTime,
                      true);
}

NS_IMETHODIMP
nsLocalFile::SetLastAccessedTime(PRTime aLastAccessedTime) {
  return SetTimeImpl(aLastAccessedTime, TimeField::AccessedTime,
                      true);
}

NS_IMETHODIMP
nsLocalFile::GetLastAccessedTimeOfLink(PRTime* aLastAccessedTime) {
  return GetTimeImpl(aLastAccessedTime, TimeField::AccessedTime,
                      false);
}

NS_IMETHODIMP
nsLocalFile::SetLastAccessedTimeOfLink(PRTime aLastAccessedTime) {
  return SetTimeImpl(aLastAccessedTime, TimeField::AccessedTime,
                      false);
}

NS_IMETHODIMP
nsLocalFile::GetLastModifiedTime(PRTime* aLastModTime) {
  return GetTimeImpl(aLastModTime, TimeField::ModifiedTime,
                      true);
}

NS_IMETHODIMP
nsLocalFile::SetLastModifiedTime(PRTime aLastModTime) {
  return SetTimeImpl(aLastModTime, TimeField::ModifiedTime,
                      true);
}

NS_IMETHODIMP
nsLocalFile::GetLastModifiedTimeOfLink(PRTime* aLastModTimeOfLink) {
  return GetTimeImpl(aLastModTimeOfLink, TimeField::ModifiedTime,
                      false);
}

NS_IMETHODIMP
nsLocalFile::SetLastModifiedTimeOfLink(PRTime aLastModTimeOfLink) {
  return SetTimeImpl(aLastModTimeOfLink, TimeField::ModifiedTime,
                      false);
}

NS_IMETHODIMP
nsLocalFile::GetCreationTime(PRTime* aCreationTime) {
  return GetCreationTimeImpl(aCreationTime, true);
}

NS_IMETHODIMP
nsLocalFile::GetCreationTimeOfLink(PRTime* aCreationTimeOfLink) {
  return GetCreationTimeImpl(aCreationTimeOfLink, false);
}

nsresult nsLocalFile::GetCreationTimeImpl(PRTime* aCreationTime,
                                          bool aFollowLinks) {
  CHECK_mPath();
  if (NS_WARN_IF(!aCreationTime)) {
    return NS_ERROR_INVALID_ARG;
  }

#if defined(_DARWIN_FEATURE_64_BIT_INODE)
  using StatFn = int (*)(const char*, struct STAT*);
  StatFn statFn = aFollowLinks ? &STAT : &LSTAT;

  struct STAT fileStats {};
  if (statFn(mPath.get(), &fileStats) < 0) {
    return NSRESULT_FOR_ERRNO();
  }

  *aCreationTime = TimespecToMillis(fileStats.st_birthtimespec);
  return NS_OK;
#else
  return NS_ERROR_NOT_IMPLEMENTED;
#endif
}


#define NORMALIZE_PERMS(mode) ((mode) & (S_IRWXU | S_IRWXG | S_IRWXO))

NS_IMETHODIMP
nsLocalFile::GetPermissions(uint32_t* aPermissions) {
  if (NS_WARN_IF(!aPermissions)) {
    return NS_ERROR_INVALID_ARG;
  }

  struct STAT statInfo {};
  nsresult rv = StatFile(&statInfo);
  if (NS_FAILED(rv)) {
    return rv;
  }

  *aPermissions = NORMALIZE_PERMS(statInfo.st_mode);
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetPermissionsOfLink(uint32_t* aPermissionsOfLink) {
  CHECK_mPath();
  if (NS_WARN_IF(!aPermissionsOfLink)) {
    return NS_ERROR_INVALID_ARG;
  }

  struct STAT sbuf;
  if (LSTAT(mPath.get(), &sbuf) == -1) {
    return NSRESULT_FOR_ERRNO();
  }
  *aPermissionsOfLink = NORMALIZE_PERMS(sbuf.st_mode);
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::SetPermissions(uint32_t aPermissions) {
  CHECK_mPath();

  if (chmod(mPath.get(), aPermissions) >= 0) {
    return NS_OK;
  }
  return NSRESULT_FOR_ERRNO();
}

NS_IMETHODIMP
nsLocalFile::SetPermissionsOfLink(uint32_t aPermissions) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsLocalFile::GetFileSize(int64_t* aFileSize) {
  if (NS_WARN_IF(!aFileSize)) {
    return NS_ERROR_INVALID_ARG;
  }
  *aFileSize = 0;

  struct STAT statInfo {};
  nsresult rv = StatFile(&statInfo);
  if (NS_FAILED(rv)) {
    return rv;
  }

  if (!S_ISDIR(statInfo.st_mode)) {
    *aFileSize = (int64_t)statInfo.st_size;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::SetFileSize(int64_t aFileSize) {
  CHECK_mPath();

#if defined(HAVE_TRUNCATE64)
  if (truncate64(mPath.get(), (off64_t)aFileSize) == -1) {
    return NSRESULT_FOR_ERRNO();
  }
#else
  off_t size = (off_t)aFileSize;
  if (truncate(mPath.get(), size) == -1) {
    return NSRESULT_FOR_ERRNO();
  }
#endif
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetFileSizeOfLink(int64_t* aFileSize) {
  CHECK_mPath();
  if (NS_WARN_IF(!aFileSize)) {
    return NS_ERROR_INVALID_ARG;
  }

  struct STAT sbuf;
  if (LSTAT(mPath.get(), &sbuf) == -1) {
    return NSRESULT_FOR_ERRNO();
  }

  *aFileSize = (int64_t)sbuf.st_size;
  return NS_OK;
}

#if defined(USE_LINUX_QUOTACTL)
static bool GetDeviceName(unsigned int aDeviceMajor, unsigned int aDeviceMinor,
                          nsACString& aDeviceName) {
  bool ret = false;

  const int kMountInfoLineLength = 200;
  const int kMountInfoDevPosition = 6;

  char mountinfoLine[kMountInfoLineLength];
  char deviceNum[kMountInfoLineLength];

  SprintfLiteral(deviceNum, "%u:%u", aDeviceMajor, aDeviceMinor);

  FILE* f = fopen("/proc/self/mountinfo", "rt");
  if (!f) {
    return ret;
  }

  while (fgets(mountinfoLine, kMountInfoLineLength, f)) {
    char* p_dev = strstr(mountinfoLine, deviceNum);

    for (int i = 0; i < kMountInfoDevPosition && p_dev; ++i) {
      p_dev = strchr(p_dev, ' ');
      if (p_dev) {
        p_dev++;
      }
    }

    if (p_dev) {
      char* p_dev_end = strchr(p_dev, ' ');
      if (p_dev_end) {
        *p_dev_end = '\0';
        aDeviceName.Assign(p_dev);
        ret = true;
        break;
      }
    }
  }

  fclose(f);
  return ret;
}
#endif

#if defined(USE_LINUX_QUOTACTL)
template <typename StatInfoFunc, typename QuotaInfoFunc>
nsresult nsLocalFile::GetDiskInfo(StatInfoFunc&& aStatInfoFunc,
                                  QuotaInfoFunc&& aQuotaInfoFunc,
                                  int64_t* aResult)
#else
template <typename StatInfoFunc>
nsresult nsLocalFile::GetDiskInfo(StatInfoFunc&& aStatInfoFunc,
                                  int64_t* aResult)
#endif
{
  if (NS_WARN_IF(!aResult)) {
    return NS_ERROR_INVALID_ARG;
  }


#if defined(STATFS)

  CHECK_mPath();

  struct STATFS fs_buf;


  if (STATFS(mPath.get(), &fs_buf) < 0) {
#if defined(DEBUG)
    printf("ERROR: GetDiskInfo: STATFS call FAILED. \n");
#endif
    return NS_ERROR_FAILURE;
  }

  CheckedInt64 statfsResult = std::forward<StatInfoFunc>(aStatInfoFunc)(fs_buf);
  if (!statfsResult.isValid()) {
    return NS_ERROR_CANNOT_CONVERT_DATA;
  }

  *aResult = statfsResult.value();

#if defined(USE_LINUX_QUOTACTL)

  struct STAT statInfo {};
  nsresult rv = StatFile(&statInfo);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsAutoCString deviceName;
  if (!GetDeviceName(major(statInfo.st_dev), minor(statInfo.st_dev),
                     deviceName)) {
    return NS_OK;
  }

  struct dqblk dq;
  if (!quotactl(QCMD(Q_GETQUOTA, USRQUOTA), deviceName.get(), getuid(),
                (caddr_t)&dq)
#if defined(QIF_BLIMITS)
      && dq.dqb_valid & QIF_BLIMITS
#endif
      && dq.dqb_bhardlimit) {
    CheckedInt64 quotaResult = std::forward<QuotaInfoFunc>(aQuotaInfoFunc)(dq);
    if (!quotaResult.isValid()) {
      return NS_OK;
    }

    if (quotaResult.value() < *aResult) {
      *aResult = quotaResult.value();
    }
  }
#endif

#if defined(DEBUG_DISK_SPACE)
  printf("DiskInfo: %lu bytes\n", *aResult);
#endif

  return NS_OK;

#else
#if defined(DEBUG)
  printf("ERROR: GetDiskInfo: Not implemented for plaforms without statfs.\n");
#endif
  return NS_ERROR_NOT_IMPLEMENTED;

#endif
}

NS_IMETHODIMP
nsLocalFile::GetDiskSpaceAvailable(int64_t* aDiskSpaceAvailable) {
#if defined(STATFS)
  return GetDiskInfo(
      [](const struct STATFS& aStatInfo) {
        return aStatInfo.f_bavail * static_cast<uint64_t>(aStatInfo.F_BSIZE);
      },
#if defined(USE_LINUX_QUOTACTL)
      [](const struct dqblk& aQuotaInfo) -> uint64_t {
        const uint64_t hardlimit = aQuotaInfo.dqb_bhardlimit * BLOCK_SIZE;
        if (hardlimit > aQuotaInfo.dqb_curspace) {
          return hardlimit - aQuotaInfo.dqb_curspace;
        }
        return 0;
      },
#endif
      aDiskSpaceAvailable);
#else
#if defined(DEBUG)
  printf(
      "ERROR: GetDiskSpaceAvailable: Not implemented for plaforms without "
      "statfs.\n");
#endif
  return NS_ERROR_NOT_IMPLEMENTED;
#endif
}

NS_IMETHODIMP
nsLocalFile::GetDiskCapacity(int64_t* aDiskCapacity) {
#if defined(STATFS)
  return GetDiskInfo(
      [](const struct STATFS& aStatInfo) {
        return aStatInfo.f_blocks * static_cast<uint64_t>(aStatInfo.F_BSIZE);
      },
#if defined(USE_LINUX_QUOTACTL)
      [](const struct dqblk& aQuotaInfo) {
        return aQuotaInfo.dqb_bhardlimit * BLOCK_SIZE;
      },
#endif
      aDiskCapacity);
#else
#if defined(DEBUG)
  printf(
      "ERROR: GetDiskCapacity: Not implemented for plaforms without "
      "statfs.\n");
#endif
  return NS_ERROR_NOT_IMPLEMENTED;
#endif
}

NS_IMETHODIMP
nsLocalFile::GetParent(nsIFile** aParent) {
  CHECK_mPath();
  if (NS_WARN_IF(!aParent)) {
    return NS_ERROR_INVALID_ARG;
  }
  *aParent = nullptr;

  if (mPath.EqualsLiteral("/")) {
    return NS_OK;
  }

  char* buffer = mPath.BeginWriting();
  char* slashp = strrchr(buffer, '/');
  NS_ASSERTION(slashp, "non-canonical path?");
  if (!slashp) {
    return NS_ERROR_FILE_INVALID_PATH;
  }

  if (slashp == buffer) {
    slashp++;
  }

  char c = *slashp;
  *slashp = '\0';

  nsCOMPtr<nsIFile> localFile;
  nsresult rv = NS_NewNativeLocalFile(nsDependentCString(buffer),
                                      getter_AddRefs(localFile));

  *slashp = c;

  if (NS_FAILED(rv)) {
    return rv;
  }

  localFile.forget(aParent);
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::Exists(bool* aResult) {
  CHECK_mPath();
  if (NS_WARN_IF(!aResult)) {
    return NS_ERROR_INVALID_ARG;
  }

  *aResult = (access(mPath.get(), F_OK) == 0);
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::IsWritable(bool* aResult) {
  CHECK_mPath();
  if (NS_WARN_IF(!aResult)) {
    return NS_ERROR_INVALID_ARG;
  }

  *aResult = (access(mPath.get(), W_OK) == 0);
  if (*aResult || errno == EACCES) {
    return NS_OK;
  }
  return NSRESULT_FOR_ERRNO();
}

NS_IMETHODIMP
nsLocalFile::IsReadable(bool* aResult) {
  CHECK_mPath();
  if (NS_WARN_IF(!aResult)) {
    return NS_ERROR_INVALID_ARG;
  }

  *aResult = (access(mPath.get(), R_OK) == 0);
  if (*aResult || errno == EACCES) {
    return NS_OK;
  }
  return NSRESULT_FOR_ERRNO();
}

NS_IMETHODIMP
nsLocalFile::IsExecutable(bool* aResult) {
  CHECK_mPath();
  if (NS_WARN_IF(!aResult)) {
    return NS_ERROR_INVALID_ARG;
  }


  bool symLink;
  nsresult rv = IsSymlink(&symLink);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsAutoString path;
  if (symLink) {
    GetTarget(path);
  } else {
    GetPath(path);
  }

  int32_t dotIdx = path.RFindChar(char16_t('.'));
  if (dotIdx != kNotFound) {
    char16_t* p = path.BeginWriting();
    for (p += dotIdx + 1; *p; ++p) {
      *p += (*p >= L'A' && *p <= L'Z') ? 'a' - 'A' : 0;
    }

    static const char* const executableExts[] = {
        ".air",  
        ".jar"  
    };
    nsDependentSubstring ext = Substring(path, dotIdx);
    for (auto executableExt : executableExts) {
      if (ext.EqualsASCII(executableExt)) {
        *aResult = true;
        return NS_OK;
      }
    }
  }


  *aResult = (access(mPath.get(), X_OK) == 0);
  if (*aResult || errno == EACCES) {
    return NS_OK;
  }
  return NSRESULT_FOR_ERRNO();
}

NS_IMETHODIMP
nsLocalFile::IsDirectory(bool* aResult) {
  if (NS_WARN_IF(!aResult)) {
    return NS_ERROR_INVALID_ARG;
  }

  *aResult = false;

  struct STAT statInfo {};
  nsresult rv = StatFile(&statInfo);
  if (NS_FAILED(rv)) {
    return rv;
  }

  *aResult = S_ISDIR(statInfo.st_mode);

  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::IsFile(bool* aResult) {
  if (NS_WARN_IF(!aResult)) {
    return NS_ERROR_INVALID_ARG;
  }
  *aResult = false;

  struct STAT statInfo {};
  nsresult rv = StatFile(&statInfo);
  if (NS_FAILED(rv)) {
    return rv;
  }

  *aResult = S_ISREG(statInfo.st_mode);
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::IsHidden(bool* aResult) {
  if (NS_WARN_IF(!aResult)) {
    return NS_ERROR_INVALID_ARG;
  }
  nsACString::const_iterator begin, end;
  LocateNativeLeafName(begin, end);
  *aResult = (*begin == '.');
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::IsSymlink(bool* aResult) {
  if (NS_WARN_IF(!aResult)) {
    return NS_ERROR_INVALID_ARG;
  }
  CHECK_mPath();

  struct STAT symStat;
  if (LSTAT(mPath.get(), &symStat) == -1) {
    return NSRESULT_FOR_ERRNO();
  }
  *aResult = S_ISLNK(symStat.st_mode);
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::IsSpecial(bool* aResult) {
  if (NS_WARN_IF(!aResult)) {
    return NS_ERROR_INVALID_ARG;
  }

  struct STAT statInfo {};
  nsresult rv = StatFile(&statInfo);
  if (NS_FAILED(rv)) {
    return rv;
  }

  *aResult = S_ISCHR(statInfo.st_mode) || S_ISBLK(statInfo.st_mode) ||
#if defined(S_ISSOCK)
             S_ISSOCK(statInfo.st_mode) ||
#endif
             S_ISFIFO(statInfo.st_mode);

  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::Equals(nsIFile* aInFile, bool* aResult) {
  if (NS_WARN_IF(!aInFile)) {
    return NS_ERROR_INVALID_ARG;
  }
  if (NS_WARN_IF(!aResult)) {
    return NS_ERROR_INVALID_ARG;
  }
  *aResult = false;

  nsAutoCString inPath;
  nsresult rv = aInFile->GetNativePath(inPath);
  if (NS_FAILED(rv)) {
    return rv;
  }

  *aResult = !strcmp(inPath.get(), mPath.get());
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::Contains(nsIFile* aInFile, bool* aResult) {
  CHECK_mPath();
  if (NS_WARN_IF(!aInFile)) {
    return NS_ERROR_INVALID_ARG;
  }
  if (NS_WARN_IF(!aResult)) {
    return NS_ERROR_INVALID_ARG;
  }

  nsAutoCString inPath;
  nsresult rv;

  if (NS_FAILED(rv = aInFile->GetNativePath(inPath))) {
    return rv;
  }

  *aResult = false;

  ssize_t len = mPath.Length();
  if (strncmp(mPath.get(), inPath.get(), len) == 0) {
    if (inPath[len] == '/') {
      *aResult = true;
    }
  }

  return NS_OK;
}

static nsresult ReadLinkSafe(const nsCString& aTarget, int32_t aExpectedSize,
                             nsACString& aOutBuffer) {
  const auto allocSize = CheckedInt<size_t>(aExpectedSize) + 1;
  if (!allocSize.isValid()) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  auto result = aOutBuffer.BulkWrite(allocSize.value(), 0, false);
  if (result.isErr()) {
    return result.unwrapErr();
  }

  auto handle = result.unwrap();

  while (true) {
    ssize_t bytesWritten =
        readlink(aTarget.get(), handle.Elements(), handle.Length());
    if (bytesWritten < 0) {
      return NSRESULT_FOR_ERRNO();
    }

    if ((size_t)bytesWritten < handle.Length()) {
      handle.Finish(bytesWritten, false);
      return NS_OK;
    }

    auto restartResult = handle.RestartBulkWrite(handle.Length() * 2, 0, false);
    if (restartResult.isErr()) {
      return restartResult.unwrapErr();
    }
  }
}

NS_IMETHODIMP
nsLocalFile::GetNativeTarget(nsACString& aResult) {
  CHECK_mPath();
  aResult.Truncate();

  struct STAT symStat;
  if (LSTAT(mPath.get(), &symStat) == -1) {
    return NSRESULT_FOR_ERRNO();
  }

  if (!S_ISLNK(symStat.st_mode)) {
    return NS_ERROR_FILE_INVALID_PATH;
  }

  nsAutoCString target;
  nsresult rv = ReadLinkSafe(mPath, symStat.st_size, target);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsCOMPtr<nsIFile> self(this);
  int32_t maxLinks = 40;
  while (true) {
    if (maxLinks-- == 0) {
      rv = NS_ERROR_FILE_UNRESOLVABLE_SYMLINK;
      break;
    }

    if (target[0] != '/') {
      nsCOMPtr<nsIFile> parent;
      if (NS_FAILED(rv = self->GetParent(getter_AddRefs(parent)))) {
        break;
      }
      if (NS_FAILED(rv = parent->AppendRelativeNativePath(target))) {
        break;
      }
      if (NS_FAILED(rv = parent->GetNativePath(aResult))) {
        break;
      }
      self = parent;
    } else {
      aResult = target;
    }

    const nsPromiseFlatCString& flatRetval = PromiseFlatCString(aResult);

    if (LSTAT(flatRetval.get(), &symStat) == -1) {
      break;
    }

    if (!S_ISLNK(symStat.st_mode)) {
      break;
    }

    nsAutoCString newTarget;
    rv = ReadLinkSafe(flatRetval, symStat.st_size, newTarget);
    if (NS_FAILED(rv)) {
      break;
    }

    target = newTarget;
  }

  if (NS_FAILED(rv)) {
    aResult.Truncate();
  }
  return rv;
}

NS_IMETHODIMP
nsLocalFile::GetDirectoryEntriesImpl(nsIDirectoryEnumerator** aEntries) {
  RefPtr<nsDirEnumeratorUnix> dir;
  nsresult rv = nsDirEnumeratorUnix::Create(this, dir);
  if (NS_FAILED(rv)) {
    *aEntries = nullptr;
  } else {
    dir.forget(aEntries);
  }

  return rv;
}

NS_IMETHODIMP
nsLocalFile::Load(PRLibrary** aResult) {
  CHECK_mPath();
  if (NS_WARN_IF(!aResult)) {
    return NS_ERROR_INVALID_ARG;
  }

#if defined(NS_BUILD_REFCNT_LOGGING)
  nsTraceRefcnt::SetActivityIsLegal(false);
#endif

  *aResult = PR_LoadLibrary(mPath.get());

#if defined(NS_BUILD_REFCNT_LOGGING)
  nsTraceRefcnt::SetActivityIsLegal(true);
#endif

  if (!*aResult) {
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetPersistentDescriptor(nsACString& aPersistentDescriptor) {
  return GetNativePath(aPersistentDescriptor);
}

NS_IMETHODIMP
nsLocalFile::SetPersistentDescriptor(const nsACString& aPersistentDescriptor) {
  return InitWithNativePath(aPersistentDescriptor);
}

NS_IMETHODIMP
nsLocalFile::Reveal() {
  if (!FilePreferences::IsAllowedPath(mPath)) {
    return NS_ERROR_FILE_ACCESS_DENIED;
  }

#if defined(MOZ_WIDGET_GTK)
  nsCOMPtr<nsIGIOService> giovfs = do_GetService(NS_GIOSERVICE_CONTRACTID);
  if (!giovfs) {
    return NS_ERROR_FAILURE;
  }
  return giovfs->RevealFile(this);
#else
  return NS_ERROR_FAILURE;
#endif
}

NS_IMETHODIMP
nsLocalFile::Launch() {
  if (!FilePreferences::IsAllowedPath(mPath)) {
    return NS_ERROR_FILE_ACCESS_DENIED;
  }

#if defined(MOZ_WIDGET_GTK)
  nsCOMPtr<nsIGIOService> giovfs = do_GetService(NS_GIOSERVICE_CONTRACTID);
  if (!giovfs) {
    return NS_ERROR_FAILURE;
  }

  return giovfs->LaunchFile(mPath);
#else
  return NS_ERROR_FAILURE;
#endif
}

nsresult NS_NewNativeLocalFile(const nsACString& aPath, nsIFile** aResult) {
  RefPtr<nsLocalFile> file = new nsLocalFile();

  if (!aPath.IsEmpty()) {
    nsresult rv = file->InitWithNativePath(aPath);
    if (NS_FAILED(rv)) {
      return rv;
    }
  }
  file.forget(aResult);
  return NS_OK;
}

nsresult NS_NewUTF8LocalFile(const nsACString& aPath, nsIFile** aResult) {
  static_assert(NS_IsNativeUTF8());
  return NS_NewNativeLocalFile(aPath, aResult);
}

nsresult NS_NewPathStringLocalFile(const PathSubstring& aPath,
                                   nsIFile** aResult) {
  return NS_NewNativeLocalFile(aPath, aResult);
}


#define SET_UCS(func, ucsArg)                          \
  {                                                    \
    nsAutoCString buf;                                 \
    nsresult rv = NS_CopyUnicodeToNative(ucsArg, buf); \
    if (NS_FAILED(rv)) return rv;                      \
    return (func)(buf);                                \
  }

#define GET_UCS(func, ucsArg)                   \
  {                                             \
    nsAutoCString buf;                          \
    nsresult rv = (func)(buf);                  \
    if (NS_FAILED(rv)) return rv;               \
    return NS_CopyNativeToUnicode(buf, ucsArg); \
  }

#define SET_UCS_2ARGS_2(func, opaqueArg, ucsArg)       \
  {                                                    \
    nsAutoCString buf;                                 \
    nsresult rv = NS_CopyUnicodeToNative(ucsArg, buf); \
    if (NS_FAILED(rv)) return rv;                      \
    return (func)(opaqueArg, buf);                     \
  }

nsresult nsLocalFile::InitWithPath(const nsAString& aFilePath) {
  SET_UCS(InitWithNativePath, aFilePath);
}
nsresult nsLocalFile::Append(const nsAString& aNode) {
  SET_UCS(AppendNative, aNode);
}
nsresult nsLocalFile::AppendRelativePath(const nsAString& aNode) {
  SET_UCS(AppendRelativeNativePath, aNode);
}
nsresult nsLocalFile::GetLeafName(nsAString& aLeafName) {
  GET_UCS(GetNativeLeafName, aLeafName);
}
nsresult nsLocalFile::SetLeafName(const nsAString& aLeafName) {
  SET_UCS(SetNativeLeafName, aLeafName);
}
nsresult nsLocalFile::GetPath(nsAString& aResult) {
  return NS_CopyNativeToUnicode(mPath, aResult);
}
nsresult nsLocalFile::CopyTo(nsIFile* aNewParentDir,
                             const nsAString& aNewName) {
  SET_UCS_2ARGS_2(CopyToNative, aNewParentDir, aNewName);
}
nsresult nsLocalFile::CopyToFollowingLinks(nsIFile* aNewParentDir,
                                           const nsAString& aNewName) {
  SET_UCS_2ARGS_2(CopyToFollowingLinksNative, aNewParentDir, aNewName);
}
nsresult nsLocalFile::MoveTo(nsIFile* aNewParentDir,
                             const nsAString& aNewName) {
  SET_UCS_2ARGS_2(MoveToNative, aNewParentDir, aNewName);
}
NS_IMETHODIMP
nsLocalFile::MoveToFollowingLinks(nsIFile* aNewParentDir,
                                  const nsAString& aNewName) {
  SET_UCS_2ARGS_2(MoveToFollowingLinksNative, aNewParentDir, aNewName);
}

NS_IMETHODIMP
nsLocalFile::RenameTo(nsIFile* aNewParentDir, const nsAString& aNewName) {
  SET_UCS_2ARGS_2(RenameToNative, aNewParentDir, aNewName);
}

NS_IMETHODIMP
nsLocalFile::RenameToNative(nsIFile* aNewParentDir,
                            const nsACString& aNewName) {
  nsresult rv;

  CHECK_mPath();

  nsAutoCString newPathName;
  rv = GetNativeTargetPathName(aNewParentDir, aNewName, newPathName);
  if (NS_FAILED(rv)) {
    return rv;
  }

  if (!FilePreferences::IsAllowedPath(newPathName)) {
    return NS_ERROR_FILE_ACCESS_DENIED;
  }

  if (rename(mPath.get(), newPathName.get()) < 0) {
    if (errno == EXDEV) {
      rv = NS_ERROR_FILE_ACCESS_DENIED;
    } else {
      rv = NSRESULT_FOR_ERRNO();
    }
  }

  return rv;
}

nsresult nsLocalFile::GetTarget(nsAString& aResult) {
  GET_UCS(GetNativeTarget, aResult);
}

nsresult NS_NewLocalFile(const nsAString& aPath, nsIFile** aResult) {
  nsAutoCString buf;
  nsresult rv = NS_CopyUnicodeToNative(aPath, buf);
  if (NS_FAILED(rv)) {
    return rv;
  }
  return NS_NewNativeLocalFile(buf, aResult);
}

