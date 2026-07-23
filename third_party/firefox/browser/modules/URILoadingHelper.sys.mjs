/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { BrowserUtils } from "resource://gre/modules/BrowserUtils.sys.mjs";
import { PrivateBrowsingUtils } from "resource://gre/modules/PrivateBrowsingUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "ReferrerInfo", () =>
  Components.Constructor(
    "@mozilla.org/referrer-info;1",
    "nsIReferrerInfo",
    "init"
  )
);

function saveLink(window, url, params) {
  if ("isContentWindowPrivate" in params) {
    window.saveURL(
      url,
      null,
      null,
      null,
      true,
      true,
      params.referrerInfo,
      null,
      null,
      params.isContentWindowPrivate,
      params.originPrincipal
    );
  } else {
    if (!params.initiatingDoc) {
      console.error(
        "openUILink/openLinkIn was called with " +
          "where == 'save' but without initiatingDoc.  See bug 814264."
      );
      return;
    }
    window.saveURL(
      url,
      null,
      null,
      null,
      true,
      true,
      params.referrerInfo,
      null,
      params.initiatingDoc
    );
  }
}

function openInWindow(url, params, sourceWindow) {
  let {
    referrerInfo,
    forceNonPrivate,
    triggeringRemoteType,
    forceAllowDataURI,
    globalHistoryOptions,
    allowThirdPartyFixup,
    userContextId,
    postData,
    originPrincipal,
    originStoragePrincipal,
    triggeringPrincipal,
    policyContainer,
    resolveOnContentBrowserCreated,
    chromeless,
    width,
    height,
  } = params;
  const chromelessDimensions =
    chromeless && width && height ? `,width=${width},height=${height}` : "";
  const CHROMELESS_FEATURES = `resizable,minimizable,titlebar,close${chromelessDimensions}`;
  let features = `chrome,dialog=no,${chromeless ? CHROMELESS_FEATURES : "all"}`;
  if (params.private) {
    features += ",private";
    referrerInfo = new lazy.ReferrerInfo(
      referrerInfo.referrerPolicy,
      false,
      referrerInfo.originalReferrer
    );
  } else if (forceNonPrivate) {
    features += ",non-private";
  }

  var sa = Cc["@mozilla.org/array;1"].createInstance(Ci.nsIMutableArray);

  var wuri = Cc["@mozilla.org/supports-string;1"].createInstance(
    Ci.nsISupportsString
  );
  wuri.data = url;

  let extraOptions = Cc["@mozilla.org/hash-property-bag;1"].createInstance(
    Ci.nsIWritablePropertyBag2
  );
  if (triggeringRemoteType) {
    extraOptions.setPropertyAsACString(
      "triggeringRemoteType",
      triggeringRemoteType
    );
  }
  if (params.hasValidUserGestureActivation !== undefined) {
    extraOptions.setPropertyAsBool(
      "hasValidUserGestureActivation",
      params.hasValidUserGestureActivation
    );
  }
  if (params.textDirectiveUserActivation !== undefined) {
    extraOptions.setPropertyAsBool(
      "textDirectiveUserActivation",
      params.textDirectiveUserActivation
    );
  }
  if (forceAllowDataURI) {
    extraOptions.setPropertyAsBool("forceAllowDataURI", true);
  }
  if (params.fromExternal !== undefined) {
    extraOptions.setPropertyAsBool("fromExternal", params.fromExternal);
  }
  if (globalHistoryOptions?.triggeringSponsoredURL) {
    extraOptions.setPropertyAsACString(
      "triggeringSponsoredURL",
      globalHistoryOptions.triggeringSponsoredURL
    );
    if (globalHistoryOptions.triggeringSponsoredURLVisitTimeMS) {
      extraOptions.setPropertyAsUint64(
        "triggeringSponsoredURLVisitTimeMS",
        globalHistoryOptions.triggeringSponsoredURLVisitTimeMS
      );
    }
    if (globalHistoryOptions.triggeringSource) {
      extraOptions.setPropertyAsACString(
        "triggeringSource",
        globalHistoryOptions.triggeringSource
      );
    }
  }
  if (params.schemelessInput !== undefined) {
    extraOptions.setPropertyAsUint32("schemelessInput", params.schemelessInput);
  }
  var allowThirdPartyFixupSupports = Cc[
    "@mozilla.org/supports-PRBool;1"
  ].createInstance(Ci.nsISupportsPRBool);
  allowThirdPartyFixupSupports.data = allowThirdPartyFixup;

  var userContextIdSupports = Cc[
    "@mozilla.org/supports-PRUint32;1"
  ].createInstance(Ci.nsISupportsPRUint32);
  userContextIdSupports.data = userContextId;

  sa.appendElement(wuri);
  sa.appendElement(extraOptions);
  sa.appendElement(referrerInfo);
  sa.appendElement(postData);
  sa.appendElement(allowThirdPartyFixupSupports);
  sa.appendElement(userContextIdSupports);
  sa.appendElement(originPrincipal);
  sa.appendElement(originStoragePrincipal);
  sa.appendElement(triggeringPrincipal);
  sa.appendElement(null); 
  sa.appendElement(policyContainer);

  let win;

  function waitForWindowStartup() {
    return new Promise(resolve => {
      const delayedStartupObserver = aSubject => {
        if (aSubject == win) {
          Services.obs.removeObserver(
            delayedStartupObserver,
            "browser-delayed-startup-finished"
          );
          resolve();
        }
      };
      Services.obs.addObserver(
        delayedStartupObserver,
        "browser-delayed-startup-finished"
      );
    });
  }

  if (resolveOnContentBrowserCreated) {
    waitForWindowStartup().then(() =>
      resolveOnContentBrowserCreated(win.gBrowser.selectedBrowser)
    );
  }

  win = Services.ww.openWindow(
    sourceWindow,
    AppConstants.BROWSER_CHROME_URL,
    null,
    features,
    sa
  );
}

