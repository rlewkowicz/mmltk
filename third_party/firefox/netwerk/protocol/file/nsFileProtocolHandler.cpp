/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsIFile.h"
#include "nsFileProtocolHandler.h"
#include "nsFileChannel.h"
#include "nsStandardURL.h"
#include "nsURLHelper.h"
#include "nsIURIMutator.h"

#include "nsNetUtil.h"

#include "mozilla/net/NeckoCommon.h"


#if defined(XP_UNIX)
#  include "nsINIParser.h"
#  define DESKTOP_ENTRY_SECTION "Desktop Entry"
#endif


nsresult nsFileProtocolHandler::Init() { return NS_OK; }

NS_IMPL_ISUPPORTS(nsFileProtocolHandler, nsIFileProtocolHandler,
                  nsIProtocolHandler, nsISupportsWeakReference)


#if defined(XP_UNIX)
NS_IMETHODIMP
nsFileProtocolHandler::ReadURLFile(nsIFile* aFile, nsIURI** aURI) {
  nsAutoCString leafName;
  nsresult rv = aFile->GetNativeLeafName(leafName);
  if (NS_FAILED(rv) || !StringEndsWith(leafName, ".desktop"_ns)) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  bool isFile = false;
  rv = aFile->IsFile(&isFile);
  if (NS_FAILED(rv) || !isFile) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsINIParser parser;
  rv = parser.Init(aFile);
  if (NS_FAILED(rv)) return rv;

  nsAutoCString type;
  parser.GetString(DESKTOP_ENTRY_SECTION, "Type", type);
  if (!type.EqualsLiteral("Link")) return NS_ERROR_NOT_AVAILABLE;

  nsAutoCString url;
  rv = parser.GetString(DESKTOP_ENTRY_SECTION, "URL", url);
  if (NS_FAILED(rv) || url.IsEmpty()) return NS_ERROR_NOT_AVAILABLE;

  return NS_NewURI(aURI, url);
}

#else
NS_IMETHODIMP
nsFileProtocolHandler::ReadURLFile(nsIFile* aFile, nsIURI** aURI) {
  return NS_ERROR_NOT_AVAILABLE;
}
#endif

NS_IMETHODIMP
nsFileProtocolHandler::ReadShellLink(nsIFile* aFile, nsIURI** aURI) {
  return NS_ERROR_NOT_AVAILABLE;
}

NS_IMETHODIMP
nsFileProtocolHandler::GetScheme(nsACString& result) {
  result.AssignLiteral("file");
  return NS_OK;
}

NS_IMETHODIMP
nsFileProtocolHandler::NewChannel(nsIURI* uri, nsILoadInfo* aLoadInfo,
                                  nsIChannel** result) {
  nsresult rv;

  RefPtr<nsFileChannel> chan = new nsFileChannel(uri);

  nsAutoCString host;
  rv = uri->GetHost(host);
  NS_ENSURE_SUCCESS(rv, rv);
  if (NS_WARN_IF(!host.IsEmpty())) {
    return NS_ERROR_FAILURE;
  }

  rv = chan->SetLoadInfo(aLoadInfo);
  if (NS_FAILED(rv)) {
    return rv;
  }

  rv = chan->Init();
  if (NS_FAILED(rv)) {
    return rv;
  }

  *result = chan.forget().downcast<nsBaseChannel>().take();
  return NS_OK;
}

NS_IMETHODIMP
nsFileProtocolHandler::AllowPort(int32_t port, const char* scheme,
                                 bool* result) {
  *result = false;
  return NS_OK;
}


NS_IMETHODIMP
nsFileProtocolHandler::NewFileURI(nsIFile* aFile, nsIURI** aResult) {
  NS_ENSURE_ARG_POINTER(aFile);

  RefPtr<nsIFile> file(aFile);
  return NS_MutateURI(new mozilla::net::nsStandardURL::Mutator())
      .Apply(&nsIFileURLMutator::SetFile, file)
      .Finalize(aResult);
}

NS_IMETHODIMP
nsFileProtocolHandler::NewFileURIMutator(nsIFile* aFile,
                                         nsIURIMutator** aResult) {
  NS_ENSURE_ARG_POINTER(aFile);
  nsresult rv;

  nsCOMPtr<nsIURIMutator> mutator = new mozilla::net::nsStandardURL::Mutator();
  nsCOMPtr<nsIFileURLMutator> fileMutator = do_QueryInterface(mutator, &rv);
  if (NS_FAILED(rv)) {
    return rv;
  }

  rv = fileMutator->SetFile(aFile);
  if (NS_FAILED(rv)) {
    return rv;
  }

  mutator.forget(aResult);
  return NS_OK;
}

NS_IMETHODIMP
nsFileProtocolHandler::GetURLSpecFromFile(nsIFile* file, nsACString& result) {
  NS_ENSURE_ARG_POINTER(file);
  return net_GetURLSpecFromFile(file, result);
}

NS_IMETHODIMP
nsFileProtocolHandler::GetURLSpecFromActualFile(nsIFile* file,
                                                nsACString& result) {
  NS_ENSURE_ARG_POINTER(file);
  return net_GetURLSpecFromActualFile(file, result);
}

NS_IMETHODIMP
nsFileProtocolHandler::GetURLSpecFromDir(nsIFile* file, nsACString& result) {
  NS_ENSURE_ARG_POINTER(file);
  return net_GetURLSpecFromDir(file, result);
}

NS_IMETHODIMP
nsFileProtocolHandler::GetFileFromURLSpec(const nsACString& spec,
                                          nsIFile** result) {
  return net_GetFileFromURLSpec(spec, result);
}
