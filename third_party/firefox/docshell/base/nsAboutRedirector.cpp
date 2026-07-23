/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsAboutRedirector.h"
#include "nsNetUtil.h"
#include "nsAboutProtocolUtils.h"
#include "nsIProtocolHandler.h"
#include "mozilla/Preferences.h"

#define ABOUT_CONFIG_ENABLED_PREF "general.aboutConfig.enable"

NS_IMPL_ISUPPORTS(nsAboutRedirector, nsIAboutModule)

struct RedirEntry {
  const char* id;
  const char* url;
  uint32_t flags;
};

static const RedirEntry kRedirMap[] = {
    {"about", "chrome://global/content/aboutAbout.html", 0},
    {"buildconfig", "chrome://global/content/buildconfig.html",
     nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
         nsIAboutModule::IS_SECURE_CHROME_UI},
    {"checkerboard", "chrome://global/content/aboutCheckerboard.html",
     nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
         nsIAboutModule::ALLOW_SCRIPT},
    {"config", "chrome://global/content/aboutconfig/aboutconfig.html",
     nsIAboutModule::IS_SECURE_CHROME_UI},
    {"credits", "https://www.mozilla.org/credits/",
     nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
         nsIAboutModule::URI_MUST_LOAD_IN_CHILD},
    {"httpsonlyerror", "chrome://global/content/httpsonlyerror/errorpage.html",
     nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
         nsIAboutModule::URI_CAN_LOAD_IN_CHILD | nsIAboutModule::ALLOW_SCRIPT |
         nsIAboutModule::HIDE_FROM_ABOUTABOUT},
    {"logo", "chrome://branding/content/about.png",
     nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT},
    {"memory", "chrome://global/content/aboutMemory.xhtml",
     nsIAboutModule::ALLOW_SCRIPT},
    {"mozilla", "chrome://global/content/mozilla.html",
     nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT},
    {"webauthn", "chrome://global/content/aboutWebauthn.html",
     nsIAboutModule::ALLOW_SCRIPT | nsIAboutModule::IS_SECURE_CHROME_UI},
    {"neterror", "chrome://global/content/aboutNetError.html",
     nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
         nsIAboutModule::URI_CAN_LOAD_IN_CHILD | nsIAboutModule::ALLOW_SCRIPT |
         nsIAboutModule::HIDE_FROM_ABOUTABOUT},
    {"networking", "chrome://global/content/aboutNetworking.html",
     nsIAboutModule::ALLOW_SCRIPT},
    {"pdf", "chrome://global/content/aboutPDF.html",
     nsIAboutModule::ALLOW_SCRIPT |
         nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
         nsIAboutModule::URI_MUST_LOAD_IN_CHILD |
         nsIAboutModule::URI_CAN_LOAD_IN_PRIVILEGEDABOUT_PROCESS |
         nsIAboutModule::IS_SECURE_CHROME_UI},
    {"performance", "about:processes",
     nsIAboutModule::ALLOW_SCRIPT | nsIAboutModule::IS_SECURE_CHROME_UI |
         nsIAboutModule::HIDE_FROM_ABOUTABOUT},
    {"processes", "chrome://global/content/aboutProcesses.html",
     nsIAboutModule::ALLOW_SCRIPT | nsIAboutModule::IS_SECURE_CHROME_UI},
    {"restricted",
     "chrome://global/content/aboutRestricted/aboutRestricted.html",
     nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
         nsIAboutModule::URI_CAN_LOAD_IN_CHILD | nsIAboutModule::ALLOW_SCRIPT |
         nsIAboutModule::HIDE_FROM_ABOUTABOUT},
    {"serviceworkers", "chrome://global/content/aboutServiceWorkers.xhtml",
     nsIAboutModule::ALLOW_SCRIPT},
    {"profiles", "chrome://global/content/aboutProfiles.xhtml",
     nsIAboutModule::ALLOW_SCRIPT | nsIAboutModule::IS_SECURE_CHROME_UI},
    {"srcdoc", "about:blank",
     nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
         nsIAboutModule::HIDE_FROM_ABOUTABOUT |
         nsIAboutModule::MAKE_LINKABLE | nsIAboutModule::URI_CAN_LOAD_IN_CHILD},
    {"support", "chrome://global/content/aboutSupport.xhtml",
     nsIAboutModule::ALLOW_SCRIPT | nsIAboutModule::IS_SECURE_CHROME_UI},
};

