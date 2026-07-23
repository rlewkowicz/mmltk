/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Bootstrap.h"

#include "nsXPCOMPrivate.h"
#include <stdlib.h>
#include <stdio.h>

#include "mozilla/FileUtils.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Try.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/UniquePtrExtensions.h"


using namespace mozilla;

#define XPCOM_DEPENDENT_LIBS_LIST "dependentlibs.list"

#  define READ_TEXTMODE "r"

typedef void (*NSFuncPtr)();

using LibHandleType = void*;

using LibHandleResult = ::mozilla::Result<LibHandleType, DLErrorType>;

#  include <dlfcn.h>

#if defined(MOZ_LINKER)
extern "C" {
NS_HIDDEN __typeof(dlopen) __wrap_dlopen;
NS_HIDDEN __typeof(dlsym) __wrap_dlsym;
NS_HIDDEN __typeof(dlclose) __wrap_dlclose;
}

#    define dlopen __wrap_dlopen
#    define dlsym __wrap_dlsym
#    define dlclose __wrap_dlclose
#endif

static LibHandleResult GetLibHandle(pathstr_t aDependentLib) {
  LibHandleType libHandle = dlopen(aDependentLib, RTLD_GLOBAL | RTLD_LAZY
  );
  if (!libHandle) {
    UniqueFreePtr<char> errMsg(strdup(dlerror()));
    fprintf(stderr, "XPCOMGlueLoad error for file %s:\n%s\n", aDependentLib,
            errMsg.get());
    return Err(std::move(errMsg));
  }
  return libHandle;
}

static NSFuncPtr GetSymbol(LibHandleType aLibHandle, const char* aSymbol) {
  return (NSFuncPtr)dlsym(aLibHandle, aSymbol);
}

#if !defined(MOZ_LINKER) && !0
static void CloseLibHandle(LibHandleType aLibHandle) { dlclose(aLibHandle); }
#endif

struct DependentLib {
  LibHandleType libHandle;
  DependentLib* next;
};

static DependentLib* sTop;

static void AppendDependentLib(LibHandleType aLibHandle) {
  auto* d = new DependentLib;
  if (!d) {
    return;
  }

  d->next = sTop;
  d->libHandle = aLibHandle;

  sTop = d;
}

using ReadDependentCBResult = ::mozilla::Result<::mozilla::Ok, DLErrorType>;

static ReadDependentCBResult ReadDependentCB(
    pathstr_t aDependentLib, LibLoadingStrategy aLibLoadingStrategy) {
#if !defined(MOZ_LINKER) && !0
  if (aLibLoadingStrategy == LibLoadingStrategy::ReadAhead) {
    ReadAheadLib(aDependentLib);
  }
#endif
  LibHandleType libHandle = MOZ_TRY(GetLibHandle(aDependentLib));

  AppendDependentLib(libHandle);
  return Ok();
}

inline FILE* TS_tfopen(const char* aPath, const char* aMode) {
  return fopen(aPath, aMode);
}

#if !defined(MOZ_LINKER) && !0
static void XPCOMGlueUnload() {
  while (sTop) {
    CloseLibHandle(sTop->libHandle);

    DependentLib* temp = sTop;
    sTop = sTop->next;

    delete temp;
  }
}
#endif


using XPCOMGlueLoadError = BootstrapError;
using XPCOMGlueLoadResult =
    ::mozilla::Result<::mozilla::Ok, XPCOMGlueLoadError>;