function openInCurrentTab(targetBrowser, url, uriObj, params) {
  let loadFlags = Ci.nsIWebNavigation.LOAD_FLAGS_NONE;

  if (params.allowThirdPartyFixup) {
    loadFlags |= Ci.nsIWebNavigation.LOAD_FLAGS_ALLOW_THIRD_PARTY_FIXUP;
    loadFlags |= Ci.nsIWebNavigation.LOAD_FLAGS_FIXUP_SCHEME_TYPOS;
  }
  if (!params.allowInheritPrincipal) {
    loadFlags |= Ci.nsIWebNavigation.LOAD_FLAGS_DISALLOW_INHERIT_PRINCIPAL;
  }

  if (params.allowPopups) {
    loadFlags |= Ci.nsIWebNavigation.LOAD_FLAGS_ALLOW_POPUPS;
  }
  if (params.indicateErrorPageLoad) {
    loadFlags |= Ci.nsIWebNavigation.LOAD_FLAGS_ERROR_LOAD_CHANGES_RV;
  }
  if (params.forceAllowDataURI) {
    loadFlags |= Ci.nsIWebNavigation.LOAD_FLAGS_FORCE_ALLOW_DATA_URI;
  }
  if (params.fromExternal) {
    loadFlags |= Ci.nsIWebNavigation.LOAD_FLAGS_FROM_EXTERNAL;
  }

  let { URI_INHERITS_SECURITY_CONTEXT } = Ci.nsIProtocolHandler;
  if (
    params.forceAboutBlankViewerInCurrent &&
    (!uriObj ||
      Services.io.getDynamicProtocolFlags(uriObj) &
        URI_INHERITS_SECURITY_CONTEXT)
  ) {
    targetBrowser.createAboutBlankDocumentViewer(
      params.originPrincipal,
      params.originStoragePrincipal
    );
  }

  let {
    triggeringPrincipal,
    policyContainer,
    referrerInfo,
    postData,
    userContextId,
    hasValidUserGestureActivation,
    textDirectiveUserActivation,
    globalHistoryOptions,
    triggeringRemoteType,
    schemelessInput,
  } = params;

  targetBrowser.fixupAndLoadURIString(url, {
    triggeringPrincipal,
    policyContainer,
    loadFlags,
    referrerInfo,
    postData,
    userContextId,
    hasValidUserGestureActivation,
    textDirectiveUserActivation,
    globalHistoryOptions,
    triggeringRemoteType,
    schemelessInput,
  });

  params.resolveOnContentBrowserCreated?.(targetBrowser);
}

function updatePrincipals(window, params) {
  let { userContextId } = params;
  function useOAForPrincipal(principal) {
    if (principal && principal.isContentPrincipal) {
      let privateBrowsingId =
        params.private ||
        (window && PrivateBrowsingUtils.isWindowPrivate(window));
      let attrs = {
        userContextId,
        privateBrowsingId,
        firstPartyDomain: principal.originAttributes.firstPartyDomain,
      };
      return Services.scriptSecurityManager.principalWithOA(principal, attrs);
    }
    return principal;
  }
  params.originPrincipal = useOAForPrincipal(params.originPrincipal);
  params.originStoragePrincipal = useOAForPrincipal(
    params.originStoragePrincipal
  );
  params.triggeringPrincipal = useOAForPrincipal(params.triggeringPrincipal);
}

