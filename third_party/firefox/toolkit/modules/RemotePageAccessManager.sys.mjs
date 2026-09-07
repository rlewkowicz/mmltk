/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export let RemotePageAccessManager = {
  accessMap: {
    "about:certerror": {
      RPMSendAsyncMessage: [
        "Browser:EnableOnlineMode",
        "Browser:ResetSSLPreferences",
        "GetChangedCertPrefs",
        "Browser:OpenCaptivePortalPage",
        "Browser:SSLErrorGoBack",
        "Browser:PrimeMitm",
        "Browser:ResetEnterpriseRootsPref",
        "DisplayOfflineSupportPage",
      ],
      RPMRecordGleanEvent: ["*"],
      RPMAddMessageListener: ["*"],
      RPMRemoveMessageListener: ["*"],
      RPMGetFormatURLPref: ["app.support.baseURL"],
      RPMGetBoolPref: [
        "security.certerrors.mitm.priming.enabled",
        "security.certerrors.permanentOverride",
        "security.enterprise_roots.auto-enabled",
        "security.certerror.hideAddException",
        "security.certerrors.felt-privacy-v1",
        "browser.ipProtection.userEnabled",
      ],
      RPMGetIntPref: [
        "security.dialog_enable_delay",
        "services.settings.clock_skew_seconds",
        "services.settings.last_update_seconds",
      ],
      RPMGetAppBuildID: ["*"],
      RPMGetHostForDisplay: ["*"],
      RPMGetInnermostAsciiHost: ["*"],
      RPMIsWindowPrivate: ["*"],
    },
    "about:home": {
      RPMSendAsyncMessage: ["ActivityStream:ContentToMain"],
      RPMAddMessageListener: ["ActivityStream:MainToContent"],
      RPMGetFormatURLPref: ["app.support.baseURL"],
    },
    "about:httpsonlyerror": {
      RPMGetFormatURLPref: ["app.support.baseURL"],
      RPMGetIntPref: ["security.dialog_enable_delay"],
      RPMSendAsyncMessage: ["goBack", "openInsecure"],
      RPMAddMessageListener: ["WWWReachable"],
      RPMTryPingSecureWWWLink: ["*"],
      RPMOpenSecureWWWLink: ["*"],
    },
    "about:pdf": {
      RPMCanSetDefaultPDFHandler: ["*"],
      RPMGetBoolPref: ["browser.aboutpdf.promo.dismissed"],
      RPMOpenPDFFile: ["*"],
      RPMSetDefaultPDFHandler: ["*"],
      RPMSetPref: ["browser.aboutpdf.promo.dismissed"],
    },
    "about:keyboard": {
      RPMAddMessageListener: ["CustomKeys:CapturedKey"],
      RPMGetFormatURLPref: ["app.support.baseURL"],
      RPMSendAsyncMessage: ["CustomKeys:CaptureKey"],
      RPMSendQuery: [
        "CustomKeys:ChangeKey",
        "CustomKeys:ClearKey",
        "CustomKeys:Confirm",
        "CustomKeys:GetDefaultKey",
        "CustomKeys:GetKeys",
        "CustomKeys:ResetAll",
        "CustomKeys:ResetKey",
      ],
    },
    "about:neterror": {
      RPMSendAsyncMessage: [
        "Browser:EnableOnlineMode",
        "Browser:ResetSSLPreferences",
        "GetChangedCertPrefs",
        "Browser:OpenCaptivePortalPage",
        "Browser:SSLErrorGoBack",
        "Browser:PrimeMitm",
        "Browser:ResetEnterpriseRootsPref",
        "DisplayOfflineSupportPage",
        "OpenTRRPreferences",
      ],
      RPMCheckAlternateHostAvailable: ["*"],
      RPMRecordGleanEvent: [
        "securityDohNeterror",
        "securityUiTlserror",
        "securityUiCerterror",
        "securityUiNeterror",
      ],
      RPMAddMessageListener: ["*"],
      RPMRemoveMessageListener: ["*"],
      RPMGetFormatURLPref: [
        "app.support.baseURL",
        "network.trr_ui.skip_reason_learn_more_url",
      ],
      RPMGetBoolPref: [
        "security.certerrors.mitm.priming.enabled",
        "security.certerrors.permanentOverride",
        "security.enterprise_roots.auto-enabled",
        "security.certerror.hideAddException",
        "security.certerrors.felt-privacy-v1",
        "browser.ipProtection.userEnabled",
      ],
      RPMGetHostForDisplay: ["*"],
      RPMGetInnermostAsciiHost: ["*"],
      RPMGetHttpResponseHeader: ["*"],
      RPMIsTRROnlyFailure: ["*"],
      RPMIsFirefox: ["*"],
      RPMHasConnectivity: ["*"],
      RPMGetTRRSkipReason: ["*"],
      RPMGetTRRDomain: ["*"],
      RPMIsSiteSpecificTRRError: ["*"],
      RPMSetTRRDisabledLoadFlags: ["*"],
      RPMShowOSXLocalNetworkPermissionWarning: ["*"],
      RPMSendQuery: ["Browser:AddTRRExcludedDomain"],
      RPMGetIntPref: ["network.trr.mode", "security.dialog_enable_delay"],
    },
    "about:newtab": {
      RPMSendAsyncMessage: ["ActivityStream:ContentToMain"],
      RPMAddMessageListener: ["ActivityStream:MainToContent"],
      RPMGetFormatURLPref: ["app.support.baseURL"],
    },
    "about:privatebrowsing": {
      RPMSendAsyncMessage: [
        "OpenPrivateWindow",
        "SearchBannerDismissed",
        "OpenSearchPreferences",
        "SearchHandoff",
      ],
      RPMSendQuery: [
        "IsPromoBlocked",
        "ShouldShowSearchBanner",
        "ShouldShowPromo",
        "SpecialMessageActionDispatch",
      ],
      RPMAddMessageListener: ["*"],
      RPMRemoveMessageListener: ["*"],
      RPMGetFormatURLPref: ["app.support.baseURL"],
      RPMIsWindowPrivate: ["*"],
      RPMGetBoolPref: ["browser.nova.enabled"],
    },
    "about:deleteprofile": {
      RPMSendQuery: ["Profiles:GetDeleteProfileContent"],
      RPMSendAsyncMessage: ["Profiles:CancelDelete", "Profiles:DeleteProfile"],
    },
    "about:editprofile": {
      RPMSendQuery: [
        "Profiles:GetEditProfileContent",
        "Profiles:UpdateProfileTheme",
        "Profiles:UpdateProfileAvatar",
        "Profiles:SetDesktopShortcut",
      ],
      RPMSendAsyncMessage: [
        "Profiles:UpdateProfileName",
        "Profiles:OpenDeletePage",
        "Profiles:CloseProfileTab",
        "Profiles:MoreThemes",
        "Profiles:PageHide",
      ],
    },
    "about:newprofile": {
      RPMSendQuery: [
        "Profiles:GetNewProfileContent",
        "Profiles:UpdateProfileTheme",
        "Profiles:UpdateProfileAvatar",
      ],
      RPMSendAsyncMessage: [
        "Profiles:UpdateProfileName",
        "Profiles:DeleteProfile",
        "Profiles:CloseProfileTab",
        "Profiles:MoreThemes",
        "Profiles:PageHide",
      ],
      RPMGetBoolPref: ["browser.profiles.profile-name.updated"],
      RPMGetFormatURLPref: ["app.support.baseURL"],
    },
    "about:protections": {
      RPMSendAsyncMessage: [
        "OpenContentBlockingPreferences",
        "OpenAboutLogins",
        "OpenSyncPreferences",
        "ClearMonitorCache",
        "RecordEntryPoint",
      ],
      RPMSendQuery: [
        "FetchUserLoginsData",
        "FetchMonitorData",
        "FetchContentBlockingEvents",
        "FetchMobileDeviceConnected",
        "FetchEntryPoint",
        "FetchVPNSubStatus",
        "FetchShowVPNCard",
        "FetchPrivacyMetrics",
      ],
      RPMAddMessageListener: ["*"],
      RPMRemoveMessageListener: ["*"],
      RPMSetPref: [
        "browser.contentblocking.report.show_mobile_app",
        "browser.contentblocking.report.hide_vpn_banner",
      ],
      RPMGetBoolPref: [
        "browser.contentblocking.report.lockwise.enabled",
        "browser.contentblocking.report.monitor.enabled",
        "privacy.fingerprintingProtection",
        "privacy.socialtracking.block_cookies.enabled",
        "browser.contentblocking.report.privacy_metrics.enabled",
        "privacy.trackingprotection.cryptomining.enabled",
        "privacy.trackingprotection.fingerprinting.enabled",
        "privacy.trackingprotection.harmfuladdon.enabled",
        "privacy.trackingprotection.enabled",
        "privacy.trackingprotection.socialtracking.enabled",
        "browser.contentblocking.report.show_mobile_app",
        "browser.contentblocking.report.hide_vpn_banner",
        "browser.vpn_promo.enabled",
      ],
      RPMGetStringPref: [
        "browser.contentblocking.category",
        "browser.contentblocking.report.monitor.url",
        "browser.contentblocking.report.monitor.sign_in_url",
        "browser.contentblocking.report.manage_devices.url",
        "browser.contentblocking.report.lockwise.mobile-android.url",
        "browser.contentblocking.report.lockwise.mobile-ios.url",
        "browser.contentblocking.report.mobile-ios.url",
        "browser.contentblocking.report.mobile-android.url",
        "browser.contentblocking.report.vpn.url",
        "browser.contentblocking.report.vpn-promo.url",
        "browser.contentblocking.report.vpn-android.url",
        "browser.contentblocking.report.vpn-ios.url",
      ],
      RPMGetIntPref: ["network.cookie.cookieBehavior"],
      RPMGetFormatURLPref: [
        "browser.contentblocking.report.monitor.how_it_works.url",
        "browser.contentblocking.report.lockwise.how_it_works.url",
        "browser.contentblocking.report.monitor.preferences_url",
        "browser.contentblocking.report.monitor.home_page_url",
        "browser.contentblocking.report.social.url",
        "browser.contentblocking.report.cookie.url",
        "browser.contentblocking.report.tracker.url",
        "browser.contentblocking.report.fingerprinter.url",
        "browser.contentblocking.report.cryptominer.url",
      ],
      RPMRecordGleanEvent: ["securityUiProtections"],
    },
    "about:restricted": {
      RPMSendAsyncMessage: ["goBack"],
      RPMGetBoolPref: ["security.restrict_to_adults.always"],
    },
    "about:welcome": {
      RPMSendAsyncMessage: ["ActivityStream:ContentToMain"],
      RPMAddMessageListener: ["ActivityStream:MainToContent"],
    },
  },

  checkAllowAccess(aDocument, aFeature, aValue) {
    let principal = aDocument.nodePrincipal;
    if (!principal) {
      return false;
    }

    return this.checkAllowAccessWithPrincipal(
      principal,
      aFeature,
      aValue,
      aDocument
    );
  },

  checkAllowAccessWithPrincipal(aPrincipal, aFeature, aValue, aDocument) {
    let accessMapForFeature = this.checkAllowAccessToFeature(
      aPrincipal,
      aFeature,
      aDocument
    );
    if (!accessMapForFeature) {
      console.error(
        "RemotePageAccessManager does not allow access to Feature: ",
        aFeature,
        " for: ",
        aDocument.location
      );

      return false;
    }

    if (accessMapForFeature.includes(aValue) || accessMapForFeature[0] == "*") {
      return true;
    }

    return false;
  },

  checkAllowAccessToFeature(aPrincipal, aFeature, aDocument) {
    let spec;
    if (!aPrincipal.isContentPrincipal) {
      if (!aDocument) {
        return null;
      }
      if (!aDocument.documentURIObject.schemeIs("about")) {
        return null;
      }
      spec =
        aDocument.documentURIObject.prePath +
        aDocument.documentURIObject.filePath;
    } else {
      if (!aPrincipal.schemeIs("about")) {
        return null;
      }
      spec = aPrincipal.prePath + aPrincipal.filePath;
    }

    let accessMapForURI = this.accessMap[spec];
    if (!accessMapForURI) {
      return null;
    }

    return accessMapForURI[aFeature];
  },

  addPage(aUrl, aFunctionMap) {
    if (!false) {
      throw new Error("Cannot only modify privileges during testing");
    }

    if (aUrl in this.accessMap) {
      throw new Error("Cannot modify privileges of existing page");
    }

    this.accessMap[aUrl] = aFunctionMap;
  },
};
