/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsAnonymousTemporaryFile.h"
#include "nsXULAppAPI.h"
#include "nsCOMPtr.h"
#include "nsString.h"
#include "prio.h"
#include "SpecialSystemDirectory.h"


using namespace mozilla;

static nsresult GetTempDir(nsIFile** aTempDir) {
  if (NS_WARN_IF(!aTempDir)) {
    return NS_ERROR_INVALID_ARG;
  }
  nsCOMPtr<nsIFile> tmpFile;
  nsresult rv =
      GetSpecialSystemDirectory(OS_TemporaryDirectory, getter_AddRefs(tmpFile));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }


  tmpFile.forget(aTempDir);

  return NS_OK;
}

nsresult NS_OpenAnonymousTemporaryNsIFile(nsIFile** aFile) {
  MOZ_ASSERT(XRE_IsParentProcess());

  if (NS_WARN_IF(!aFile)) {
    return NS_ERROR_INVALID_ARG;
  }

  nsresult rv;
  nsCOMPtr<nsIFile> tmpFile;
  rv = GetTempDir(getter_AddRefs(tmpFile));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsAutoCString name("mozilla-temp-");
  name.AppendInt(rand());

  rv = tmpFile->AppendNative(name);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = tmpFile->CreateUnique(nsIFile::NORMAL_FILE_TYPE, 0600);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  tmpFile.forget(aFile);
  return NS_OK;
}

nsresult NS_OpenAnonymousTemporaryFile(PRFileDesc** aOutFileDesc) {
  nsCOMPtr<nsIFile> tmpFile;
  nsresult rv = NS_OpenAnonymousTemporaryNsIFile(getter_AddRefs(tmpFile));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = tmpFile->OpenNSPRFileDesc(PR_RDWR | nsIFile::DELETE_ON_CLOSE, PR_IRWXU,
                                 aOutFileDesc);

  return rv;
}