function _createNullPrincipalFromTabUserContextId(tab = null) {
  const window = lazy.BrowserWindowTracker.getTopWindow();
  if (!tab) {
    tab = window.gBrowser.selectedTab;
  }

  let userContextId;
  if (tab.hasAttribute("usercontextid")) {
    userContextId = tab.getAttribute("usercontextid");
  }
  return Services.scriptSecurityManager.createNullPrincipal({
    userContextId,
  });
}

export const URILoadingHelper = {
  openLinkIn(window, url, where, params) {
    if (!where || !url) {
      return;
    }

    let {
      allowThirdPartyFixup,
      postData,
      charset,
      allowInheritPrincipal,
      forceAllowDataURI,
      forceNonPrivate,
      skipTabAnimation,
      allowPinnedTabHostChange,
      userContextId,
      triggeringPrincipal,
      originPrincipal,
      originStoragePrincipal,
      triggeringRemoteType,
      policyContainer,
      resolveOnNewTabCreated,
      resolveOnContentBrowserCreated,
      globalHistoryOptions,
      hasValidUserGestureActivation,
      textDirectiveUserActivation,
    } = params;

    params = Object.assign({}, params);

    if (!params.referrerInfo) {
      params.referrerInfo = new lazy.ReferrerInfo(
        Ci.nsIReferrerInfo.EMPTY,
        true,
        null
      );
    }

    if (!triggeringPrincipal) {
      throw new Error("Must load with a triggering Principal");
    }

    if (where == "save") {
      saveLink(window, url, params);
      return;
    }

    let w = this._resolveInitialTargetWindow(
      where,
      params,
      window,
      forceNonPrivate
    );

    updatePrincipals(w, params);

    if (where == "chromeless") {
      params.chromeless = true;
      where = "window";
    }

    if (!w || where == "window") {
      openInWindow(url, params, w || window);
      return;
    }


    w.focus();

    let targetBrowser;
    let uriObj;
    let loadInBackground = BrowserUtils.willLoadInBackground(where, params);

    if (where == "current") {
      targetBrowser = params.targetBrowser || w.gBrowser.selectedBrowser;
      uriObj = URL.parse(url)?.URI;

      let tab = w.gBrowser.getTabForBrowser(targetBrowser);
      if (
        params.userContextId != null &&
        params.userContextId !==
          targetBrowser.browsingContext.originAttributes.userContextId
      ) {
        where = "tab";
        targetBrowser = null;
      } else if (
        !allowPinnedTabHostChange &&
        tab.pinned &&
        url != "about:crashcontent"
      ) {
        try {
          if (
            !uriObj ||
            (!uriObj.schemeIs("javascript") &&
              targetBrowser.currentURI.host != uriObj.host)
          ) {
            where = "tab";
            targetBrowser = null;
          }
        } catch (err) {
          where = "tab";
          targetBrowser = null;
        }
      }
    }

    let focusUrlBar = false;

    switch (where) {
      case "current":
        openInCurrentTab(targetBrowser, url, uriObj, params);

        focusUrlBar =
          w.document.activeElement == w.gURLBar.inputField &&
          w.isBlankPageURL(url);
        break;
      case "tab":
      case "tabshifted": {
        focusUrlBar =
          !loadInBackground &&
          w.isBlankPageURL(url);

        let tabUsedForLoad = w.gBrowser.addTab(url, {
          referrerInfo: params.referrerInfo,
          charset,
          postData,
          inBackground: loadInBackground,
          allowThirdPartyFixup,
          relatedToCurrent: params.relatedToCurrent,
          skipAnimation: skipTabAnimation,
          userContextId,
          originPrincipal,
          originStoragePrincipal,
          triggeringPrincipal,
          allowInheritPrincipal,
          triggeringRemoteType,
          policyContainer,
          forceAllowDataURI,
          focusUrlBar,
          openerBrowser: params.openerBrowser,
          fromExternal: params.fromExternal,
          globalHistoryOptions,
          schemelessInput: params.schemelessInput,
          hasValidUserGestureActivation,
          textDirectiveUserActivation,
        });
        targetBrowser = tabUsedForLoad.linkedBrowser;

        resolveOnNewTabCreated?.(targetBrowser);
        resolveOnContentBrowserCreated?.(targetBrowser);

        break;
      }
    }

    if (
      !params.avoidBrowserFocus &&
      !focusUrlBar &&
      targetBrowser == w.gBrowser.selectedBrowser
    ) {
      targetBrowser.focus();
    }
  },
  _resolveInitialTargetWindow(where, params, win, forceNonPrivate) {
    if (where === "current" && params.targetBrowser) {
      return params.targetBrowser.documentGlobal;
    }

    if (where === "tab" || where === "tabshifted") {
      const target = this.getTargetWindow(win, {
        skipPopups: true,
        skipTaskbarTabs: true,
        forceNonPrivate,
      });
      if (win.top !== target) {
        params.relatedToCurrent = false;
      }
      return target;
    }
    return this.getTargetWindow(win, { forceNonPrivate });
  },
  getTargetWindow(
    window,
    { skipPopups, skipTaskbarTabs, forceNonPrivate } = {}
  ) {
    let { top } = window;
    if (
      top.document.documentElement.getAttribute("windowtype") ==
        "navigator:browser" &&
      (!skipPopups || top.toolbar.visible) &&
      (!skipTaskbarTabs ||
        !top.document.documentElement.hasAttribute("taskbartab")) &&
      (!forceNonPrivate || !PrivateBrowsingUtils.isWindowPrivate(top))
    ) {
      return top;
    }

    return lazy.BrowserWindowTracker.getTopWindow({
      private: !forceNonPrivate && PrivateBrowsingUtils.isWindowPrivate(window),
      allowPopups: !skipPopups,
      allowTaskbarTabs: !skipTaskbarTabs,
    });
  },

  openUILink(
    window,
    url,
    event,
    aIgnoreButton,
    aIgnoreAlt,
    aAllowThirdPartyFixup,
    aPostData,
    aReferrerInfo
  ) {
    event = BrowserUtils.getRootEvent(event);
    let params;

    if (aIgnoreButton && typeof aIgnoreButton == "object") {
      params = aIgnoreButton;

      aIgnoreButton = params.ignoreButton;
      aIgnoreAlt = params.ignoreAlt;
      delete params.ignoreButton;
      delete params.ignoreAlt;
    } else {
      params = {
        allowThirdPartyFixup: aAllowThirdPartyFixup,
        postData: aPostData,
        referrerInfo: aReferrerInfo,
        initiatingDoc: event ? event.target.ownerDocument : null,
      };
    }

    if (!params.triggeringPrincipal) {
      throw new Error(
        "Required argument triggeringPrincipal missing within openUILink"
      );
    }

    let where = BrowserUtils.whereToOpenLink(event, aIgnoreButton, aIgnoreAlt);
    params.forceForeground ??= true;
    this.openLinkIn(window, url, where, params);
  },

  openTrustedLinkIn(window, url, where, params = {}) {
    if (!params.triggeringPrincipal) {
      params.triggeringPrincipal =
        Services.scriptSecurityManager.getSystemPrincipal();
    }

    params.forceForeground ??= true;
    this.openLinkIn(window, url, where, params);
  },

  openWebLinkIn(window, url, where, params = {}) {
    if (!params.triggeringPrincipal) {
      params.triggeringPrincipal =
        Services.scriptSecurityManager.createNullPrincipal({});
    }
    if (params.triggeringPrincipal.isSystemPrincipal) {
      throw new Error(
        "System principal should never be passed into openWebLinkIn()"
      );
    }
    params.forceForeground ??= true;
    this.openLinkIn(window, url, where, params);
  },

  guessUserContextId(aURI) {
    let host;
    try {
      host = aURI.host;
    } catch (e) {}
    if (!host) {
      return null;
    }
    const containerScores = new Map();
    let guessedUserContextId = null;
    let maxCount = 0;
    for (let win of lazy.BrowserWindowTracker.orderedWindows) {
      for (let tab of win.gBrowser.visibleTabs) {
        let { userContextId } = tab;
        let currentURIHost = null;
        try {
          currentURIHost = tab.linkedBrowser.currentURI.host;
        } catch (e) {}

        if (currentURIHost == host) {
          let count = (containerScores.get(userContextId) ?? 0) + 1;
          containerScores.set(userContextId, count);
          if (count > maxCount) {
            guessedUserContextId = userContextId;
            maxCount = count;
          }
        }
      }
    }

    return guessedUserContextId;
  },
  switchToTabHavingURI(
    window,
    aURI,
    aOpenNew,
    aOpenParams = {},
    aUserContextId = null,
    aSplitView = null
  ) {
    const kPrivateBrowsingURLs = new Set(["about:addons"]);

    let ignoreFragment = aOpenParams.ignoreFragment;
    let ignoreQueryString = aOpenParams.ignoreQueryString;
    let replaceQueryString = aOpenParams.replaceQueryString;
    let adoptIntoActiveWindow = aOpenParams.adoptIntoActiveWindow;

    delete aOpenParams.ignoreFragment;
    delete aOpenParams.ignoreQueryString;
    delete aOpenParams.replaceQueryString;
    delete aOpenParams.adoptIntoActiveWindow;

    let isBrowserWindow = !!window.gBrowser;

    function switchIfURIInWindow(aWindow) {
      if (
        !kPrivateBrowsingURLs.has(aURI.spec) &&
        PrivateBrowsingUtils.isWindowPrivate(window) !==
          PrivateBrowsingUtils.isWindowPrivate(aWindow)
      ) {
        return false;
      }

      function cleanURL(url, removeQuery, removeFragment) {
        let ret = url;
        if (removeFragment) {
          ret = ret.split("#")[0];
          if (removeQuery) {
            ret = ret.split("?")[0];
          }
        } else if (removeQuery) {
          let fragment = ret.split("#")[1];
          ret = ret
            .split("?")[0]
            .concat(fragment != undefined ? "#".concat(fragment) : "");
        }
        return ret;
      }

      let ignoreFragmentWhenComparing =
        typeof ignoreFragment == "string" &&
        ignoreFragment.startsWith("whenComparing");
      let requestedCompare = cleanURL(
        aURI.displaySpec,
        ignoreQueryString || replaceQueryString,
        ignoreFragmentWhenComparing
      );
      let browsers = aWindow.gBrowser.browsers;
      for (let i = 0; i < browsers.length; i++) {
        let browser = browsers[i];
        let browserCompare = cleanURL(
          browser.currentURI.displaySpec,
          ignoreQueryString || replaceQueryString,
          ignoreFragmentWhenComparing
        );
        let browserUserContextId = browser.getAttribute("usercontextid") || "";
        if (aUserContextId != null && aUserContextId != browserUserContextId) {
          continue;
        }
        if (requestedCompare == browserCompare) {
          let doAdopt =
            adoptIntoActiveWindow && isBrowserWindow && aWindow != window;

          if (doAdopt) {
            const newTab = window.gBrowser.adoptTab(
              aWindow.gBrowser.getTabForBrowser(browser),
              {
                tabIndex: window.gBrowser.tabContainer.selectedIndex + 1,
                selectTab: true,
              }
            );
            if (!newTab) {
              doAdopt = false;
            }
          }
          if (!doAdopt) {
            aWindow.focus();
          }

          if (
            ignoreFragment == "whenComparingAndReplace" ||
            replaceQueryString
          ) {
            browser.loadURI(aURI, {
              triggeringPrincipal:
                aOpenParams.triggeringPrincipal ||
                _createNullPrincipalFromTabUserContextId(),
            });
          }

          if (!doAdopt) {
            if (aSplitView) {
              let tabToMove = aWindow.gBrowser.tabs[i];
              if (aSplitView.tabs.includes(tabToMove)) {
                aWindow.gBrowser.selectedTab = tabToMove;
              } else {
                let tabToReplace = aSplitView.tabs.find(tab => tab.selected);
                aSplitView.replaceTab(tabToReplace, tabToMove);
              }
              aSplitView.documentGlobal.focus();
            } else {
              aWindow.gBrowser.tabContainer.selectedIndex = i;
            }
          }

          return true;
        }
      }
      return false;
    }

    if (!(aURI instanceof Ci.nsIURI)) {
      aURI = Services.io.newURI(aURI);
    }

    if (isBrowserWindow && switchIfURIInWindow(window)) {
      return true;
    }

    for (let browserWin of lazy.BrowserWindowTracker.orderedWindows) {
      if (browserWin.closed || browserWin == window) {
        continue;
      }
      if (switchIfURIInWindow(browserWin)) {
        return true;
      }
    }

    if (aOpenNew) {
      if (aUserContextId != null) {
        aOpenParams.userContextId = aUserContextId;
      }
      if (isBrowserWindow && window.gBrowser.selectedTab.isEmpty) {
        this.openTrustedLinkIn(window, aURI.spec, "current", aOpenParams);
      } else {
        this.openTrustedLinkIn(window, aURI.spec, "tab", aOpenParams);
      }
    }

    return false;
  },
};
