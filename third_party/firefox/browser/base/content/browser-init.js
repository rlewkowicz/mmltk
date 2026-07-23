/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

let _resolveDelayedStartup;
var delayedStartupPromise = new Promise(resolve => {
  _resolveDelayedStartup = resolve;
});

var gBrowserInit = {
  delayedStartupFinished: false,
  domContentLoaded: false,

  _tabToAdopt: undefined,
  _firstContentWindowPaintDeferred: Promise.withResolvers(),
  idleTasksFinished: Promise.withResolvers(),
  _reducedProtectionPrefObserver: null,

  _setupFirstContentWindowPaintPromise() {
    let lastTransactionId = window.windowUtils.lastTransactionId;
    let layerTreeListener = () => {
      if (this.getTabToAdopt()) {
        return;
      }
      removeEventListener("MozLayerTreeReady", layerTreeListener);
      let listener = e => {
        if (e.transactionId > lastTransactionId) {
          window.removeEventListener("MozAfterPaint", listener);
          this._firstContentWindowPaintDeferred.resolve();
        }
      };
      addEventListener("MozAfterPaint", listener);
    };
    addEventListener("MozLayerTreeReady", layerTreeListener);
  },

  getTabToAdopt() {
    if (this._tabToAdopt !== undefined) {
      return this._tabToAdopt;
    }

    if (window.arguments && window.XULElement.isInstance(window.arguments[0])) {
      this._tabToAdopt = window.arguments[0];

      window.arguments[0] = null;
    } else {
      this._tabToAdopt = null;
    }

    return this._tabToAdopt;
  },

  _clearTabToAdopt() {
    this._tabToAdopt = null;
  },

  isAdoptingTab() {
    return !!this.getTabToAdopt();
  },

  onBeforeInitialXULLayout() {
    this._setupFirstContentWindowPaintPromise();

    if (ChromeUtils.shouldResistFingerprinting("RoundWindowSize", null)) {
      document.documentElement.setAttribute("sizemode", "normal");
    } else if (!document.documentElement.hasAttribute("width")) {
      const TARGET_WIDTH = 1280;
      const TARGET_HEIGHT = 1040;
      let width = Math.min(screen.availWidth * 0.9, TARGET_WIDTH);
      let height = Math.min(screen.availHeight * 0.9, TARGET_HEIGHT);

      document.documentElement.setAttribute("width", width);
      document.documentElement.setAttribute("height", height);

      if (width < TARGET_WIDTH && height < TARGET_HEIGHT) {
        document.documentElement.setAttribute("sizemode", "maximized");
      }
    }
    {
      const toolbarMenubar = document.getElementById("toolbar-menubar");
      const nativeMenubar = Services.appinfo.nativeMenubar;
      toolbarMenubar.collapsed = nativeMenubar;
      if (nativeMenubar) {
        toolbarMenubar.removeAttribute("autohide");
      } else {
        document.l10n.setAttributes(
          toolbarMenubar,
          "toolbar-context-menu-menu-bar-cmd"
        );
        toolbarMenubar.setAttribute("data-l10n-attrs", "toolbarname");
      }
    }
    BrowserUtils.callModulesFromCategory(
      {
        categoryName:
          "browser-window-before-initial-xul-layout-document-preparation",
        jsGlobal: globalThis,
      },
      window
    );

    window.TabBarVisibility.update();

    if (
      Services.prefs.getBoolPref(
        "toolkit.legacyUserProfileCustomizations.windowIcon",
        false
      )
    ) {
      document.documentElement.setAttribute("icon", "main-window");
    }

    BrowserUtils.callModulesFromCategory(
      {
        categoryName: "browser-window-before-initial-xul-layout",
        jsGlobal: globalThis,
      },
      window
    );
  },

  onDOMContentLoaded() {
    window.docShell.treeOwner
      .QueryInterface(Ci.nsIInterfaceRequestor)
      .getInterface(Ci.nsIAppWindow).XULBrowserWindow = window.XULBrowserWindow;
    BrowserUtils.callModulesFromCategory(
      { categoryName: "browser-window-domcontentloaded-before-tabbrowser" },
      window
    );

    BrowserUtils.callModulesFromCategory(
      {
        categoryName: "browser-window-domcontentloaded-tabbrowser",
        jsGlobal: globalThis,
      },
      window
    );
    gURLBar.addGBrowserListeners();
    if (Services.prefs.getBoolPref("browser.search.widget.new", false)) {
      document.getElementById("searchbar-new")?.addGBrowserListeners();
    }

    BrowserUtils.callModulesFromCategory(
      {
        categoryName: "browser-window-domcontentloaded",
        jsGlobal: globalThis,
      },
      window
    );

    gURLBar.initPlaceHolder();

    this._callWithURIToLoad(uriToLoad => {
      let url = URL.parse(uriToLoad);
      if (!url) {
        return;
      }
      let nonQuery = url.URI.prePath + url.pathname;
      if (nonQuery in gPageIcons) {
        gBrowser.setIcon(gBrowser.selectedTab, gPageIcons[nonQuery]);
      }
    });

    this._setInitialFocus();

    this.domContentLoaded = true;
  },

  onLoad() {
    gBrowser.addEventListener("DOMUpdateBlockedPopups", e =>
      PopupAndRedirectBlockerObserver.handleEvent(e)
    );
    gBrowser.addEventListener("DOMUpdateBlockedRedirect", e =>
      PopupAndRedirectBlockerObserver.handleEvent(e)
    );
    window.addEventListener("AppCommand", HandleAppCommandEvent, true);

    if (!gMultiProcessBrowser) {
      gBrowser.tabpanels.addEventListener("click", contentAreaClick, {
        capture: true,
        mozSystemGroup: true,
      });
    }

    gBrowser.addProgressListener(window.XULBrowserWindow);
    gBrowser.addTabsProgressListener(window.TabsProgressListener);

    BrowserUtils.callModulesFromCategory(
      {
        categoryName: "browser-window-load-before-sessionstore-init",
        jsGlobal: globalThis,
      },
      window
    );

    Services.obs.notifyObservers(window, "browser-window-before-show");

    if (!window.toolbar.visible) {
      gURLBar.readOnly = true;
    }

    BrowserUtils.callModulesFromCategory(
      { categoryName: "browser-window-load", jsGlobal: globalThis },
      window
    );


    let tabToAdopt = this.getTabToAdopt();
    if (tabToAdopt) {
      let evt = new CustomEvent("before-initial-tab-adopted", {
        bubbles: true,
      });
      gBrowser.tabpanels.dispatchEvent(evt);

      gBrowser.stop();

      gURLBar.removeAttribute("focused");

      let swapBrowsers = () => {
        if (gBrowser.isTabGroupLabel(tabToAdopt)) {
          gBrowser.adoptTabGroup(tabToAdopt.group, { elementIndex: 0 });
          gBrowser.removeTab(gBrowser.selectedTab);
        } else if (gBrowser.isTabGroup(tabToAdopt)) {
          let tempBlankTab = gBrowser.selectedTab;
          gBrowser.adoptTabGroup(tabToAdopt, { tabIndex: 0, selectTab: true });
          gBrowser.removeTab(tempBlankTab);
        } else if (gBrowser.isSplitViewWrapper(tabToAdopt)) {
          let tempBlankTab = gBrowser.selectedTab;
          let splitview = gBrowser.adoptSplitView(tabToAdopt, {
            elementIndex: 0,
            selectTab: true,
          });
          if (gBrowser.selectedTabs.length > 1) {
            gBrowser.addRangeToMultiSelectedTabs(
              splitview.tabs[0],
              splitview.tabs[splitview.tabs.length - 1]
            );
          }
          gBrowser.removeTab(tempBlankTab);
        } else {
          gBrowser.swapBrowsersAndCloseOther(gBrowser.selectedTab, tabToAdopt);
        }

        this._clearTabToAdopt();
      };
      if (
        gBrowser.isTab(tabToAdopt) &&
        !tabToAdopt.linkedBrowser.isRemoteBrowser
      ) {
        swapBrowsers();
      } else {
        addEventListener("MozAfterPaint", swapBrowsers, { once: true });
      }
    }

    this._boundDelayedStartup = this._delayedStartup.bind(this);
    window.addEventListener("MozAfterPaint", this._boundDelayedStartup);

    if (!PrivateBrowsingUtils.enabled) {
      document.getElementById("Tools:PrivateBrowsing").hidden = true;
      document.getElementById("key_privatebrowsing").remove();
    }

    if (BrowserUIUtils.quitShortcutDisabled) {
      document.getElementById("key_quitApplication").remove();
      document.getElementById("menu_FileQuitItem").removeAttribute("key");

      PanelMultiView.getViewNode(
        document,
        "appMenu-quit-button2"
      )?.removeAttribute("key");
    }

    this._loadHandled = true;
  },

  _cancelDelayedStartup() {
    window.removeEventListener("MozAfterPaint", this._boundDelayedStartup);
    this._boundDelayedStartup = null;
  },

  _delayedStartup() {
    this._cancelDelayedStartup();

    gBrowser.addEventListener(
      "PermissionStateChange",
      function () {
        gIdentityHandler.refreshIdentityBlock();
      },
      true
    );

    this._handleURIToLoad();

    Services.obs.addObserver(gIdentityHandler, "perm-changed");
    Services.obs.addObserver(
      gSessionHistoryObserver,
      "browser:purge-session-history"
    );
    Services.obs.addObserver(
      gStoragePressureObserver,
      "QuotaManager::StoragePressure"
    );
    Services.obs.addObserver(gKeywordURIFixup, "keyword-uri-fixup");
    Services.obs.addObserver(gLocaleChangeObserver, "intl:app-locales-changed");
    BrowserUtils.callModulesFromCategory(
      {
        categoryName: "browser-window-delayed-startup",
        jsGlobal: globalThis,
      },
      window
    );

    UpdateUrlbarSearchSplitterState();

    if (Services.prefs.getBoolPref("browser.search.widget.new", false)) {
      document.getElementById("searchbar-new")?.delayedStartupInit();
    }

    let safeMode = document.getElementById("helpSafeMode");
    if (Services.appinfo.inSafeMode) {
      document.l10n.setAttributes(safeMode, "menu-help-exit-troubleshoot-mode");
      safeMode.setAttribute(
        "appmenu-data-l10n-id",
        "appmenu-help-exit-troubleshoot-mode"
      );
    }

    gBidiUI = isBidiEnabled();
    if (gBidiUI) {
      document.getElementById("documentDirection-separator").hidden = false;
      document.getElementById("documentDirection-swap").hidden = false;
      document.getElementById("textfieldDirection-separator").hidden = false;
      document.getElementById("textfieldDirection-swap").hidden = false;
    }

    if (!Services.prefs.getBoolPref("ui.click_hold_context_menus", false)) {
      SetClickAndHoldHandlers();
    }

    function initBackForwardButtonTooltip(tooltipId, l10nId, shortcutId) {
      let shortcut = document.getElementById(shortcutId);
      shortcut = ShortcutUtils.prettifyShortcut(shortcut);

      let tooltip = document.getElementById(tooltipId);
      document.l10n.setAttributes(tooltip, l10nId, { shortcut });
    }

    initBackForwardButtonTooltip(
      "back-button-tooltip-description",
      "navbar-tooltip-back-2",
      "goBackKb"
    );

    initBackForwardButtonTooltip(
      "forward-button-tooltip-description",
      "navbar-tooltip-forward-2",
      "goForwardKb"
    );

    if (AppConstants.platform != "macosx") {
      updateEditUIVisibility();
      if (AppConstants.MOZ_PLACES) {
        let placesContext = document.getElementById("placesContext");
        placesContext.addEventListener("popupshowing", updateEditUIVisibility);
        placesContext.addEventListener("popuphiding", updateEditUIVisibility);
      }
    }

    let wasMinimized = window.windowState == window.STATE_MINIMIZED;
    window.addEventListener("sizemodechange", () => {
      let isMinimized = window.windowState == window.STATE_MINIMIZED;
      if (wasMinimized != isMinimized) {
        wasMinimized = isMinimized;
        UpdatePopupNotificationsVisibility();
      }
    });

    window.addEventListener("mousemove", MousePosTracker);
    window.addEventListener("mouseout", MousePosTracker);
    window.addEventListener("dragover", MousePosTracker);

    let htmlTooltip = document.getElementById("aHTMLTooltip");
    htmlTooltip.addEventListener("popupshowing", () => {
      let browser =
        htmlTooltip.triggerNode?.documentGlobal.browsingContext.top
          .embedderElement;
      htmlTooltip.toggleAttribute(
        "contenttooltip",
        browser?.getTabBrowser() == gBrowser
      );
    });

    gNavToolbox.addEventListener("customizationstarting", CustomizationHandler);
    gNavToolbox.addEventListener("aftercustomization", CustomizationHandler);

    SessionStore.promiseInitialized.then(() => {
      if (window.closed) {
        return;
      }

      BrowserUtils.callModulesFromCategory(
        {
          categoryName: "browser-window-sessionstore-initialized",
          jsGlobal: globalThis,
        },
        window
      );
    });

    if (BrowserHandler.kiosk) {
      if (!gURLBar.readOnly) {
        window.fullScreen = true;
      }
    }

    SessionStore.promiseAllWindowsRestored.then(() => {
      this._schedulePerWindowIdleTasks();
      document.documentElement.setAttribute("sessionrestored", "true");
    });

    this.delayedStartupFinished = true;
    _resolveDelayedStartup();
    Services.obs.notifyObservers(window, "browser-delayed-startup-finished");
  },

  get firstContentWindowPaintPromise() {
    return this._firstContentWindowPaintDeferred.promise;
  },

  _setInitialFocus() {
    let initiallyFocusedElement = document.commandDispatcher.focusedElement;

    let shouldRemoveFocusedAttribute = true;

    this._callWithURIToLoad(uriToLoad => {
      if (
        isBlankPageURL(uriToLoad) ||
        uriToLoad == "about:privatebrowsing" ||
        this.getTabToAdopt()?.isEmpty
      ) {
        gURLBar.select();
        shouldRemoveFocusedAttribute = false;
        return;
      }

      let promise = gBrowser.selectedBrowser.isRemoteBrowser
        ? this.firstContentWindowPaintPromise
        : Promise.resolve();

      promise.then(() => {
        if (
          document.commandDispatcher.focusedElement == initiallyFocusedElement
        ) {
          gBrowser.selectedBrowser.focus();
        }
      });
    });

    if (shouldRemoveFocusedAttribute) {
      window.requestAnimationFrame(() => {
        if (shouldRemoveFocusedAttribute) {
          gURLBar.removeAttribute("focused");
        }
      });
    }
  },

  _handleURIToLoad() {
    this._callWithURIToLoad(uriToLoad => {
      if (!uriToLoad) {
        return;
      }

      if (Array.isArray(uriToLoad)) {
        try {
          gBrowser.loadTabs(uriToLoad, {
            inBackground: false,
            replace: true,
            userContextId: window.arguments[5],
            triggeringPrincipal:
              window.arguments[8] ||
              Services.scriptSecurityManager.getSystemPrincipal(),
            allowInheritPrincipal: window.arguments[9],
            policyContainer: window.arguments[10],
            fromExternal: true,
          });
        } catch (e) {}
      } else if (window.arguments.length >= 3) {
        let userContextId =
          window.arguments[5] != undefined
            ? window.arguments[5]
            : Ci.nsIScriptSecurityManager.DEFAULT_USER_CONTEXT_ID;

        let hasValidUserGestureActivation = undefined;
        let textDirectiveUserActivation = undefined;
        let fromExternal = undefined;
        let globalHistoryOptions = undefined;
        let triggeringRemoteType = undefined;
        let forceAllowDataURI = false;
        let schemelessInput = Ci.nsILoadInfo.SchemelessInputTypeUnset;
        if (window.arguments[1]) {
          if (!(window.arguments[1] instanceof Ci.nsIPropertyBag2)) {
            throw new Error(
              "window.arguments[1] must be null or Ci.nsIPropertyBag2!"
            );
          }

          let extraOptions = window.arguments[1];
          if (extraOptions.hasKey("hasValidUserGestureActivation")) {
            hasValidUserGestureActivation = extraOptions.getPropertyAsBool(
              "hasValidUserGestureActivation"
            );
          }
          if (extraOptions.hasKey("textDirectiveUserActivation")) {
            textDirectiveUserActivation = extraOptions.getPropertyAsBool(
              "textDirectiveUserActivation"
            );
          }
          if (extraOptions.hasKey("fromExternal")) {
            fromExternal = extraOptions.getPropertyAsBool("fromExternal");
          }
          if (extraOptions.hasKey("triggeringSponsoredURL")) {
            globalHistoryOptions = {
              triggeringSponsoredURL: extraOptions.getPropertyAsACString(
                "triggeringSponsoredURL"
              ),
            };
            if (extraOptions.hasKey("triggeringSponsoredURLVisitTimeMS")) {
              globalHistoryOptions.triggeringSponsoredURLVisitTimeMS =
                extraOptions.getPropertyAsUint64(
                  "triggeringSponsoredURLVisitTimeMS"
                );
            }
            if (extraOptions.hasKey("triggeringSource")) {
              globalHistoryOptions.triggeringSource =
                extraOptions.getPropertyAsACString("triggeringSource");
            }
          }
          if (extraOptions.hasKey("triggeringRemoteType")) {
            triggeringRemoteType = extraOptions.getPropertyAsACString(
              "triggeringRemoteType"
            );
          }
          if (extraOptions.hasKey("forceAllowDataURI")) {
            forceAllowDataURI =
              extraOptions.getPropertyAsBool("forceAllowDataURI");
          }
          if (extraOptions.hasKey("schemelessInput")) {
            schemelessInput =
              extraOptions.getPropertyAsUint32("schemelessInput");
          }
        }

        try {
          openLinkIn(uriToLoad, "current", {
            referrerInfo: window.arguments[2] || null,
            postData: window.arguments[3] || null,
            allowThirdPartyFixup: window.arguments[4] || false,
            userContextId,
            originPrincipal: window.arguments[6],
            originStoragePrincipal: window.arguments[7],
            triggeringPrincipal: window.arguments[8],
            allowInheritPrincipal: window.arguments[9] !== false,
            policyContainer: window.arguments[10],
            forceAboutBlankViewerInCurrent: !!window.arguments[6],
            forceAllowDataURI,
            hasValidUserGestureActivation,
            textDirectiveUserActivation,
            fromExternal,
            globalHistoryOptions,
            triggeringRemoteType,
            schemelessInput,
          });
        } catch (e) {
          console.error(e);
        }

        window.focus();
      } else {
        loadOneOrMoreURIs(uriToLoad, {
          newWindowLoad: true,
        });
      }
    });
  },

  _schedulePerWindowIdleTasks() {
    if (window.closed) {
      return;
    }

    function scheduleIdleTask(func, options) {
      requestIdleCallback(function idleTaskRunner() {
        if (!window.closed) {
          func();
        }
      }, options);
    }

    scheduleIdleTask(() => {
      let reduceMotionQuery = window.matchMedia(
        "(prefers-reduced-motion: reduce)"
      );
      function readSetting() {
        gReduceMotionSetting = reduceMotionQuery.matches;
      }
      reduceMotionQuery.addListener(readSetting);
      readSetting();
    });

    scheduleIdleTask(() => {
      gGestureSupport.init(true);

      gHistorySwipeAnimation.init();
    });

    scheduleIdleTask(
      () => {
        try {
          DownloadsCommon.initializeAllDataLinks();
          ChromeUtils.importESModule(
            "moz-src:///browser/components/downloads/DownloadsTaskbar.sys.mjs"
          )
            .DownloadsTaskbar.registerIndicator(window)
            .catch(ex => {
              console.error(ex);
            });
          if (AppConstants.platform == "macosx") {
            ChromeUtils.importESModule(
              "moz-src:///browser/components/downloads/DownloadsMacFinderProgress.sys.mjs"
            ).DownloadsMacFinderProgress.register();
          }
        } catch (ex) {
          console.error(ex);
        }
      },
      { timeout: 10000 }
    );

    if (Win7Features) {
      scheduleIdleTask(() => Win7Features.onOpenWindow());
    }

    scheduleIdleTask(() => {
      this.idleTasksFinished.resolve();
      Services.obs.notifyObservers(
        window,
        "browser-idle-startup-tasks-finished"
      );
    });
  },

  get uriToLoadPromise() {
    delete this.uriToLoadPromise;
    return (this.uriToLoadPromise = (function () {
      let uri = window.arguments?.[0];
      if (!uri || window.XULElement.isInstance(uri)) {
        return null;
      }

      let defaultArgs = BrowserHandler.defaultArgs;

      if (uri != defaultArgs) {
        if (uri instanceof Ci.nsIArray) {
          return Array.from(
            uri.enumerate(Ci.nsISupportsString),
            supportStr => supportStr.data
          );
        } else if (uri instanceof Ci.nsISupportsString) {
          return uri.data;
        }
        return uri;
      }

      let willOverride = SessionStartup.willOverrideHomepage;
      if (typeof willOverride == "boolean") {
        return willOverride ? null : uri;
      }
      return willOverride.then(willOverrideHomepage =>
        willOverrideHomepage ? null : uri
      );
    })());
  },

  _callWithURIToLoad(callback) {
    let uriToLoad = this.uriToLoadPromise;
    if (uriToLoad && uriToLoad.then) {
      uriToLoad.then(callback);
    } else {
      callback(uriToLoad);
    }
  },

  onUnload() {
    BrowserUtils.callModulesFromCategory(
      { categoryName: "browser-window-unload-begin", jsGlobal: globalThis },
      window
    );

    if (!this._loadHandled) {
      return;
    }

    gGestureSupport.init(false);

    gHistorySwipeAnimation.uninit();

    try {
      gBrowser.removeProgressListener(window.XULBrowserWindow);
      gBrowser.removeTabsProgressListener(window.TabsProgressListener);
    } catch (ex) {}

    BrowserUtils.callModulesFromCategory(
      { categoryName: "browser-window-unload", jsGlobal: globalThis },
      window
    );

    if (this._boundDelayedStartup) {
      this._cancelDelayedStartup();
    } else {
      if (Win7Features) {
        Win7Features.onCloseWindow();
      }
      BrowserUtils.callModulesFromCategory(
        {
          categoryName: "browser-window-unload-delayed-startup",
          jsGlobal: globalThis,
        },
        window
      );

      Services.obs.removeObserver(gIdentityHandler, "perm-changed");
      Services.obs.removeObserver(
        gSessionHistoryObserver,
        "browser:purge-session-history"
      );
      Services.obs.removeObserver(
        gStoragePressureObserver,
        "QuotaManager::StoragePressure"
      );
      Services.obs.removeObserver(gKeywordURIFixup, "keyword-uri-fixup");
      Services.obs.removeObserver(
        gLocaleChangeObserver,
        "intl:app-locales-changed"
      );
    }

    BrowserUtils.callModulesFromCategory(
      {
        categoryName: "browser-window-unload-tabbrowser",
        jsGlobal: globalThis,
      },
      window
    );
    window.XULBrowserWindow = null;
    window.docShell.treeOwner
      .QueryInterface(Ci.nsIInterfaceRequestor)
      .getInterface(Ci.nsIAppWindow).XULBrowserWindow = null;

    BrowserUtils.callModulesFromCategory(
      { categoryName: "browser-window-final-unload", jsGlobal: globalThis },
      window
    );
  },
};
