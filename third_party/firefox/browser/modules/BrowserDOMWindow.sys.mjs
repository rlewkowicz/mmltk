/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { BrowserWindowTracker } from "resource:///modules/BrowserWindowTracker.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { PrivateBrowsingUtils } from "resource://gre/modules/PrivateBrowsingUtils.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

let lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  TaskbarTabsUtils: "resource:///modules/taskbartabs/TaskbarTabsUtils.sys.mjs",
  URILoadingHelper: "resource:///modules/URILoadingHelper.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "loadDivertedInBackground",
  "browser.tabs.loadDivertedInBackground"
);

ChromeUtils.defineLazyGetter(lazy, "ReferrerInfo", () =>
  Components.Constructor(
    "@mozilla.org/referrer-info;1",
    "nsIReferrerInfo",
    "init"
  )
);

export class BrowserDOMWindow {
  win = null;

  constructor(win) {
    this.win = win;
  }

  static setupInWindow(win) {
    win.browserDOMWindow = new BrowserDOMWindow(win);
  }

  static teardownInWindow(win) {
    win.browserDOMWindow = null;
  }

  #openURIInNewTab(
    aURI,
    aReferrerInfo,
    aIsPrivate,
    aIsExternal,
    aForceNotRemote = false,
    aUserContextId = Ci.nsIScriptSecurityManager.DEFAULT_USER_CONTEXT_ID,
    aOpenWindowInfo = null,
    aOpenerBrowser = null,
    aTriggeringPrincipal = null,
    aName = "",
    aPolicyContainer = null,
    aSkipLoad = false,
    aWhere = undefined
  ) {
    let win, needToFocusWin;

    if (
      this.win.toolbar.visible &&
      !lazy.TaskbarTabsUtils.isTaskbarTabWindow(this.win)
    ) {
      win = this.win;
    } else {
      win = BrowserWindowTracker.getTopWindow({ private: aIsPrivate });
      needToFocusWin = true;
    }

    if (!win) {
      return null;
    }

    if (aIsExternal && (!aURI || aURI.spec == "about:blank")) {
      win.BrowserCommands.openTab(); 
      win.focus();
      return win.gBrowser.selectedBrowser;
    }

    let loadInBackground;
    if (aWhere === Ci.nsIBrowserDOMWindow.OPEN_NEWTAB_BACKGROUND) {
      loadInBackground = true;
    } else if (aWhere === Ci.nsIBrowserDOMWindow.OPEN_NEWTAB_FOREGROUND) {
      loadInBackground = false;
    } else {
      loadInBackground = lazy.loadDivertedInBackground;
    }

    const uriString = aURI ? aURI.spec : "about:blank";
    const tabOptions = {
      triggeringPrincipal: aTriggeringPrincipal,
      referrerInfo: aReferrerInfo,
      userContextId: aUserContextId,
      fromExternal: aIsExternal,
      inBackground: loadInBackground,
      forceNotRemote: aForceNotRemote,
      openWindowInfo: aOpenWindowInfo,
      openerBrowser: aOpenerBrowser,
      name: aName,
      policyContainer: aPolicyContainer,
      skipLoad: aSkipLoad,
    };

    let tab;
    if (aWhere == Ci.nsIBrowserDOMWindow.OPEN_NEWTAB_AFTER_CURRENT) {
      tab = win.gBrowser.addAdjacentTab(
        win.gBrowser.selectedTab,
        uriString,
        tabOptions
      );
    } else {
      tab = win.gBrowser.addTab(uriString, tabOptions);
    }

    let browser = win.gBrowser.getBrowserForTab(tab);

    if (needToFocusWin || (!loadInBackground && aIsExternal)) {
      win.focus();
    }

    return browser;
  }

  createContentWindow(
    aURI,
    aOpenWindowInfo,
    aWhere,
    aFlags,
    aTriggeringPrincipal,
    aPolicyContainer
  ) {
    return this.#getContentWindowOrOpenURI(
      null,
      aOpenWindowInfo,
      aWhere,
      aFlags,
      aTriggeringPrincipal,
      aPolicyContainer,
      true
    );
  }

  openURI(
    aURI,
    aOpenWindowInfo,
    aWhere,
    aFlags,
    aTriggeringPrincipal,
    aPolicyContainer
  ) {
    if (!aURI) {
      console.error("openURI should only be called with a valid URI");
      throw Components.Exception("", Cr.NS_ERROR_FAILURE);
    }
    return this.#getContentWindowOrOpenURI(
      aURI,
      aOpenWindowInfo,
      aWhere,
      aFlags,
      aTriggeringPrincipal,
      aPolicyContainer,
      false
    );
  }

  #getContentWindowOrOpenURI(
    aURI,
    aOpenWindowInfo,
    aWhere,
    aFlags,
    aTriggeringPrincipal,
    aPolicyContainer,
    aSkipLoad
  ) {
    var browsingContext = null;
    var isExternal = !!(aFlags & Ci.nsIBrowserDOMWindow.OPEN_EXTERNAL);
    var guessUserContextIdEnabled =
      isExternal &&
      !Services.prefs.getBoolPref(
        "browser.link.force_default_user_context_id_for_external_opens",
        false
      );
    var openingUserContextId =
      (guessUserContextIdEnabled &&
        lazy.URILoadingHelper.guessUserContextId(aURI)) ||
      Ci.nsIScriptSecurityManager.DEFAULT_USER_CONTEXT_ID;

    if (aOpenWindowInfo && isExternal) {
      console.error(
        "BrowserDOMWindow.openURI did not expect aOpenWindowInfo to be " +
          "passed if the context is OPEN_EXTERNAL."
      );
      throw Components.Exception("", Cr.NS_ERROR_FAILURE);
    }

    if (isExternal && aURI && aURI.schemeIs("chrome")) {
      dump("use --chrome command-line option to load external chrome urls\n");
      return null;
    }

    if (aWhere == Ci.nsIBrowserDOMWindow.OPEN_DEFAULTWINDOW) {
      const externalLinkOpeningBehavior = isExternal
        ? Services.prefs.getIntPref(
            "browser.link.open_newwindow.override.external",
            -1
          )
        : -1;
      if (isExternal && externalLinkOpeningBehavior != -1) {
        aWhere = externalLinkOpeningBehavior;
      } else {
        aWhere = Services.prefs.getIntPref("browser.link.open_newwindow");
      }
    }

    let referrerInfo;
    if (aFlags & Ci.nsIBrowserDOMWindow.OPEN_NO_REFERRER) {
      referrerInfo = new lazy.ReferrerInfo(
        Ci.nsIReferrerInfo.EMPTY,
        false,
        null
      );
    } else if (
      aOpenWindowInfo &&
      aOpenWindowInfo.parent &&
      aOpenWindowInfo.parent.window
    ) {
      referrerInfo = new lazy.ReferrerInfo(
        aOpenWindowInfo.parent.window.document.referrerInfo.referrerPolicy,
        true,
        Services.io.newURI(aOpenWindowInfo.parent.window.location.href)
      );
    } else {
      referrerInfo = new lazy.ReferrerInfo(
        Ci.nsIReferrerInfo.EMPTY,
        true,
        null
      );
    }

    let isPrivate = aOpenWindowInfo
      ? aOpenWindowInfo.originAttributes.privateBrowsingId != 0
      : PrivateBrowsingUtils.isWindowPrivate(this.win);

    switch (aWhere) {
      case Ci.nsIBrowserDOMWindow.OPEN_NEWWINDOW: {
        var url = aURI && aURI.spec;
        let features = "all,dialog=no";
        if (isPrivate) {
          features += ",private";
        }
        try {
          let extraOptions = Cc[
            "@mozilla.org/hash-property-bag;1"
          ].createInstance(Ci.nsIWritablePropertyBag2);
          extraOptions.setPropertyAsBool("fromExternal", isExternal);

          this.win.openDialog(
            AppConstants.BROWSER_CHROME_URL,
            "_blank",
            features,
            url,
            extraOptions,
            null,
            null,
            null,
            null,
            null,
            null,
            aTriggeringPrincipal,
            null,
            aPolicyContainer,
            aOpenWindowInfo
          );
          browsingContext = null;
        } catch (ex) {
          console.error(ex);
        }
        break;
      }
      case Ci.nsIBrowserDOMWindow.OPEN_NEWTAB:
      case Ci.nsIBrowserDOMWindow.OPEN_NEWTAB_BACKGROUND:
      case Ci.nsIBrowserDOMWindow.OPEN_NEWTAB_FOREGROUND:
      case Ci.nsIBrowserDOMWindow.OPEN_NEWTAB_AFTER_CURRENT: {
        let forceNotRemote = aOpenWindowInfo && !aOpenWindowInfo.isRemote;
        let userContextId = aOpenWindowInfo
          ? aOpenWindowInfo.originAttributes.userContextId
          : openingUserContextId;
        let browser = this.#openURIInNewTab(
          aURI,
          referrerInfo,
          isPrivate,
          isExternal,
          forceNotRemote,
          userContextId,
          aOpenWindowInfo,
          aOpenWindowInfo?.parent?.top.embedderElement,
          aTriggeringPrincipal,
          "",
          aPolicyContainer,
          aSkipLoad,
          aWhere
        );
        if (browser) {
          browsingContext = browser.browsingContext;
        }
        break;
      }
      case Ci.nsIBrowserDOMWindow.OPEN_PRINT_BROWSER: {
        let browser =
          this.win.PrintUtils.handleStaticCloneCreatedForPrint(aOpenWindowInfo);
        if (browser) {
          browsingContext = browser.browsingContext;
        }
        break;
      }
      default:
        browsingContext = this.win.gBrowser.selectedBrowser.browsingContext;
        if (aURI) {
          let loadFlags = Ci.nsIWebNavigation.LOAD_FLAGS_NONE;
          if (isExternal) {
            loadFlags |= Ci.nsIWebNavigation.LOAD_FLAGS_FROM_EXTERNAL;
          } else if (!aTriggeringPrincipal.isSystemPrincipal) {
            loadFlags |= Ci.nsIWebNavigation.LOAD_FLAGS_FIRST_LOAD;
          }
          this.win.gBrowser.fixupAndLoadURIString(aURI.spec, {
            triggeringPrincipal: aTriggeringPrincipal,
            policyContainer: aPolicyContainer,
            loadFlags,
            referrerInfo,
          });
        }
        if (!lazy.loadDivertedInBackground) {
          this.win.focus();
        }
    }
    return browsingContext;
  }

  createContentWindowInFrame(aURI, aParams, aWhere, aFlags, aName) {
    return this.#getContentWindowOrOpenURIInFrame(
      null,
      aParams,
      aWhere,
      aFlags,
      aName,
      true
    );
  }

  openURIInFrame(aURI, aParams, aWhere, aFlags, aName) {
    return this.#getContentWindowOrOpenURIInFrame(
      aURI,
      aParams,
      aWhere,
      aFlags,
      aName,
      false
    );
  }

  #getContentWindowOrOpenURIInFrame(
    aURI,
    aParams,
    aWhere,
    aFlags,
    aName,
    aSkipLoad
  ) {
    if (aWhere == Ci.nsIBrowserDOMWindow.OPEN_PRINT_BROWSER) {
      return this.win.PrintUtils.handleStaticCloneCreatedForPrint(
        aParams.openWindowInfo
      );
    }

    if (
      aWhere != Ci.nsIBrowserDOMWindow.OPEN_NEWTAB &&
      aWhere != Ci.nsIBrowserDOMWindow.OPEN_NEWTAB_BACKGROUND &&
      aWhere != Ci.nsIBrowserDOMWindow.OPEN_NEWTAB_FOREGROUND
    ) {
      dump("Error: openURIInFrame can only open in new tabs or print");
      return null;
    }

    var isExternal = !!(aFlags & Ci.nsIBrowserDOMWindow.OPEN_EXTERNAL);

    var userContextId =
      aParams.openerOriginAttributes &&
      "userContextId" in aParams.openerOriginAttributes
        ? aParams.openerOriginAttributes.userContextId
        : Ci.nsIScriptSecurityManager.DEFAULT_USER_CONTEXT_ID;

    return this.#openURIInNewTab(
      aURI,
      aParams.referrerInfo,
      aParams.isPrivate,
      isExternal,
      false,
      userContextId,
      aParams.openWindowInfo,
      aParams.openerBrowser,
      aParams.triggeringPrincipal,
      aName,
      aParams.policyContainer,
      aSkipLoad,
      aWhere
    );
  }

  canClose() {
    return this.win.CanCloseWindow();
  }

  get tabCount() {
    return this.win.gBrowser.tabs.length;
  }
}

BrowserDOMWindow.prototype.QueryInterface = ChromeUtils.generateQI([
  "nsIBrowserDOMWindow",
]);