NS_IMETHODIMP
nsAboutRedirector::NewChannel(nsIURI* aURI, nsILoadInfo* aLoadInfo,
                              nsIChannel** aResult) {
  NS_ENSURE_ARG_POINTER(aURI);
  NS_ENSURE_ARG_POINTER(aLoadInfo);
  NS_ASSERTION(aResult, "must not be null");

  nsAutoCString path;
  nsresult rv = NS_GetAboutModuleName(aURI, path);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIIOService> ioService = do_GetIOService(&rv);
  NS_ENSURE_SUCCESS(rv, rv);

  if (path.EqualsASCII("config") &&
      !mozilla::Preferences::GetBool(ABOUT_CONFIG_ENABLED_PREF, true)) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  for (const auto& redir : kRedirMap) {
    if (!strcmp(path.get(), redir.id)) {
      nsCOMPtr<nsIChannel> tempChannel;
      nsCOMPtr<nsIURI> tempURI;
      rv = NS_NewURI(getter_AddRefs(tempURI), redir.url);
      NS_ENSURE_SUCCESS(rv, rv);

      rv = NS_NewChannelInternal(getter_AddRefs(tempChannel), tempURI,
                                 aLoadInfo);
      NS_ENSURE_SUCCESS(rv, rv);

      bool isUIResource = false;
      rv = NS_URIChainHasFlags(tempURI, nsIProtocolHandler::URI_IS_UI_RESOURCE,
                               &isUIResource);
      NS_ENSURE_SUCCESS(rv, rv);

      bool isAboutBlank = NS_IsAboutBlank(tempURI);

      if (!isUIResource && !isAboutBlank) {
        aLoadInfo->SetResultPrincipalURI(tempURI);
      }

      tempChannel->SetOriginalURI(aURI);

      tempChannel.forget(aResult);
      return rv;
    }
  }

  NS_ERROR("nsAboutRedirector called for unknown case");
  return NS_ERROR_ILLEGAL_VALUE;
}

NS_IMETHODIMP
nsAboutRedirector::GetURIFlags(nsIURI* aURI, uint32_t* aResult) {
  NS_ENSURE_ARG_POINTER(aURI);

  nsAutoCString name;
  nsresult rv = NS_GetAboutModuleName(aURI, name);
  NS_ENSURE_SUCCESS(rv, rv);

  for (const auto& redir : kRedirMap) {
    if (name.EqualsASCII(redir.id)) {
      *aResult = redir.flags;
      return NS_OK;
    }
  }

  NS_ERROR("nsAboutRedirector called for unknown case");
  return NS_ERROR_ILLEGAL_VALUE;
}

NS_IMETHODIMP
nsAboutRedirector::GetChromeURI(nsIURI* aURI, nsIURI** chromeURI) {
  NS_ENSURE_ARG_POINTER(aURI);

  nsAutoCString name;
  nsresult rv = NS_GetAboutModuleName(aURI, name);
  NS_ENSURE_SUCCESS(rv, rv);

  for (const auto& redir : kRedirMap) {
    if (name.EqualsASCII(redir.id)) {
      return NS_NewURI(chromeURI, redir.url);
    }
  }

  NS_ERROR("nsAboutRedirector called for unknown case");
  return NS_ERROR_ILLEGAL_VALUE;
}

nsresult nsAboutRedirector::Create(REFNSIID aIID, void** aResult) {
  RefPtr about = mozilla::MakeRefPtr<nsAboutRedirector>();
  return about->QueryInterface(aIID, aResult);
}