static XPCOMGlueLoadResult XPCOMGlueLoad(
    const char* aXPCOMFile, LibLoadingStrategy aLibLoadingStrategy) {
#if defined(MOZ_LINKER) || 0
  ReadDependentCBResult readDependentCBResult =
      ReadDependentCB(aXPCOMFile, aLibLoadingStrategy);
  if (readDependentCBResult.isErr()) {
    return Err(AsVariant(readDependentCBResult.unwrapErr()));
  }
#else
  char xpcomDir[MAXPATHLEN];
  const char* lastSlash = strrchr(aXPCOMFile, '/');
  char* cursor;
  if (lastSlash) {
    size_t len = size_t(lastSlash - aXPCOMFile);

    if (len > MAXPATHLEN - sizeof(XPCOM_FILE_PATH_SEPARATOR
                                      XPCOM_DEPENDENT_LIBS_LIST)) {
      return Err(AsVariant(NS_ERROR_FAILURE));
    }
    memcpy(xpcomDir, aXPCOMFile, len);
    strcpy(xpcomDir + len, XPCOM_FILE_PATH_SEPARATOR
               XPCOM_DEPENDENT_LIBS_LIST);
    cursor = xpcomDir + len + 1;
  } else {
    strcpy(xpcomDir, XPCOM_DEPENDENT_LIBS_LIST);
    cursor = xpcomDir;
  }


  const auto flist = TS_tfopen(xpcomDir, READ_TEXTMODE);
  const auto cleanup = MakeScopeExit([&]() {
    if (flist) {
      fclose(flist);
    }
  });
  if (!flist) {
    return Err(AsVariant(NS_ERROR_FAILURE));
  }

  *cursor = '\0';

  char buffer[MAXPATHLEN];

  while (fgets(buffer, sizeof(buffer), flist)) {
    int l = strlen(buffer);

    if (l == 0 || *buffer == '#') {
      continue;
    }

    if (buffer[l - 1] == '\n') {
      buffer[l - 1] = '\0';
    }

    if (l + size_t(cursor - xpcomDir) > MAXPATHLEN) {
      return Err(AsVariant(NS_ERROR_FAILURE));
    }

    strcpy(cursor, buffer);
    ReadDependentCBResult readDependentCBResult =
        ReadDependentCB(xpcomDir, aLibLoadingStrategy);
    if (readDependentCBResult.isErr()) {
      XPCOMGlueUnload();
      return Err(AsVariant(readDependentCBResult.unwrapErr()));
    }

  }
#endif
  return Ok();
}

#if defined(MOZ_WIDGET_GTK) && \
    (defined(MOZ_MEMORY) || 0 || 0)
#  define MOZ_GSLICE_INIT
#endif

#if defined(MOZ_GSLICE_INIT)
#  include <glib.h>

class GSliceInit {
 public:
  GSliceInit() {
    mHadGSlice = bool(getenv("G_SLICE"));
    if (!mHadGSlice) {
      setenv("G_SLICE", "always-malloc", 1);
    }
  }

  ~GSliceInit() {
    if (!mHadGSlice) {
      unsetenv("G_SLICE");
    }
  }

 private:
  bool mHadGSlice;
};
#endif

namespace mozilla {

BootstrapResult GetBootstrap(const char* aXPCOMFile,
                             LibLoadingStrategy aLibLoadingStrategy) {
#if defined(MOZ_GSLICE_INIT)
  GSliceInit gSliceInit;
#endif

  if (!aXPCOMFile) {
    return Err(AsVariant(NS_ERROR_INVALID_ARG));
  }

  char* lastSlash =
      strrchr(const_cast<char*>(aXPCOMFile), XPCOM_FILE_PATH_SEPARATOR[0]);
  if (!lastSlash) {
    return Err(AsVariant(NS_ERROR_FILE_INVALID_PATH));
  }

  size_t base_len = size_t(lastSlash - aXPCOMFile) + 1;

  UniqueFreePtr<char> file(
      reinterpret_cast<char*>(malloc(base_len + sizeof(XPCOM_DLL))));
  memcpy(file.get(), aXPCOMFile, base_len);
  memcpy(file.get() + base_len, XPCOM_DLL, sizeof(XPCOM_DLL));

  MOZ_TRY(XPCOMGlueLoad(file.get(), aLibLoadingStrategy));

  if (!sTop) {
    return Err(AsVariant(NS_ERROR_NOT_AVAILABLE));
  }


  GetBootstrapType func =
      (GetBootstrapType)GetSymbol(sTop->libHandle, "XRE_GetBootstrap");
  if (!func) {
    return Err(AsVariant(NS_ERROR_NOT_AVAILABLE));
  }

  Bootstrap::UniquePtr b;
  (*func)(b);

  return b;
}

}  
