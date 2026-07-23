/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DownloadPlatform.h"
#include "nsNetUtil.h"
#include "nsString.h"
#include "nsINestedURI.h"
#include "nsIProtocolHandler.h"
#include "nsIURI.h"
#include "nsIFile.h"
#include "xpcpublic.h"

#include "mozilla/dom/Promise.h"
#include "mozilla/Preferences.h"

#define PREF_BDM_ADDTORECENTDOCS "browser.download.manager.addToRecentDocs"



#if defined(MOZ_WIDGET_GTK)
#  include <gtk/gtk.h>
#endif

using namespace mozilla;
using dom::Promise;

DownloadPlatform* DownloadPlatform::gDownloadPlatformService = nullptr;

NS_IMPL_ISUPPORTS(DownloadPlatform, mozIDownloadPlatform);

DownloadPlatform* DownloadPlatform::GetDownloadPlatform() {
  if (!gDownloadPlatformService) {
    gDownloadPlatformService = new DownloadPlatform();
  }

  NS_ADDREF(gDownloadPlatformService);

  return gDownloadPlatformService;
}

#if defined(MOZ_WIDGET_GTK)
static void gio_set_metadata_done(GObject* source_obj, GAsyncResult* res,
                                  gpointer user_data) {
  GError* err = nullptr;
  g_file_set_attributes_finish(G_FILE(source_obj), res, nullptr, &err);
  if (err) {
#if defined(DEBUG)
    NS_DebugBreak(NS_DEBUG_WARNING, "Set file metadata failed: ", err->message,
                  __FILE__, __LINE__);
#endif
    g_error_free(err);
  }
}
#endif



nsresult DownloadPlatform::DownloadDone(nsIURI* aSource, nsIURI* aReferrer,
                                        nsIFile* aTarget,
                                        const nsACString& aContentType,
                                        bool aIsPrivate, JSContext* aCx,
                                        Promise** aPromise) {
  nsIGlobalObject* globalObject =
      xpc::NativeGlobal(JS::CurrentGlobalOrNull(aCx));

  if (NS_WARN_IF(!globalObject)) {
    return NS_ERROR_FAILURE;
  }

  ErrorResult result;
  RefPtr<Promise> promise = Promise::Create(globalObject, result);

  if (NS_WARN_IF(result.Failed())) {
    return result.StealNSResult();
  }

  nsresult rv = NS_OK;
  bool pendingAsyncOperations = false;

#if 0 || 0 || 0 || \
    defined(MOZ_WIDGET_GTK)

  nsAutoString path;
  if (aTarget && NS_SUCCEEDED(aTarget->GetPath(path))) {
#if 0 || defined(MOZ_WIDGET_GTK) || 0
    {
      bool addToRecentDocs = Preferences::GetBool(PREF_BDM_ADDTORECENTDOCS);
      if (addToRecentDocs && !aIsPrivate) {
#if defined(MOZ_WIDGET_GTK)
        GtkRecentManager* manager = gtk_recent_manager_get_default();

        gchar* uri = g_filename_to_uri(NS_ConvertUTF16toUTF8(path).get(),
                                       nullptr, nullptr);
        if (uri) {
          gtk_recent_manager_add_item(manager, uri);
          g_free(uri);
        }
#endif
      }
#if defined(MOZ_WIDGET_GTK)
      if (!aIsPrivate) {
        GFile* gio_file =
            g_file_new_for_path(NS_ConvertUTF16toUTF8(path).get());
        nsCString source_uri;
        nsresult rv = aSource->GetSpec(source_uri);
        NS_ENSURE_SUCCESS(rv, rv);
        GFileInfo* file_info = g_file_info_new();
        g_file_info_set_attribute_string(file_info, "metadata::download-uri",
                                         source_uri.get());
        g_file_set_attributes_async(gio_file, file_info, G_FILE_QUERY_INFO_NONE,
                                    G_PRIORITY_DEFAULT, nullptr,
                                    gio_set_metadata_done, nullptr);
        g_object_unref(file_info);
        g_object_unref(gio_file);
      }
#endif
    }
#endif

  }

#endif

  if (!pendingAsyncOperations) {
    promise->MaybeResolveWithUndefined();
  }
  promise.forget(aPromise);
  return rv;
}

nsresult DownloadPlatform::MaybeWriteDownloadOriginInformation(
    nsIFile* aTargetFile, nsIURI* aSourceUrl, nsIReferrerInfo* aReferrerInfo,
    bool aIsPrivate, JSContext* aCx, dom::Promise** aPromise) {
  NS_ENSURE_ARG(aCx);
  NS_ENSURE_ARG(aPromise);

  nsIGlobalObject* globalObject =
      xpc::NativeGlobal(JS::CurrentGlobalOrNull(aCx));
  if (NS_WARN_IF(!globalObject)) {
    return NS_ERROR_FAILURE;
  }

  mozilla::ErrorResult result;
  RefPtr domPromise = dom::Promise::Create(globalObject, result);
  if (NS_WARN_IF(result.Failed())) {
    return result.StealNSResult();
  }

  domPromise->MaybeResolve(false);

  domPromise.forget(aPromise);
  return NS_OK;
}

bool DownloadPlatform::IsURLPossiblyFromWeb(nsIURI* aURI) {
  nsCOMPtr<nsIIOService> ios = do_GetIOService();
  nsCOMPtr<nsIURI> uri = aURI;
  if (!ios) {
    return true;
  }

  while (uri) {
    uint32_t flags;
    nsresult rv = ios->GetDynamicProtocolFlags(uri, &flags);
    if (NS_FAILED(rv)) {
      return true;
    }
    if (!(flags & nsIProtocolHandler::URI_DANGEROUS_TO_LOAD) &&
        !(flags & nsIProtocolHandler::URI_IS_UI_RESOURCE) &&
        !(flags & nsIProtocolHandler::URI_IS_LOCAL_FILE)) {
      return true;
    }
    nsCOMPtr<nsINestedURI> nestedURI = do_QueryInterface(uri);
    uri = nullptr;
    if (nestedURI) {
      rv = nestedURI->GetInnerURI(getter_AddRefs(uri));
      if (NS_FAILED(rv)) {
        return true;
      }
    }
  }
  return false;
}
