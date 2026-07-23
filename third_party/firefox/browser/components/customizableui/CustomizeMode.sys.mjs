/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const kPrefCustomizationDebug = "browser.uiCustomization.debug";
const kPaletteId = "customization-palette";
const kDragDataTypePrefix = "text/toolbarwrapper-id/";
const kSkipSourceNodePref = "browser.uiCustomization.skipSourceNodeCheck";
const kDrawInTitlebarPref = "browser.tabs.inTitlebar";
const kCompactModeShowPref = "browser.compactmode.show";
const kBookmarksToolbarPref = "browser.toolbars.bookmarks.visibility";
const kKeepBroadcastAttributes = "keepbroadcastattributeswhencustomizing";

const kPanelItemContextMenu = "customizationPanelItemContextMenu";
const kPaletteItemContextMenu = "customizationPaletteItemContextMenu";

const kDownloadAutohideCheckboxId = "downloads-button-autohide-checkbox";
const kDownloadAutohidePanelId = "downloads-button-autohide-panel";
const kDownloadAutoHidePref = "browser.download.autohideButton";

import { CustomizableUI } from "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  DragPositionManager:
    "moz-src:///browser/components/customizableui/DragPositionManager.sys.mjs",
  URILoadingHelper: "resource:///modules/URILoadingHelper.sys.mjs",
});
ChromeUtils.defineLazyGetter(lazy, "gWidgetsBundle", function () {
  const kUrl =
    "chrome://browser/locale/customizableui/customizableWidgets.properties";
  return Services.strings.createBundle(kUrl);
});
XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gTouchBarUpdater",
  "@mozilla.org/widget/touchbarupdater;1",
  Ci.nsITouchBarUpdater
);

let gDebug;
ChromeUtils.defineLazyGetter(lazy, "log", () => {
  let { ConsoleAPI } = ChromeUtils.importESModule(
    "resource://gre/modules/Console.sys.mjs"
  );
  gDebug = Services.prefs.getBoolPref(kPrefCustomizationDebug, false);
  let consoleOptions = {
    maxLogLevel: gDebug ? "all" : "log",
    prefix: "CustomizeMode",
  };
  return new ConsoleAPI(consoleOptions);
});

var gDraggingInToolbars;

var gTab;

function closeGlobalTab() {
  let win = gTab.documentGlobal;
  if (win.gBrowser.browsers.length == 1) {
    win.BrowserCommands.openTab();
  }
  win.gBrowser.removeTab(gTab, { animate: true });
  gTab = null;
}

var gTabsProgressListener = {
  onLocationChange(aBrowser, aWebProgress, aRequest, aLocation) {
    if (
      !gTab ||
      gTab.linkedBrowser != aBrowser ||
      aLocation.spec == "about:blank"
    ) {
      return;
    }

    unregisterGlobalTab();
  },
};

function unregisterGlobalTab() {
  gTab.removeEventListener("TabClose", unregisterGlobalTab);
  let win = gTab.documentGlobal;
  win.removeEventListener("unload", unregisterGlobalTab);
  win.gBrowser.removeTabsProgressListener(gTabsProgressListener);

  gTab.removeAttribute("customizemode");

  gTab = null;
}

export class CustomizeMode {
  constructor(aWindow) {
    this.#window = aWindow;
    this.#document = aWindow.document;
    this.#browser = aWindow.gBrowser;
    this.areas = new Set();

    this.#translationObserver = new aWindow.MutationObserver(mutations =>
      this.#onTranslations(mutations)
    );
    this.#ensureCustomizationPanels();

    let content = this.$("customization-content-container");
    if (!content) {
      this.#window.MozXULElement.insertFTLIfNeeded("browser/customizeMode.ftl");
      let container = this.$("customization-container");
      container.replaceChild(
        this.#window.MozXULElement.parseXULToFragment(
          container.firstChild.data
        ),
        container.lastChild
      );
    }

    this.#attachEventListeners();

    this.visiblePalette = this.$(kPaletteId);
    this.pongArena = this.$("customization-pong-arena");

    if (this.#canDrawInTitlebar()) {
      this.#updateTitlebarCheckbox();
      Services.prefs.addObserver(kDrawInTitlebarPref, this);
    } else {
      this.$("customization-titlebar-visibility-checkbox").hidden = true;
    }

    Services.prefs.addObserver(kBookmarksToolbarPref, this);

    this.#window.addEventListener("unload", this);
  }

  #transitioning = false;

  #window = null;

  #document = null;

  #browser = null;

  areas = null;

  #stowedPalette = null;

  #dragOverItem = null;

  #customizing = false;

  #skipSourceNodeCheck = false;

  #enabledCommands = new Set([
    "cmd_newNavigator",
    "cmd_newNavigatorTab",
    "cmd_newNavigatorTabNoEvent",
    "cmd_close",
    "cmd_closeWindow",
    "cmd_maximizeWindow",
    "cmd_minimizeWindow",
    "cmd_restoreWindow",
    "cmd_quitApplication",
    "View:FullScreen",
    "Browser:NextTab",
    "Browser:PrevTab",
    "Browser:NewUserContextTab",
    "Tools:PrivateBrowsing",
    "zoomWindow",
  ]);

  #translationObserver = null;


  #dragSizeMap = null;

  #moveDownloadsButtonToNavBar = false;

  get #handler() {
    return this.#window.CustomizationHandler;
  }

  #uninit() {
    if (this.#canDrawInTitlebar()) {
      Services.prefs.removeObserver(kDrawInTitlebarPref, this);
    }
    Services.prefs.removeObserver(kBookmarksToolbarPref, this);
  }

  $(id) {
    return this.#document.getElementById(id);
  }

  setTab(aTab) {
    if (gTab == aTab) {
      return;
    }

    if (gTab) {
      closeGlobalTab();
    }

    gTab = aTab;

    gTab.setAttribute("customizemode", "true");

    if (gTab.linkedPanel) {
      gTab.linkedBrowser.stop();
    }

    let win = gTab.documentGlobal;

    win.gBrowser.setTabTitle(gTab);
    win.gBrowser.setIcon(gTab, "chrome://browser/skin/customize.svg");

    gTab.addEventListener("TabClose", unregisterGlobalTab);

    win.gBrowser.addTabsProgressListener(gTabsProgressListener);

    win.addEventListener("unload", unregisterGlobalTab);

    if (gTab.selected) {
      win.gCustomizeMode.enter();
    }
  }

  enter() {
    if (
      !this.#window.toolbar.visible ||
      this.#window.document.documentElement.hasAttribute("taskbartab")
    ) {
      let w = lazy.URILoadingHelper.getTargetWindow(this.#window, {
        skipPopups: true,
        skipTaskbarTabs: true,
      });
      if (w) {
        w.gCustomizeMode.enter();
        return;
      }
      let obs = () => {
        Services.obs.removeObserver(obs, "browser-delayed-startup-finished");
        w = lazy.URILoadingHelper.getTargetWindow(this.#window, {
          skipPopups: true,
          skipTaskbarTabs: true,
        });
        w.gCustomizeMode.enter();
      };
      Services.obs.addObserver(obs, "browser-delayed-startup-finished");
      this.#window.openTrustedLinkIn("about:newtab", "window");
      return;
    }
    this._wantToBeInCustomizeMode = true;

    if (this.#customizing || this.#handler.isEnteringCustomizeMode) {
      return;
    }

    if (this.#handler.isExitingCustomizeMode) {
      lazy.log.debug(
        "Attempted to enter while we're in the middle of exiting. " +
          "We'll exit after we've entered"
      );
      return;
    }

    if (!gTab) {
      this.setTab(
        this.#browser.addTab("about:blank", {
          inBackground: false,
          forceNotRemote: true,
          skipAnimation: true,
          triggeringPrincipal:
            Services.scriptSecurityManager.getSystemPrincipal(),
        })
      );
      return;
    }
    if (!gTab.selected) {
      gTab.documentGlobal.gBrowser.selectedTab = gTab;
      return;
    }
    gTab.documentGlobal.focus();
    if (gTab.ownerDocument != this.#document) {
      return;
    }

    let window = this.#window;
    let document = this.#document;

    this.#handler.isEnteringCustomizeMode = true;

    let resetButton = this.$("customization-reset-button");
    resetButton.setAttribute("disabled", "true");

    (async () => {
      if (!this.#window.gBrowserInit.delayedStartupFinished) {
        await new Promise(resolve => {
          let delayedStartupObserver = aSubject => {
            if (aSubject == this.#window) {
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

      CustomizableUI.dispatchToolboxEvent("beforecustomization", {}, window);
      CustomizableUI.notifyStartCustomizing(this.#window);

      document.addEventListener("keypress", this);

      window.PanelUI.hide();

      let panelHolder = document.getElementById("customization-panelHolder");
      let panelContextMenu = document.getElementById(kPanelItemContextMenu);
      this._previousPanelContextMenuParent = panelContextMenu.parentNode;
      document.getElementById("mainPopupSet").appendChild(panelContextMenu);
      panelHolder.appendChild(window.PanelUI.overflowFixedList);

      window.PanelUI.overflowFixedList.toggleAttribute("customizing", true);
      window.PanelUI.menuButton.disabled = true;
      document.getElementById("nav-bar-overflow-button").disabled = true;

      this.#transitioning = true;

      let customizer = document.getElementById("customization-container");
      let browser = document.getElementById("browser");
      browser.hidden = true;
      customizer.hidden = false;

      this.#wrapAreaItemsSync(CustomizableUI.AREA_TABSTRIP);

      this.#document.documentElement.toggleAttribute("customizing", true);

      let customizableToolbars = document.querySelectorAll(
        "toolbar[customizable=true]:not([autohide], [collapsed])"
      );
      for (let toolbar of customizableToolbars) {
        toolbar.toggleAttribute("customizing", true);
      }

      this.#updateOverflowPanelArrowOffset();

      CustomizableUI.dispatchToolboxEvent("customizationstarting", {}, window);

      await this.#wrapAllAreaItems();
      this.#populatePalette();

      this.#setupPaletteDragging();

      window.gNavToolbox.addEventListener("toolbarvisibilitychange", this);

      this.#updateResetButton();
      this.#updateUndoResetButton();
      this.#updateTouchBarButton();
      this.#updateDensityMenu();

      this.#skipSourceNodeCheck =
        Services.prefs.getPrefType(kSkipSourceNodePref) ==
          Ci.nsIPrefBranch.PREF_BOOL &&
        Services.prefs.getBoolPref(kSkipSourceNodePref);

      CustomizableUI.addListener(this);
      this.#customizing = true;
      this.#transitioning = false;

      this.visiblePalette.hidden = false;
      window.setTimeout(() => {
        this.visiblePalette.clientTop;
        this.visiblePalette.setAttribute("showing", "true");
      }, 0);
      this.#updateEmptyPaletteNotice();

      this.#setupDownloadAutoHideToggle();

      this.#handler.isEnteringCustomizeMode = false;

      CustomizableUI.dispatchToolboxEvent("customizationready", {}, window);

      if (!this._wantToBeInCustomizeMode) {
        this.exit();
      }
    })().catch(e => {
      lazy.log.error("Error entering customize mode", e);
      this.#handler.isEnteringCustomizeMode = false;
      this.exit();
    });
  }

  exit() {
    this._wantToBeInCustomizeMode = false;

    if (!this.#customizing || this.#handler.isExitingCustomizeMode) {
      return;
    }

    if (this.#handler.isEnteringCustomizeMode) {
      lazy.log.debug(
        "Attempted to exit while we're in the middle of entering. " +
          "We'll exit after we've entered"
      );
      return;
    }

    if (this.resetting) {
      lazy.log.debug(
        "Attempted to exit while we're resetting. " +
          "We'll exit after resetting has finished."
      );
      return;
    }

    this.#handler.isExitingCustomizeMode = true;

    this.#translationObserver.disconnect();

    this.#teardownDownloadAutoHideToggle();

    CustomizableUI.removeListener(this);

    let window = this.#window;
    let document = this.#document;

    document.removeEventListener("keypress", this);

    this.#togglePong(false);

    let resetButton = this.$("customization-reset-button");
    let undoResetButton = this.$("customization-undo-reset-button");
    undoResetButton.hidden = resetButton.disabled = true;

    this.#transitioning = true;

    this.#depopulatePalette();

    this.#customizing = false;
    document.documentElement.removeAttribute("customizing");

    if (this.#browser.selectedTab == gTab) {
      closeGlobalTab();
    }

    let customizer = document.getElementById("customization-container");
    let browser = document.getElementById("browser");
    customizer.hidden = true;
    browser.hidden = false;

    window.gNavToolbox.removeEventListener("toolbarvisibilitychange", this);

    this.#teardownPaletteDragging();

    (async () => {
      await this.#unwrapAllAreaItems();

      this.areas.clear();

      CustomizableUI.dispatchToolboxEvent("customizationending", {}, window);

      window.PanelUI.menuButton.disabled = false;
      let overflowContainer = document.getElementById(
        "widget-overflow-mainView"
      ).firstElementChild;
      overflowContainer.appendChild(window.PanelUI.overflowFixedList);
      document.getElementById("nav-bar-overflow-button").disabled = false;
      let panelContextMenu = document.getElementById(kPanelItemContextMenu);
      this._previousPanelContextMenuParent.appendChild(panelContextMenu);

      let customizableToolbars = document.querySelectorAll(
        "toolbar[customizable=true]:not([autohide])"
      );
      for (let toolbar of customizableToolbars) {
        toolbar.removeAttribute("customizing");
      }

      this.#maybeMoveDownloadsButtonToNavBar();

      delete this._lastLightweightTheme;
      this.#transitioning = false;
      this.#handler.isExitingCustomizeMode = false;
      CustomizableUI.dispatchToolboxEvent("aftercustomization", {}, window);
      CustomizableUI.notifyEndCustomizing(window);

      if (this._wantToBeInCustomizeMode) {
        this.enter();
      }
    })().catch(e => {
      lazy.log.error("Error exiting customize mode", e);
      this.#handler.isExitingCustomizeMode = false;
    });
  }

  async #updateOverflowPanelArrowOffset() {
    let currentDensity =
      this.#document.documentElement.getAttribute("uidensity");
    let offset = await this.#window.promiseDocumentFlushed(() => {
      let overflowButton = this.$("nav-bar-overflow-button");
      let buttonRect = overflowButton.getBoundingClientRect();
      let endDistance;
      if (this.#window.RTL_UI) {
        endDistance = buttonRect.left;
      } else {
        endDistance = this.#window.innerWidth - buttonRect.right;
      }
      return endDistance + buttonRect.width / 2;
    });
    if (
      !this.#document ||
      currentDensity != this.#document.documentElement.getAttribute("uidensity")
    ) {
      return;
    }
    this.$("customization-panelWrapper").style.setProperty(
      "--panel-arrow-offset",
      offset + "px"
    );
  }

  #getCustomizableChildForNode(aNode) {
    let areas = CustomizableUI.areas;
    let numberOfAreas = areas.length;
    for (let i = 0; i < numberOfAreas; i++) {
      let area = areas[i];
      let areaNode = aNode.ownerDocument.getElementById(area);
      let customizationTarget = CustomizableUI.getCustomizationTarget(areaNode);
      if (customizationTarget && customizationTarget != areaNode) {
        areas.push(customizationTarget.id);
      }
      let overflowTarget =
        areaNode && areaNode.getAttribute("default-overflowtarget");
      if (overflowTarget) {
        areas.push(overflowTarget);
      }
    }
    areas.push(kPaletteId);

    while (aNode && aNode.parentNode) {
      let parent = aNode.parentNode;
      if (areas.includes(parent.id)) {
        return aNode;
      }
      aNode = parent;
    }
    return null;
  }

  #promiseWidgetAnimationOut(aNode) {
    if (
      this.#window.gReduceMotion ||
      aNode.getAttribute("cui-anchorid") == "nav-bar-overflow-button" ||
      (aNode.tagName != "toolbaritem" && aNode.tagName != "toolbarbutton") ||
      (aNode.id == "downloads-button" && aNode.hidden)
    ) {
      return null;
    }

    let animationNode;
    if (aNode.parentNode && aNode.parentNode.id.startsWith("wrapper-")) {
      animationNode = aNode.parentNode;
    } else {
      animationNode = aNode;
    }
    return new Promise(resolve => {
      function cleanupCustomizationExit() {
        resolveAnimationPromise();
      }

      function cleanupWidgetAnimationEnd(e) {
        if (
          e.animationName == "widget-animate-out" &&
          e.target.id == animationNode.id
        ) {
          resolveAnimationPromise();
        }
      }

      function resolveAnimationPromise() {
        animationNode.removeEventListener(
          "animationend",
          cleanupWidgetAnimationEnd
        );
        animationNode.removeEventListener(
          "customizationending",
          cleanupCustomizationExit
        );
        resolve(animationNode);
      }

      this.#window.requestAnimationFrame(() => {
        this.#window.requestAnimationFrame(() => {
          animationNode.classList.add("animate-out");
          animationNode.documentGlobal.gNavToolbox.addEventListener(
            "customizationending",
            cleanupCustomizationExit
          );
          animationNode.addEventListener(
            "animationend",
            cleanupWidgetAnimationEnd
          );
        });
      });
    });
  }

  async addToToolbar(aNode) {
    aNode = this.#getCustomizableChildForNode(aNode);
    if (aNode.localName == "toolbarpaletteitem" && aNode.firstElementChild) {
      aNode = aNode.firstElementChild;
    }
    let widgetAnimationPromise = this.#promiseWidgetAnimationOut(aNode);
    let animationNode;
    if (widgetAnimationPromise) {
      animationNode = await widgetAnimationPromise;
    }

    let widgetToAdd = aNode.id;
    if (
      CustomizableUI.isSpecialWidget(widgetToAdd) &&
      aNode.closest("#customization-palette")
    ) {
      widgetToAdd = widgetToAdd.match(
        /^customizableui-special-(spring|spacer|separator)/
      )[1];
    }

    CustomizableUI.addWidgetToArea(widgetToAdd, CustomizableUI.AREA_NAVBAR);
    if (!this.#customizing) {
      CustomizableUI.dispatchToolboxEvent("customizationchange");
    }

    if (aNode.id == "downloads-button") {
      Services.prefs.setBoolPref(kDownloadAutoHidePref, false);
      if (this.#customizing) {
        this.#showDownloadsAutoHidePanel();
      }
    }

    if (animationNode) {
      animationNode.classList.remove("animate-out");
    }
  }

  async addToPanel(aNode, aReason) {
    aNode = this.#getCustomizableChildForNode(aNode);
    if (aNode.localName == "toolbarpaletteitem" && aNode.firstElementChild) {
      aNode = aNode.firstElementChild;
    }
    let widgetAnimationPromise = this.#promiseWidgetAnimationOut(aNode);
    let animationNode;
    if (widgetAnimationPromise) {
      animationNode = await widgetAnimationPromise;
    }

    let panel = CustomizableUI.AREA_FIXED_OVERFLOW_PANEL;
    CustomizableUI.addWidgetToArea(aNode.id, panel);
    if (!this.#customizing) {
      CustomizableUI.dispatchToolboxEvent("customizationchange");
    }

    if (aNode.id == "downloads-button") {
      Services.prefs.setBoolPref(kDownloadAutoHidePref, false);
      if (this.#customizing) {
        this.#showDownloadsAutoHidePanel();
      }
    }

    if (animationNode) {
      animationNode.classList.remove("animate-out");
    }
    if (!this.#window.gReduceMotion) {
      let overflowButton = this.$("nav-bar-overflow-button");
      overflowButton.setAttribute("animate", "true");
      overflowButton.addEventListener(
        "animationend",
        function onAnimationEnd(event) {
          if (event.animationName.startsWith("overflow-animation")) {
            this.removeEventListener("animationend", onAnimationEnd);
            this.removeAttribute("animate");
          }
        }
      );
    }
  }

  async removeFromArea(aNode, aReason) {
    aNode = this.#getCustomizableChildForNode(aNode);
    if (aNode.localName == "toolbarpaletteitem" && aNode.firstElementChild) {
      aNode = aNode.firstElementChild;
    }
    let widgetAnimationPromise = this.#promiseWidgetAnimationOut(aNode);
    let animationNode;
    if (widgetAnimationPromise) {
      animationNode = await widgetAnimationPromise;
    }

    CustomizableUI.removeWidgetFromArea(aNode.id);
    if (!this.#customizing) {
      CustomizableUI.dispatchToolboxEvent("customizationchange");
    }

    if (aNode.id == "downloads-button") {
      Services.prefs.setBoolPref(kDownloadAutoHidePref, false);
      if (this.#customizing) {
        this.#showDownloadsAutoHidePanel();
      }
    }
    if (animationNode) {
      animationNode.classList.remove("animate-out");
    }
  }

  #populatePalette() {
    let fragment = this.#document.createDocumentFragment();
    let toolboxPalette = this.#window.gNavToolbox.palette;

    try {
      let unusedWidgets = CustomizableUI.getUnusedWidgets(toolboxPalette);
      for (let widget of unusedWidgets) {
        let paletteItem = this.#makePaletteItem(widget);
        if (!paletteItem) {
          continue;
        }
        fragment.appendChild(paletteItem);
      }

      let flexSpace = CustomizableUI.createSpecialWidget(
        "spring",
        this.#document
      );
      fragment.appendChild(this.wrapToolbarItem(flexSpace, "palette"));

      this.visiblePalette.appendChild(fragment);
      this.#stowedPalette = this.#window.gNavToolbox.palette;
      this.#window.gNavToolbox.palette = this.visiblePalette;

      this.#updateCommandsDisabledState(true);
    } catch (ex) {
      lazy.log.error(ex);
    }
  }

  #makePaletteItem(aWidget) {
    let widgetNode = aWidget.forWindow(this.#window).node;
    if (!widgetNode) {
      lazy.log.error(
        "Widget with id " + aWidget.id + " does not return a valid node"
      );
      return null;
    }
    if (widgetNode.hidden) {
      return null;
    }

    let wrapper = this.createOrUpdateWrapper(widgetNode, "palette");
    wrapper.appendChild(widgetNode);
    return wrapper;
  }

  #depopulatePalette() {
    this.#updateCommandsDisabledState(false);

    this.visiblePalette.hidden = true;
    let paletteChild = this.visiblePalette.firstElementChild;
    let nextChild;
    while (paletteChild) {
      nextChild = paletteChild.nextElementSibling;
      let itemId = paletteChild.firstElementChild.id;
      if (CustomizableUI.isSpecialWidget(itemId)) {
        this.visiblePalette.removeChild(paletteChild);
      } else {
        let unwrappedPaletteItem = this.unwrapToolbarItem(paletteChild);
        this.#stowedPalette.appendChild(unwrappedPaletteItem);
      }

      paletteChild = nextChild;
    }
    this.visiblePalette.hidden = false;
    this.#window.gNavToolbox.palette = this.#stowedPalette;
  }

  #updateCommandsDisabledState(shouldBeDisabled) {
    for (let command of this.#document.querySelectorAll("command")) {
      if (!command.id || !this.#enabledCommands.has(command.id)) {
        if (shouldBeDisabled) {
          if (!command.hasAttribute("disabled")) {
            command.setAttribute("disabled", true);
          } else {
            command.setAttribute("wasdisabled", true);
          }
        } else if (command.getAttribute("wasdisabled") != "true") {
          command.removeAttribute("disabled");
        } else {
          command.removeAttribute("wasdisabled");
        }
      }
    }
  }

  #isCustomizableItem(aNode) {
    return (
      aNode.localName == "toolbarbutton" ||
      aNode.localName == "toolbaritem" ||
      aNode.localName == "toolbarseparator" ||
      aNode.localName == "toolbarspring" ||
      aNode.localName == "toolbarspacer"
    );
  }

  isWrappedToolbarItem(aNode) {
    return aNode.localName == "toolbarpaletteitem";
  }

  #deferredWrapToolbarItem(aNode, aPlace) {
    return new Promise(resolve => {
      Services.tm.dispatchToMainThread(() => {
        let wrapper = this.wrapToolbarItem(aNode, aPlace);
        resolve(wrapper);
      });
    });
  }

  wrapToolbarItem(aNode, aPlace) {
    if (!this.#isCustomizableItem(aNode)) {
      return aNode;
    }
    let wrapper = this.createOrUpdateWrapper(aNode, aPlace);

    if (aNode.parentNode) {
      aNode = aNode.parentNode.replaceChild(wrapper, aNode);
    }
    wrapper.appendChild(aNode);
    return wrapper;
  }

  #updateWrapperLabel(aNode, aIsUpdate, aWrapper = aNode.parentElement) {
    if (aNode.hasAttribute("label")) {
      aWrapper.setAttribute("title", aNode.getAttribute("label"));
      aWrapper.setAttribute("tooltiptext", aNode.getAttribute("label"));
    } else if (aNode.hasAttribute("title")) {
      aWrapper.setAttribute("title", aNode.getAttribute("title"));
      aWrapper.setAttribute("tooltiptext", aNode.getAttribute("title"));
    } else if (aNode.hasAttribute("data-l10n-id") && !aIsUpdate) {
      this.#translationObserver.observe(aNode, {
        attributes: true,
        attributeFilter: ["label", "title"],
      });
    }
  }

  #onTranslations(aMutations) {
    for (let mut of aMutations) {
      let { target } = mut;
      if (
        target.parentElement?.localName == "toolbarpaletteitem" &&
        (target.hasAttribute("label") || mut.target.hasAttribute("title"))
      ) {
        this.#updateWrapperLabel(target, true);
      }
    }
  }

  createOrUpdateWrapper(aNode, aPlace, aIsUpdate) {
    let wrapper;
    if (
      aIsUpdate &&
      aNode.parentNode &&
      aNode.parentNode.localName == "toolbarpaletteitem"
    ) {
      wrapper = aNode.parentNode;
      aPlace = wrapper.getAttribute("place");
    } else {
      wrapper = this.#document.createXULElement("toolbarpaletteitem");
      wrapper.setAttribute("place", aPlace);
    }

    if (
      aNode.hasAttribute("command") &&
      aNode.getAttribute(kKeepBroadcastAttributes) != "true"
    ) {
      wrapper.setAttribute("itemcommand", aNode.getAttribute("command"));
      aNode.removeAttribute("command");
    }

    if (
      aNode.hasAttribute("observes") &&
      aNode.getAttribute(kKeepBroadcastAttributes) != "true"
    ) {
      wrapper.setAttribute("itemobserves", aNode.getAttribute("observes"));
      aNode.removeAttribute("observes");
    }

    if (aNode.hasAttribute("checked")) {
      wrapper.setAttribute("itemchecked", "true");
      aNode.removeAttribute("checked");
    }

    if (aNode.hasAttribute("id")) {
      wrapper.setAttribute("id", "wrapper-" + aNode.getAttribute("id"));
    }

    this.#updateWrapperLabel(aNode, aIsUpdate, wrapper);

    if (aNode.hasAttribute("flex")) {
      wrapper.setAttribute("flex", aNode.getAttribute("flex"));
    }

    let removable =
      aPlace == "palette" || CustomizableUI.isWidgetRemovable(aNode);
    wrapper.setAttribute("removable", removable);

    wrapper.setAttribute("touchdownstartsdrag", "true");

    let contextMenuAttrName = "";
    if (aNode.getAttribute("context")) {
      contextMenuAttrName = "context";
    } else if (aNode.getAttribute("contextmenu")) {
      contextMenuAttrName = "contextmenu";
    }
    let currentContextMenu = aNode.getAttribute(contextMenuAttrName);
    let contextMenuForPlace =
      aPlace == "panel" ? kPanelItemContextMenu : kPaletteItemContextMenu;
    if (aPlace != "toolbar") {
      wrapper.setAttribute("context", contextMenuForPlace);
    }
    if (currentContextMenu && currentContextMenu != contextMenuForPlace) {
      aNode.setAttribute("wrapped-context", currentContextMenu);
      aNode.setAttribute("wrapped-contextAttrName", contextMenuAttrName);
      aNode.removeAttribute(contextMenuAttrName);
    } else if (currentContextMenu == contextMenuForPlace) {
      aNode.removeAttribute(contextMenuAttrName);
    }

    if (!aIsUpdate) {
      wrapper.addEventListener("mousedown", this);
      wrapper.addEventListener("mouseup", this);
    }

    if (CustomizableUI.isSpecialWidget(aNode.id)) {
      wrapper.setAttribute(
        "title",
        lazy.gWidgetsBundle.GetStringFromName(aNode.nodeName + ".label")
      );
    }

    return wrapper;
  }

  #deferredUnwrapToolbarItem(aWrapper) {
    return new Promise(resolve => {
      Services.tm.dispatchToMainThread(() => {
        let item = null;
        try {
          item = this.unwrapToolbarItem(aWrapper);
        } catch (ex) {
          console.error(ex);
        }
        resolve(item);
      });
    });
  }

  unwrapToolbarItem(aWrapper) {
    if (aWrapper.nodeName != "toolbarpaletteitem") {
      return aWrapper;
    }
    aWrapper.removeEventListener("mousedown", this);
    aWrapper.removeEventListener("mouseup", this);

    let place = aWrapper.getAttribute("place");

    let toolbarItem = aWrapper.firstElementChild;
    if (!toolbarItem) {
      lazy.log.error(
        "no toolbarItem child for " + aWrapper.tagName + "#" + aWrapper.id
      );
      aWrapper.remove();
      return null;
    }

    if (aWrapper.hasAttribute("itemobserves")) {
      toolbarItem.setAttribute(
        "observes",
        aWrapper.getAttribute("itemobserves")
      );
    }

    if (aWrapper.hasAttribute("itemchecked")) {
      toolbarItem.checked = true;
    }

    if (aWrapper.hasAttribute("itemcommand")) {
      let commandID = aWrapper.getAttribute("itemcommand");
      toolbarItem.setAttribute("command", commandID);

      let command = this.$(commandID);
      toolbarItem.toggleAttribute(
        "disabled",
        !!command?.hasAttribute("disabled")
      );
    }

    let wrappedContext = toolbarItem.getAttribute("wrapped-context");
    if (wrappedContext) {
      let contextAttrName = toolbarItem.getAttribute("wrapped-contextAttrName");
      toolbarItem.setAttribute(contextAttrName, wrappedContext);
      toolbarItem.removeAttribute("wrapped-contextAttrName");
      toolbarItem.removeAttribute("wrapped-context");
    } else if (place == "panel") {
      toolbarItem.setAttribute("context", kPanelItemContextMenu);
    }

    if (aWrapper.parentNode) {
      aWrapper.parentNode.replaceChild(toolbarItem, aWrapper);
    }
    return toolbarItem;
  }

  async #wrapAreaItems(aArea) {
    let target = CustomizableUI.getCustomizeTargetForArea(aArea, this.#window);
    if (!target || this.areas.has(target)) {
      return null;
    }

    this.#addCustomizeTargetDragAndDropHandlers(target);
    for (let child of target.children) {
      if (
        this.#isCustomizableItem(child) &&
        !this.isWrappedToolbarItem(child)
      ) {
        await this.#deferredWrapToolbarItem(
          child,
          CustomizableUI.getPlaceForItem(child)
        ).catch(lazy.log.error);
      }
    }
    this.areas.add(target);
    return target;
  }

  #wrapAreaItemsSync(aArea) {
    let target = CustomizableUI.getCustomizeTargetForArea(aArea, this.#window);
    if (!target || this.areas.has(target)) {
      return null;
    }

    this.#addCustomizeTargetDragAndDropHandlers(target);
    try {
      for (let child of target.children) {
        if (
          this.#isCustomizableItem(child) &&
          !this.isWrappedToolbarItem(child)
        ) {
          this.wrapToolbarItem(child, CustomizableUI.getPlaceForItem(child));
        }
      }
    } catch (ex) {
      lazy.log.error(ex, ex.stack);
    }

    this.areas.add(target);
    return target;
  }

  async #wrapAllAreaItems() {
    for (let area of CustomizableUI.areas) {
      await this.#wrapAreaItems(area);
    }
  }

  #addCustomizeTargetDragAndDropHandlers(aTarget) {
    if (aTarget.id == CustomizableUI.AREA_FIXED_OVERFLOW_PANEL) {
      aTarget = this.$("customization-panelHolder");
    }
    aTarget.addEventListener("dragstart", this, true);
    aTarget.addEventListener("dragover", this, true);
    aTarget.addEventListener("dragleave", this, true);
    aTarget.addEventListener("drop", this, true);
    aTarget.addEventListener("dragend", this, true);
  }

  #wrapItemsInArea(target) {
    for (let child of target.children) {
      if (this.#isCustomizableItem(child)) {
        this.wrapToolbarItem(child, CustomizableUI.getPlaceForItem(child));
      }
    }
  }

  #removeCustomizeTargetDragAndDropHandlers(aTarget) {
    if (aTarget.id == CustomizableUI.AREA_FIXED_OVERFLOW_PANEL) {
      aTarget = this.$("customization-panelHolder");
    }
    aTarget.removeEventListener("dragstart", this, true);
    aTarget.removeEventListener("dragover", this, true);
    aTarget.removeEventListener("dragleave", this, true);
    aTarget.removeEventListener("drop", this, true);
    aTarget.removeEventListener("dragend", this, true);
  }

  #unwrapItemsInArea(target) {
    for (let toolbarItem of target.children) {
      if (this.isWrappedToolbarItem(toolbarItem)) {
        this.unwrapToolbarItem(toolbarItem);
      }
    }
  }

  #unwrapAllAreaItems() {
    return (async () => {
      for (let target of this.areas) {
        for (let toolbarItem of target.children) {
          if (this.isWrappedToolbarItem(toolbarItem)) {
            await this.#deferredUnwrapToolbarItem(toolbarItem);
          }
        }
        this.#removeCustomizeTargetDragAndDropHandlers(target);
      }
      this.areas.clear();
    })().catch(lazy.log.error);
  }

  reset() {
    this.resetting = true;
    let btn = this.$("customization-reset-button");
    btn.disabled = true;
    return (async () => {
      this.#depopulatePalette();
      await this.#unwrapAllAreaItems();

      CustomizableUI.reset();

      await this.#wrapAllAreaItems();
      this.#populatePalette();

      this.#updateResetButton();
      this.#updateUndoResetButton();
      this.#updateEmptyPaletteNotice();
      this.#moveDownloadsButtonToNavBar = false;
      this.resetting = false;
      if (!this._wantToBeInCustomizeMode) {
        this.exit();
      }
    })().catch(lazy.log.error);
  }

  undoReset() {
    this.resetting = true;

    return (async () => {
      this.#depopulatePalette();
      await this.#unwrapAllAreaItems();

      CustomizableUI.undoReset();

      await this.#wrapAllAreaItems();
      this.#populatePalette();

      this.#updateResetButton();
      this.#updateUndoResetButton();
      this.#updateEmptyPaletteNotice();
      this.#moveDownloadsButtonToNavBar = false;
      this.resetting = false;
    })().catch(lazy.log.error);
  }

  #onToolbarVisibilityChange(aEvent) {
    let toolbar = aEvent.target;
    toolbar.toggleAttribute(
      "customizing",
      aEvent.detail.visible && toolbar.getAttribute("customizable") == "true"
    );
    this.#onUIChange();
  }

  onWidgetMoved() {
    this.#onUIChange();
  }

  onWidgetAdded() {
    this.#onUIChange();
  }

  onWidgetRemoved() {
    this.#onUIChange();
  }

  onWidgetBeforeDOMChange(aNodeToChange, aSecondaryNode, aContainer) {
    if (aContainer.documentGlobal != this.#window || this.resetting) {
      return;
    }
    if (aNodeToChange.parentNode) {
      this.unwrapToolbarItem(aNodeToChange.parentNode);
    }
    if (aSecondaryNode) {
      this.unwrapToolbarItem(aSecondaryNode.parentNode);
    }
  }

  onWidgetAfterDOMChange(aNodeToChange, aSecondaryNode, aContainer) {
    if (aContainer.documentGlobal != this.#window || this.resetting) {
      return;
    }
    if (aNodeToChange.parentNode) {
      let place = CustomizableUI.getPlaceForItem(aNodeToChange);
      this.wrapToolbarItem(aNodeToChange, place);
      if (aSecondaryNode) {
        this.wrapToolbarItem(aSecondaryNode, place);
      }
    } else {

      let widgetId = aNodeToChange.id;
      let widget = CustomizableUI.getWidget(widgetId);
      if (widget.provider == CustomizableUI.PROVIDER_API) {
        let paletteItem = this.#makePaletteItem(widget);
        this.visiblePalette.appendChild(paletteItem);
      }
    }
  }

  onWidgetDestroyed(aWidgetId) {
    let wrapper = this.$("wrapper-" + aWidgetId);
    if (wrapper) {
      wrapper.remove();
    }
  }

  onWidgetAfterCreation(aWidgetId, aArea) {
    if (!aArea) {
      let widgetNode = this.$(aWidgetId);
      if (widgetNode) {
        this.wrapToolbarItem(widgetNode, "palette");
      } else {
        let widget = CustomizableUI.getWidget(aWidgetId);
        this.visiblePalette.appendChild(this.#makePaletteItem(widget));
      }
    }
  }

  onAreaNodeRegistered(aArea, aContainer) {
    if (aContainer.ownerDocument == this.#document) {
      this.#wrapItemsInArea(aContainer);
      this.#addCustomizeTargetDragAndDropHandlers(aContainer);
      this.areas.add(aContainer);
    }
  }

  onAreaNodeUnregistered(aArea, aContainer, aReason) {
    if (
      aContainer.ownerDocument == this.#document &&
      aReason == CustomizableUI.REASON_AREA_UNREGISTERED
    ) {
      this.#unwrapItemsInArea(aContainer);
      this.#removeCustomizeTargetDragAndDropHandlers(aContainer);
      this.areas.delete(aContainer);
    }
  }

  #openUIDensityPreferences() {
    this.#window.openPreferences("appearance-windowDensity");
  }

  #updateDensityMenu() {
    let button = this.#document.getElementById(
      "customization-uidensity-button"
    );
    let link = this.#document.getElementById("customization-uidensity-link");

    if (this.#window.gUIDensity.novaEnabled) {
      button.hidden = true;
      link.hidden = false;
      return;
    }

    link.hidden = true;

    let gUIDensity = this.#window.gUIDensity;
    if (gUIDensity.getCurrentDensity().mode == gUIDensity.MODE_COMPACT) {
      Services.prefs.setBoolPref(kCompactModeShowPref, true);
    }

    button.hidden =
      !Services.prefs.getBoolPref(kCompactModeShowPref) &&
      !button.querySelector("#customization-uidensity-menuitem-touch");
  }

  #openAddonsManagerThemes() {
    this.#window.BrowserAddonUI.openAddonsMgr("addons://list/theme");
  }

  #previewUIDensity(mode) {
    this.#window.gUIDensity.update(mode);
    this.#updateOverflowPanelArrowOffset();
  }

  #resetUIDensity() {
    this.#window.gUIDensity.update();
    this.#updateOverflowPanelArrowOffset();
  }

  setUIDensity(mode) {
    let win = this.#window;
    let gUIDensity = win.gUIDensity;
    let currentDensity = gUIDensity.getCurrentDensity();
    let panel = win.document.getElementById("customization-uidensity-menu");

    Services.prefs.setIntPref(gUIDensity.uiDensityPref, mode);

    if (currentDensity.overridden) {
      Services.prefs.setBoolPref(gUIDensity.autoTouchModePref, false);
    }

    this.#onUIChange();
    panel.hidePopup();
    this.#updateOverflowPanelArrowOffset();
  }

  #onUIDensityMenuShowing() {
    let win = this.#window;
    let doc = win.document;
    let gUIDensity = win.gUIDensity;
    let currentDensity = gUIDensity.getCurrentDensity();

    let normalItem = doc.getElementById(
      "customization-uidensity-menuitem-normal"
    );
    normalItem.mode = gUIDensity.MODE_NORMAL;

    let items = [normalItem];

    let compactItem = doc.getElementById(
      "customization-uidensity-menuitem-compact"
    );
    compactItem.mode = gUIDensity.MODE_COMPACT;

    if (Services.prefs.getBoolPref(kCompactModeShowPref)) {
      compactItem.hidden = false;
      items.push(compactItem);
    } else {
      compactItem.hidden = true;
    }

    let touchItem = doc.getElementById(
      "customization-uidensity-menuitem-touch"
    );
    if (touchItem) {
      touchItem.mode = gUIDensity.MODE_TOUCH;
      items.push(touchItem);
    }

    for (let item of items) {
      if (item.mode == currentDensity.mode) {
        item.setAttribute("aria-checked", "true");
        item.setAttribute("active", "true");
      } else {
        item.removeAttribute("aria-checked");
        item.removeAttribute("active");
      }
    }

    if (AppConstants.platform == "win") {
      let spacer = doc.getElementById("customization-uidensity-touch-spacer");
      let checkbox = doc.getElementById(
        "customization-uidensity-autotouchmode-checkbox"
      );
      spacer.removeAttribute("hidden");
      checkbox.removeAttribute("hidden");

      if (currentDensity.overridden) {
        let sb = Services.strings.createBundle(
          "chrome://browser/locale/uiDensity.properties"
        );
        touchItem.setAttribute(
          "acceltext",
          sb.GetStringFromName("uiDensity.menuitem-touch.acceltext")
        );
      } else {
        touchItem.removeAttribute("acceltext");
      }

      let autoTouchMode = Services.prefs.getBoolPref(
        win.gUIDensity.autoTouchModePref
      );
      if (autoTouchMode) {
        checkbox.setAttribute("checked", "true");
      } else {
        checkbox.removeAttribute("checked");
      }
    }
  }

  #updateAutoTouchMode(checked) {
    Services.prefs.setBoolPref("browser.touchmode.auto", checked);
    this.#onUIDensityMenuShowing();
    this.#onUIChange();
  }

  #onUIChange() {
    if (!this.resetting) {
      this.#updateResetButton();
      this.#updateUndoResetButton();
      this.#updateEmptyPaletteNotice();
    }
    CustomizableUI.dispatchToolboxEvent("customizationchange");
  }

  #updateEmptyPaletteNotice() {
    let paletteItems =
      this.visiblePalette.getElementsByTagName("toolbarpaletteitem");
    let whimsyButton = this.$("whimsy-button");

    if (
      paletteItems.length == 1 &&
      paletteItems[0].id.includes("wrapper-customizableui-special-spring")
    ) {
      whimsyButton.hidden = false;
    } else {
      this.#togglePong(false);
      whimsyButton.hidden = true;
    }
  }

  #updateResetButton() {
    let btn = this.$("customization-reset-button");
    btn.disabled = CustomizableUI.inDefaultState;
  }

  #updateUndoResetButton() {
    let undoResetButton = this.$("customization-undo-reset-button");
    undoResetButton.hidden = !CustomizableUI.canUndoReset;
  }

  #updateTouchBarButton() {
    if (AppConstants.platform != "macosx") {
      return;
    }
    let touchBarButton = this.$("customization-touchbar-button");
    let touchBarSpacer = this.$("customization-touchbar-spacer");

    let isTouchBarInitialized = lazy.gTouchBarUpdater.isTouchBarInitialized();
    touchBarButton.hidden = !isTouchBarInitialized;
    touchBarSpacer.hidden = !isTouchBarInitialized;
  }

  handleEvent(aEvent) {
    switch (aEvent.type) {
      case "toolbarvisibilitychange":
        this.#onToolbarVisibilityChange(aEvent);
        break;
      case "dragstart":
        this.#onDragStart(aEvent);
        break;
      case "dragover":
        this.#onDragOver(aEvent);
        break;
      case "drop":
        this.#onDragDrop(aEvent);
        break;
      case "dragleave":
        this.#onDragLeave(aEvent);
        break;
      case "dragend":
        this.#onDragEnd(aEvent);
        break;
      case "mousedown":
        this.#onMouseDown(aEvent);
        break;
      case "mouseup":
        this.#onMouseUp(aEvent);
        break;
      case "keypress":
        if (aEvent.keyCode == aEvent.DOM_VK_ESCAPE) {
          this.exit();
        }
        break;
      case "unload":
        this.#uninit();
        break;
    }
  }

  #setupPaletteDragging() {
    this.#addCustomizeTargetDragAndDropHandlers(this.visiblePalette);

    this.paletteDragHandler = aEvent => {
      let originalTarget = aEvent.originalTarget;
      if (
        this.#isUnwantedDragDrop(aEvent) ||
        this.visiblePalette.contains(originalTarget) ||
        this.$("customization-panelHolder").contains(originalTarget)
      ) {
        return;
      }
      if (aEvent.type == "dragover") {
        this.#onDragOver(aEvent, this.visiblePalette);
      } else {
        this.#onDragDrop(aEvent, this.visiblePalette);
      }
    };
    let contentContainer = this.$("customization-content-container");
    contentContainer.addEventListener(
      "dragover",
      this.paletteDragHandler,
      true
    );
    contentContainer.addEventListener("drop", this.paletteDragHandler, true);
  }

  #teardownPaletteDragging() {
    lazy.DragPositionManager.stop();
    this.#removeCustomizeTargetDragAndDropHandlers(this.visiblePalette);

    let contentContainer = this.$("customization-content-container");
    contentContainer.removeEventListener(
      "dragover",
      this.paletteDragHandler,
      true
    );
    contentContainer.removeEventListener("drop", this.paletteDragHandler, true);
    delete this.paletteDragHandler;
  }

  observe(aSubject, aTopic) {
    switch (aTopic) {
      case "nsPref:changed":
        this.#updateResetButton();
        this.#updateUndoResetButton();
        if (this.#canDrawInTitlebar()) {
          this.#updateTitlebarCheckbox();
        }
        break;
    }
  }

  #canDrawInTitlebar() {
    return this.#window.CustomTitlebar.systemSupported;
  }

  #ensureCustomizationPanels() {
    let template = this.$("customizationPanel");
    template.replaceWith(template.content);

    let wrapper = this.$("customModeWrapper");
    wrapper.replaceWith(wrapper.content);
  }

  #attachEventListeners() {
    let container = this.$("customization-container");

    container.addEventListener("command", event => {
      switch (event.target.id) {
        case "customization-titlebar-visibility-checkbox":
          this.#toggleTitlebar(event.target.checked);
          break;
        case "customization-uidensity-menuitem-compact":
        case "customization-uidensity-menuitem-normal":
        case "customization-uidensity-menuitem-touch":
          this.setUIDensity(event.target.mode);
          break;
        case "customization-uidensity-autotouchmode-checkbox":
          this.#updateAutoTouchMode(event.target.checked);
          break;
        case "whimsy-button":
          this.#togglePong(event.target.checked);
          break;
        case "customization-touchbar-button":
          this.#customizeTouchBar();
          break;
        case "customization-undo-reset-button":
          this.undoReset();
          break;
        case "customization-reset-button":
          this.reset();
          break;
        case "customization-done-button":
          this.exit();
          break;
      }
    });

    container.addEventListener("popupshowing", event => {
      switch (event.target.id) {
        case "customization-toolbar-menu":
          this.#window.ToolbarContextMenu.onViewToolbarsPopupShowing(event);
          break;
        case "customization-uidensity-menu":
          this.#onUIDensityMenuShowing();
          break;
      }
    });

    let updateDensity = event => {
      switch (event.target.id) {
        case "customization-uidensity-menuitem-compact":
        case "customization-uidensity-menuitem-normal":
        case "customization-uidensity-menuitem-touch":
          this.#previewUIDensity(event.target.mode);
      }
    };
    let densityMenu = this.#document.getElementById(
      "customization-uidensity-menu"
    );
    densityMenu.addEventListener("focus", updateDensity);
    densityMenu.addEventListener("mouseover", updateDensity);

    let resetDensity = event => {
      switch (event.target.id) {
        case "customization-uidensity-menuitem-compact":
        case "customization-uidensity-menuitem-normal":
        case "customization-uidensity-menuitem-touch":
          this.#resetUIDensity();
      }
    };
    densityMenu.addEventListener("blur", resetDensity);
    densityMenu.addEventListener("mouseout", resetDensity);

    this.$("customization-uidensity-link").addEventListener("click", () => {
      this.#openUIDensityPreferences();
    });

    this.$("customization-lwtheme-link").addEventListener("click", () => {
      this.#openAddonsManagerThemes();
    });

    this.$(kPaletteItemContextMenu).addEventListener("popupshowing", event => {
      this.#onPaletteContextMenuShowing(event);
    });

    this.$(kPaletteItemContextMenu).addEventListener("command", event => {
      switch (event.target.id) {
        case "customizationPaletteItemContextMenuAddToToolbar":
          this.addToToolbar(
            event.target.parentNode.triggerNode,
            "palette-context"
          );
          break;
        case "customizationPaletteItemContextMenuAddToPanel":
          this.addToPanel(
            event.target.parentNode.triggerNode,
            "palette-context"
          );
          break;
      }
    });

    let autohidePanel = this.$(kDownloadAutohidePanelId);
    autohidePanel.addEventListener("popupshown", event => {
      this._downloadPanelAutoHideTimeout = this.#window.setTimeout(
        () => event.target.hidePopup(),
        4000
      );
    });
    autohidePanel.addEventListener("mouseover", () => {
      this.#window.clearTimeout(this._downloadPanelAutoHideTimeout);
    });
    autohidePanel.addEventListener("mouseout", event => {
      this._downloadPanelAutoHideTimeout = this.#window.setTimeout(
        () => event.target.hidePopup(),
        2000
      );
    });
    autohidePanel.addEventListener("popuphidden", () => {
      this.#window.clearTimeout(this._downloadPanelAutoHideTimeout);
    });

    this.$(kDownloadAutohideCheckboxId).addEventListener("command", event => {
      this.#onDownloadsAutoHideChange(event);
    });
  }

  #updateTitlebarCheckbox() {
    let drawInTitlebar = Services.appinfo.drawInTitlebar;
    let checkbox = this.$("customization-titlebar-visibility-checkbox");
    if (drawInTitlebar) {
      checkbox.removeAttribute("checked");
    } else {
      checkbox.setAttribute("checked", "true");
    }
  }

  #toggleTitlebar(aShouldShowTitlebar) {
    Services.prefs.setIntPref(kDrawInTitlebarPref, !aShouldShowTitlebar);
  }

  #getBoundsWithoutFlushing(element) {
    return this.#window.windowUtils.getBoundsWithoutFlushing(element);
  }

  #onDragStart(aEvent) {
    __dumpDragData(aEvent);
    let item = aEvent.target;
    while (item && item.localName != "toolbarpaletteitem") {
      if (
        item.localName == "toolbar" ||
        item.id == kPaletteId ||
        item.id == "customization-panelHolder"
      ) {
        return;
      }
      item = item.parentNode;
    }

    let draggedItem = item.firstElementChild;
    let placeForItem = CustomizableUI.getPlaceForItem(item);

    let dt = aEvent.dataTransfer;
    let documentId = aEvent.target.ownerDocument.documentElement.id;

    dt.mozSetDataAt(kDragDataTypePrefix + documentId, draggedItem.id, 0);
    dt.effectAllowed = "move";

    let itemRect = this.#getBoundsWithoutFlushing(draggedItem);
    let itemCenter = {
      x: itemRect.left + itemRect.width / 2,
      y: itemRect.top + itemRect.height / 2,
    };
    this._dragOffset = {
      x: aEvent.clientX - itemCenter.x,
      y: aEvent.clientY - itemCenter.y,
    };

    let toolbarParent = draggedItem.closest("toolbar");
    if (toolbarParent) {
      let toolbarRect = this.#getBoundsWithoutFlushing(toolbarParent);
      toolbarParent.style.minHeight = toolbarRect.height + "px";
    }

    gDraggingInToolbars = new Set();

    this._initializeDragAfterMove = () => {
      if (this.#customizing && !this.#transitioning) {
        item.hidden = true;
        lazy.DragPositionManager.start(this.#window);
        let canUsePrevSibling =
          placeForItem == "toolbar" || placeForItem == "panel";
        if (item.nextElementSibling) {
          this.#setDragActive(
            item.nextElementSibling,
            "before",
            draggedItem.id,
            placeForItem
          );
          this.#dragOverItem = item.nextElementSibling;
        } else if (canUsePrevSibling && item.previousElementSibling) {
          this.#setDragActive(
            item.previousElementSibling,
            "after",
            draggedItem.id,
            placeForItem
          );
          this.#dragOverItem = item.previousElementSibling;
        }
        let currentArea = this.#getCustomizableParent(item);
        currentArea.setAttribute("draggingover", "true");
      }
      this._initializeDragAfterMove = null;
      this.#window.clearTimeout(this._dragInitializeTimeout);
    };
    this._dragInitializeTimeout = this.#window.setTimeout(
      this._initializeDragAfterMove,
      0
    );
  }

  #onDragOver(aEvent, aOverrideTarget) {
    if (this.#isUnwantedDragDrop(aEvent)) {
      return;
    }
    if (this._initializeDragAfterMove) {
      this._initializeDragAfterMove();
    }

    __dumpDragData(aEvent);

    let document = aEvent.target.ownerDocument;
    let documentId = document.documentElement.id;
    if (!aEvent.dataTransfer.mozTypesAt(0).length) {
      return;
    }

    let draggedItemId = aEvent.dataTransfer.mozGetDataAt(
      kDragDataTypePrefix + documentId,
      0
    );
    let draggedWrapper = document.getElementById("wrapper-" + draggedItemId);
    let targetArea = this.#getCustomizableParent(
      aOverrideTarget || aEvent.currentTarget
    );
    let originArea = this.#getCustomizableParent(draggedWrapper);

    if (!targetArea || !originArea) {
      return;
    }

    if (
      targetArea.id == kPaletteId &&
      !CustomizableUI.isWidgetRemovable(draggedItemId)
    ) {
      return;
    }

    if (!CustomizableUI.canWidgetMoveToArea(draggedItemId, targetArea.id)) {
      return;
    }

    let targetAreaType = CustomizableUI.getPlaceForItem(targetArea);
    let targetNode = this.#getDragOverNode(
      aEvent,
      targetArea,
      targetAreaType,
      draggedItemId
    );

    let dragOverItem, dragValue;
    if (targetNode == CustomizableUI.getCustomizationTarget(targetArea)) {
      dragOverItem =
        (targetAreaType == "toolbar"
          ? this.#findVisiblePreviousSiblingNode(targetNode.lastElementChild)
          : targetNode.lastElementChild) || targetNode;
      dragValue = "after";
    } else {
      let targetParent = targetNode.parentNode;
      let position = Array.prototype.indexOf.call(
        targetParent.children,
        targetNode
      );
      if (position == -1) {
        dragOverItem =
          targetAreaType == "toolbar"
            ? this.#findVisiblePreviousSiblingNode(targetNode.lastElementChild)
            : targetNode.lastElementChild;
        dragValue = "after";
      } else {
        dragOverItem = targetParent.children[position];
        if (targetAreaType == "toolbar") {
          let itemRect = this.#getBoundsWithoutFlushing(dragOverItem);
          let dropTargetCenter = itemRect.left + itemRect.width / 2;
          let existingDir = dragOverItem.getAttribute("dragover");
          let dirFactor = this.#window.RTL_UI ? -1 : 1;
          if (existingDir == "before") {
            dropTargetCenter +=
              ((parseInt(dragOverItem.style.borderInlineStartWidth) || 0) / 2) *
              dirFactor;
          } else {
            dropTargetCenter -=
              ((parseInt(dragOverItem.style.borderInlineEndWidth) || 0) / 2) *
              dirFactor;
          }
          let before = this.#window.RTL_UI
            ? aEvent.clientX > dropTargetCenter
            : aEvent.clientX < dropTargetCenter;
          dragValue = before ? "before" : "after";
        } else if (targetAreaType == "panel") {
          let itemRect = this.#getBoundsWithoutFlushing(dragOverItem);
          let dropTargetCenter = itemRect.top + itemRect.height / 2;
          let existingDir = dragOverItem.getAttribute("dragover");
          if (existingDir == "before") {
            dropTargetCenter +=
              (parseInt(dragOverItem.style.borderBlockStartWidth) || 0) / 2;
          } else {
            dropTargetCenter -=
              (parseInt(dragOverItem.style.borderBlockEndWidth) || 0) / 2;
          }
          dragValue = aEvent.clientY < dropTargetCenter ? "before" : "after";
        } else {
          dragValue = "before";
        }
      }
    }

    if (this.#dragOverItem && dragOverItem != this.#dragOverItem) {
      this.#cancelDragActive(this.#dragOverItem, dragOverItem);
    }

    if (
      dragOverItem != this.#dragOverItem ||
      dragValue != dragOverItem.getAttribute("dragover")
    ) {
      if (dragOverItem != CustomizableUI.getCustomizationTarget(targetArea)) {
        this.#setDragActive(
          dragOverItem,
          dragValue,
          draggedItemId,
          targetAreaType
        );
      }
      this.#dragOverItem = dragOverItem;
      targetArea.setAttribute("draggingover", "true");
    }

    aEvent.preventDefault();
    aEvent.stopPropagation();
  }

  #onDragDrop(aEvent, aOverrideTarget) {
    if (this.#isUnwantedDragDrop(aEvent)) {
      return;
    }

    __dumpDragData(aEvent);
    this._initializeDragAfterMove = null;
    this.#window.clearTimeout(this._dragInitializeTimeout);

    let targetArea = this.#getCustomizableParent(
      aOverrideTarget || aEvent.currentTarget
    );
    let document = aEvent.target.ownerDocument;
    let documentId = document.documentElement.id;
    let draggedItemId = aEvent.dataTransfer.mozGetDataAt(
      kDragDataTypePrefix + documentId,
      0
    );
    let draggedWrapper = document.getElementById("wrapper-" + draggedItemId);
    let originArea = this.#getCustomizableParent(draggedWrapper);
    if (this.#dragSizeMap) {
      this.#dragSizeMap = new WeakMap();
    }
    if (!targetArea || !originArea) {
      return;
    }
    let targetNode = this.#dragOverItem;
    let dropDir = targetNode.getAttribute("dragover");
    if (targetNode != targetArea && dropDir == "after") {
      if (targetNode.nextElementSibling) {
        targetNode = targetNode.nextElementSibling;
      } else {
        targetNode = targetArea;
      }
    }
    if (targetNode.tagName == "toolbarpaletteitem") {
      targetNode = targetNode.firstElementChild;
    }

    this.#cancelDragActive(this.#dragOverItem, null, true);

    try {
      this.#applyDrop(
        aEvent,
        targetArea,
        originArea,
        draggedItemId,
        targetNode
      );
    } catch (ex) {
      lazy.log.error(ex, ex.stack);
    }

    if (draggedItemId == "downloads-button") {
      Services.prefs.setBoolPref(kDownloadAutoHidePref, false);
      this.#showDownloadsAutoHidePanel();
    }
  }

  #applyDrop(aEvent, aTargetArea, aOriginArea, aDroppedItemId, aTargetNode) {
    let document = aEvent.target.ownerDocument;
    let draggedItem = document.getElementById(aDroppedItemId);
    draggedItem.hidden = false;
    draggedItem.removeAttribute("mousedown");

    let toolbarParent = draggedItem.closest("toolbar");
    if (toolbarParent) {
      toolbarParent.style.removeProperty("min-height");
    }

    if (draggedItem == aTargetNode) {
      return;
    }

    if (!CustomizableUI.canWidgetMoveToArea(aDroppedItemId, aTargetArea.id)) {
      return;
    }

    if (aTargetArea.id == kPaletteId) {
      if (aOriginArea.id !== kPaletteId) {
        if (!CustomizableUI.isWidgetRemovable(aDroppedItemId)) {
          return;
        }

        CustomizableUI.removeWidgetFromArea(aDroppedItemId, "drag");
        if (CustomizableUI.isSpecialWidget(aDroppedItemId)) {
          return;
        }
      }
      draggedItem = draggedItem.parentNode;

      if (aTargetNode == this.visiblePalette) {
        this.visiblePalette.appendChild(draggedItem);
      } else {
        this.visiblePalette.insertBefore(draggedItem, aTargetNode.parentNode);
      }
      this.#onDragEnd(aEvent);
      return;
    }

    let areaCustomizationTarget =
      CustomizableUI.getCustomizationTarget(aTargetArea);
    if (draggedItem.getAttribute("skipintoolbarset") == "true") {
      if (aTargetArea != aOriginArea) {
        return;
      }
      let place = draggedItem.parentNode.getAttribute("place");
      this.unwrapToolbarItem(draggedItem.parentNode);
      if (aTargetNode == areaCustomizationTarget) {
        areaCustomizationTarget.appendChild(draggedItem);
      } else {
        this.unwrapToolbarItem(aTargetNode.parentNode);
        areaCustomizationTarget.insertBefore(draggedItem, aTargetNode);
        this.wrapToolbarItem(aTargetNode, place);
      }
      this.wrapToolbarItem(draggedItem, place);
      return;
    }

    if (
      CustomizableUI.isSpecialWidget(aDroppedItemId) &&
      aOriginArea.id == kPaletteId
    ) {
      aDroppedItemId = aDroppedItemId.match(
        /^customizableui-special-(spring|spacer|separator)/
      )[1];
    }

    if (aTargetNode == areaCustomizationTarget) {
      CustomizableUI.addWidgetToArea(aDroppedItemId, aTargetArea.id);
      this.#onDragEnd(aEvent);
      return;
    }

    let placement;
    let itemForPlacement = aTargetNode;
    while (
      itemForPlacement &&
      itemForPlacement.getAttribute("skipintoolbarset") == "true" &&
      itemForPlacement.parentNode &&
      itemForPlacement.parentNode.nodeName == "toolbarpaletteitem"
    ) {
      itemForPlacement = itemForPlacement.parentNode.nextElementSibling;
      if (
        itemForPlacement &&
        itemForPlacement.nodeName == "toolbarpaletteitem"
      ) {
        itemForPlacement = itemForPlacement.firstElementChild;
      }
    }
    if (itemForPlacement) {
      let targetNodeId =
        itemForPlacement.nodeName == "toolbarpaletteitem"
          ? itemForPlacement.firstElementChild &&
            itemForPlacement.firstElementChild.id
          : itemForPlacement.id;
      placement = CustomizableUI.getPlacementOfWidget(targetNodeId);
    }
    if (!placement) {
      lazy.log.debug(
        "Could not get a position for " +
          aTargetNode.nodeName +
          "#" +
          aTargetNode.id +
          "." +
          aTargetNode.className
      );
    }
    let position = placement ? placement.position : null;

    if (aTargetArea == aOriginArea) {
      CustomizableUI.moveWidgetWithinArea(aDroppedItemId, position);
    } else {
      CustomizableUI.addWidgetToArea(aDroppedItemId, aTargetArea.id, position);
    }

    this.#onDragEnd(aEvent);

    if (aTargetNode != itemForPlacement) {
      let draggedWrapper = draggedItem.parentNode;
      let container = draggedWrapper.parentNode;
      container.insertBefore(draggedWrapper, aTargetNode.parentNode);
    }
  }

  #onDragLeave(aEvent) {
    if (this.#isUnwantedDragDrop(aEvent)) {
      return;
    }

    __dumpDragData(aEvent);

    if (this.#dragOverItem && aEvent.target == aEvent.currentTarget) {
      this.#cancelDragActive(this.#dragOverItem);
      this.#dragOverItem = null;
    }
  }

  #onDragEnd(aEvent) {
    if (this.#isUnwantedDragDrop(aEvent)) {
      return;
    }
    this._initializeDragAfterMove = null;
    this.#window.clearTimeout(this._dragInitializeTimeout);
    __dumpDragData(aEvent, "#onDragEnd");

    let document = aEvent.target.ownerDocument;
    document.documentElement.removeAttribute("customizing-movingItem");

    let documentId = document.documentElement.id;
    if (!aEvent.dataTransfer.mozTypesAt(0)) {
      return;
    }

    let draggedItemId = aEvent.dataTransfer.mozGetDataAt(
      kDragDataTypePrefix + documentId,
      0
    );

    let draggedWrapper = document.getElementById("wrapper-" + draggedItemId);

    if (draggedWrapper) {
      draggedWrapper.hidden = false;
      draggedWrapper.removeAttribute("mousedown");

      let toolbarParent = draggedWrapper.closest("toolbar");
      if (toolbarParent) {
        toolbarParent.style.removeProperty("min-height");
      }
    }

    if (this.#dragOverItem) {
      this.#cancelDragActive(this.#dragOverItem);
      this.#dragOverItem = null;
    }
    lazy.DragPositionManager.stop();
  }

  #isUnwantedDragDrop(aEvent) {
    // The synthesized events for tests generated by synthesizePlainDragAndDrop
    if (this.#skipSourceNodeCheck) {
      return false;
    }

    let mozSourceNode = aEvent.dataTransfer.mozSourceNode;
    return !mozSourceNode || mozSourceNode.documentGlobal != this.#window;
  }

  #setDragActive(aDraggedOverItem, aValue, aDraggedItemId, aPlace) {
    if (!aDraggedOverItem) {
      return;
    }

    if (aDraggedOverItem.getAttribute("dragover") != aValue) {
      aDraggedOverItem.setAttribute("dragover", aValue);

      let window = aDraggedOverItem.documentGlobal;
      let draggedItem = window.document.getElementById(aDraggedItemId);
      if (aPlace == "palette") {
        this.#setGridDragActive(aDraggedOverItem, draggedItem, aValue);
      } else {
        let targetArea = this.#getCustomizableParent(aDraggedOverItem);
        let makeSpaceImmediately = false;
        if (!gDraggingInToolbars.has(targetArea.id)) {
          gDraggingInToolbars.add(targetArea.id);
          let draggedWrapper = this.$("wrapper-" + aDraggedItemId);
          let originArea = this.#getCustomizableParent(draggedWrapper);
          makeSpaceImmediately = originArea == targetArea;
        }
        let propertyToMeasure = aPlace == "toolbar" ? "width" : "height";
        let borderWidth = this.#getDragItemSize(aDraggedOverItem, draggedItem)[
          propertyToMeasure
        ];
        let layoutSide = aPlace == "toolbar" ? "Inline" : "Block";
        let prop, otherProp;
        if (aValue == "before") {
          prop = "border" + layoutSide + "StartWidth";
          otherProp = "border-" + layoutSide.toLowerCase() + "-end-width";
        } else {
          prop = "border" + layoutSide + "EndWidth";
          otherProp = "border-" + layoutSide.toLowerCase() + "-start-width";
        }
        if (makeSpaceImmediately) {
          aDraggedOverItem.setAttribute("notransition", "true");
        }
        aDraggedOverItem.style[prop] = borderWidth + "px";
        aDraggedOverItem.style.removeProperty(otherProp);
        if (makeSpaceImmediately) {
          aDraggedOverItem.getBoundingClientRect();
          aDraggedOverItem.removeAttribute("notransition");
        }
      }
    }
  }

  #cancelDragActive(aDraggedOverItem, aNextDraggedOverItem, aNoTransition) {
    let currentArea = this.#getCustomizableParent(aDraggedOverItem);
    if (!currentArea) {
      return;
    }
    let nextArea = aNextDraggedOverItem
      ? this.#getCustomizableParent(aNextDraggedOverItem)
      : null;
    if (currentArea != nextArea) {
      currentArea.removeAttribute("draggingover");
    }
    let areaType = CustomizableUI.getAreaType(currentArea.id);
    if (areaType) {
      if (aNoTransition) {
        aDraggedOverItem.setAttribute("notransition", "true");
      }
      aDraggedOverItem.removeAttribute("dragover");
      aDraggedOverItem.style.removeProperty("border-inline-start-width");
      aDraggedOverItem.style.removeProperty("border-inline-end-width");
      aDraggedOverItem.style.removeProperty("border-block-start-width");
      aDraggedOverItem.style.removeProperty("border-block-end-width");
      if (aNoTransition) {
        aDraggedOverItem.getBoundingClientRect();
        aDraggedOverItem.removeAttribute("notransition");
      }
    } else {
      aDraggedOverItem.removeAttribute("dragover");
      if (aNextDraggedOverItem) {
        if (nextArea == currentArea) {
          return;
        }
      }
      let positionManager =
        lazy.DragPositionManager.getManagerForArea(currentArea);
      positionManager.clearPlaceholders(currentArea, aNoTransition);
    }
  }

  #setGridDragActive(aDragOverNode, aDraggedItem) {
    let targetArea = this.#getCustomizableParent(aDragOverNode);
    let draggedWrapper = this.$("wrapper-" + aDraggedItem.id);
    let originArea = this.#getCustomizableParent(draggedWrapper);
    let positionManager =
      lazy.DragPositionManager.getManagerForArea(targetArea);
    let draggedSize = this.#getDragItemSize(aDragOverNode, aDraggedItem);
    positionManager.insertPlaceholder(
      targetArea,
      aDragOverNode,
      draggedSize,
      originArea == targetArea
    );
  }

  #getDragItemSize(aDragOverNode, aDraggedItem) {
    if (!this.#dragSizeMap) {
      this.#dragSizeMap = new WeakMap();
    }
    if (!this.#dragSizeMap.has(aDraggedItem)) {
      this.#dragSizeMap.set(aDraggedItem, new WeakMap());
    }
    let itemMap = this.#dragSizeMap.get(aDraggedItem);
    let targetArea = this.#getCustomizableParent(aDragOverNode);
    let currentArea = this.#getCustomizableParent(aDraggedItem);
    let size = itemMap.get(targetArea);
    if (size) {
      return size;
    }

    let currentParent = aDraggedItem.parentNode;
    let currentSibling = aDraggedItem.nextElementSibling;
    const kAreaType = "cui-areatype";
    let areaType, currentType;

    if (targetArea != currentArea) {
      aDragOverNode.parentNode.insertBefore(aDraggedItem, aDragOverNode);
      areaType = CustomizableUI.getAreaType(targetArea.id);
      currentType =
        aDraggedItem.hasAttribute(kAreaType) &&
        aDraggedItem.getAttribute(kAreaType);
      if (areaType) {
        aDraggedItem.setAttribute(kAreaType, areaType);
      }
      this.wrapToolbarItem(aDraggedItem, areaType || "palette");
      CustomizableUI.onWidgetDrag(aDraggedItem.id, targetArea.id);
    } else {
      aDraggedItem.parentNode.hidden = false;
    }

    let rect = aDraggedItem.parentNode.getBoundingClientRect();
    size = { width: rect.width, height: rect.height };
    itemMap.set(targetArea, size);

    if (targetArea != currentArea) {
      this.unwrapToolbarItem(aDraggedItem.parentNode);
      currentParent.insertBefore(aDraggedItem, currentSibling);
      if (areaType) {
        if (currentType === false) {
          aDraggedItem.removeAttribute(kAreaType);
        } else {
          aDraggedItem.setAttribute(kAreaType, currentType);
        }
      }
      this.createOrUpdateWrapper(aDraggedItem, null, true);
      CustomizableUI.onWidgetDrag(aDraggedItem.id);
    } else {
      aDraggedItem.parentNode.hidden = true;
    }
    return size;
  }

  #getCustomizableParent(aElement) {
    if (aElement) {
      let containingPanelHolder = aElement.closest(
        "#customization-panelHolder"
      );
      if (containingPanelHolder) {
        return containingPanelHolder.querySelector(
          "#widget-overflow-fixed-list"
        );
      }
    }

    let areas = CustomizableUI.areas;
    areas.push(kPaletteId);
    return aElement.closest(areas.map(a => "#" + CSS.escape(a)).join(","));
  }

  #getDragOverNode(aEvent, aAreaElement, aPlace) {
    let expectedParent =
      CustomizableUI.getCustomizationTarget(aAreaElement) || aAreaElement;
    if (!expectedParent.contains(aEvent.target)) {
      return expectedParent;
    }
    let dragX = aEvent.clientX - this._dragOffset.x;
    let dragY = aEvent.clientY - this._dragOffset.y;

    let boundsContainer = expectedParent;
    let bounds = this.#getBoundsWithoutFlushing(boundsContainer);
    dragX = Math.min(bounds.right, Math.max(dragX, bounds.left));
    dragY = Math.min(bounds.bottom, Math.max(dragY, bounds.top));

    let targetNode;
    if (aPlace == "toolbar" || aPlace == "panel") {
      targetNode = aAreaElement.ownerDocument.elementFromPoint(dragX, dragY);
      while (targetNode && targetNode.parentNode != expectedParent) {
        targetNode = targetNode.parentNode;
      }
    } else {
      let positionManager =
        lazy.DragPositionManager.getManagerForArea(aAreaElement);
      dragX -= bounds.left;
      dragY -= bounds.top;
      targetNode = positionManager.find(aAreaElement, dragX, dragY);
    }
    return targetNode || aEvent.target;
  }

  #onMouseDown(aEvent) {
    lazy.log.debug("#onMouseDown");
    if (aEvent.button != 0) {
      return;
    }
    let doc = aEvent.target.ownerDocument;
    doc.documentElement.setAttribute("customizing-movingItem", true);
    let item = this.#getWrapper(aEvent.target);
    if (item) {
      item.toggleAttribute("mousedown", true);
    }
  }

  #onMouseUp(aEvent) {
    lazy.log.debug("#onMouseUp");
    if (aEvent.button != 0) {
      return;
    }
    let doc = aEvent.target.ownerDocument;
    doc.documentElement.removeAttribute("customizing-movingItem");
    let item = this.#getWrapper(aEvent.target);
    if (item) {
      item.removeAttribute("mousedown");
    }
  }

  #getWrapper(aElement) {
    while (aElement && aElement.localName != "toolbarpaletteitem") {
      if (aElement.localName == "toolbar") {
        return null;
      }
      aElement = aElement.parentNode;
    }
    return aElement;
  }

  #findVisiblePreviousSiblingNode(aReferenceNode) {
    while (
      aReferenceNode &&
      aReferenceNode.localName == "toolbarpaletteitem" &&
      aReferenceNode.firstElementChild.hidden
    ) {
      aReferenceNode = aReferenceNode.previousElementSibling;
    }
    return aReferenceNode;
  }

  #onPaletteContextMenuShowing(event) {
    let isFlexibleSpace = event.target.triggerNode.id.includes(
      "wrapper-customizableui-special-spring"
    );
    event.target.querySelector(".customize-context-addToPanel").disabled =
      isFlexibleSpace;
  }

  onPanelContextMenuShowing(event) {
    let inPermanentArea = !!event.target.triggerNode.closest(
      "#widget-overflow-fixed-list"
    );
    let doc = event.target.ownerDocument;
    doc.getElementById("customizationPanelItemContextMenuUnpin").hidden =
      !inPermanentArea;
    doc.getElementById("customizationPanelItemContextMenuPin").hidden =
      inPermanentArea;

    doc.documentGlobal.MozXULElement.insertFTLIfNeeded(
      "browser/toolbarContextMenu.ftl"
    );
    event.target.querySelectorAll("[data-lazy-l10n-id]").forEach(el => {
      el.setAttribute("data-l10n-id", el.getAttribute("data-lazy-l10n-id"));
      el.removeAttribute("data-lazy-l10n-id");
    });
  }

  #checkForDownloadsClick(event) {
    if (
      event.target.closest("#wrapper-downloads-button") &&
      event.button == 0
    ) {
      event.view.gCustomizeMode.#showDownloadsAutoHidePanel();
    }
  }

  #setupDownloadAutoHideToggle() {
    this.#window.addEventListener("click", this.#checkForDownloadsClick, true);
  }

  #teardownDownloadAutoHideToggle() {
    this.#window.removeEventListener(
      "click",
      this.#checkForDownloadsClick,
      true
    );
    this.$(kDownloadAutohidePanelId).hidePopup();
  }

  #maybeMoveDownloadsButtonToNavBar() {
    if (
      !CustomizableUI.getPlacementOfWidget("downloads-button") &&
      this.#moveDownloadsButtonToNavBar &&
      this.#window.DownloadsButton.autoHideDownloadsButton
    ) {
      let navbarPlacements = CustomizableUI.getWidgetIdsInArea("nav-bar");
      let insertionPoint = navbarPlacements.indexOf("urlbar-container");
      while (++insertionPoint < navbarPlacements.length) {
        let widget = navbarPlacements[insertionPoint];
        if (
          widget != "search-container" &&
          !(CustomizableUI.isSpecialWidget(widget) && widget.includes("spring"))
        ) {
          break;
        }
      }
      CustomizableUI.addWidgetToArea(
        "downloads-button",
        "nav-bar",
        insertionPoint
      );
    }
  }

  async #showDownloadsAutoHidePanel() {
    let doc = this.#document;
    let panel = doc.getElementById(kDownloadAutohidePanelId);
    panel.hidePopup();
    let button = doc.getElementById("downloads-button");
    if (button.closest("#widget-overflow-fixed-list")) {
      return;
    }

    let offsetX = 0,
      offsetY = 0;
    let panelOnTheLeft = false;
    let toolbarContainer = button.closest("toolbar");
    if (toolbarContainer && toolbarContainer.id == "nav-bar") {
      let navbarWidgets = CustomizableUI.getWidgetIdsInArea("nav-bar");
      if (
        navbarWidgets.indexOf("urlbar-container") <=
        navbarWidgets.indexOf("downloads-button")
      ) {
        panelOnTheLeft = true;
      }
    } else {
      await this.#window.promiseDocumentFlushed(() => {});

      if (!this.#customizing || !this._wantToBeInCustomizeMode) {
        return;
      }
      let buttonBounds = this.#getBoundsWithoutFlushing(button);
      let windowBounds = this.#getBoundsWithoutFlushing(doc.documentElement);
      panelOnTheLeft =
        buttonBounds.left + buttonBounds.width / 2 > windowBounds.width / 2;
    }
    let position;
    if (panelOnTheLeft) {
      position = "topleft topright";
      if (toolbarContainer) {
        offsetX = 8;
      }
    } else {
      position = "topright topleft";
      if (toolbarContainer) {
        offsetX = -8;
      }
    }

    let checkbox = doc.getElementById(kDownloadAutohideCheckboxId);
    if (this.#window.DownloadsButton.autoHideDownloadsButton) {
      checkbox.setAttribute("checked", "true");
    } else {
      checkbox.removeAttribute("checked");
    }

    panel.openPopup(button, position, offsetX, offsetY);
  }

  #onDownloadsAutoHideChange(event) {
    let checkbox = event.target.ownerDocument.getElementById(
      kDownloadAutohideCheckboxId
    );
    Services.prefs.setBoolPref(kDownloadAutoHidePref, checkbox.checked);
    event.view.gCustomizeMode.#moveDownloadsButtonToNavBar = checkbox.checked;
  }

  #customizeTouchBar() {
    let updater = Cc["@mozilla.org/widget/touchbarupdater;1"].getService(
      Ci.nsITouchBarUpdater
    );
    updater.enterCustomizeMode();
  }

  #togglePong(enabled) {
    let whimsyButton = this.$("whimsy-button");
    whimsyButton.checked = enabled;

    if (enabled) {
      this.visiblePalette.setAttribute("whimsypong", "true");
      this.pongArena.hidden = false;
      if (!this.uninitWhimsy) {
        this.uninitWhimsy = this.#whimsypong();
      }
    } else {
      this.visiblePalette.removeAttribute("whimsypong");
      if (this.uninitWhimsy) {
        this.uninitWhimsy();
        this.uninitWhimsy = null;
      }
      this.pongArena.hidden = true;
    }
  }

  #whimsypong() {
    function update() {
      updateBall();
      updatePlayers();
    }

    function updateBall() {
      if (ball[1] <= 0 || ball[1] >= gameSide) {
        if (
          (ball[1] <= 0 && (ball[0] < p1 || ball[0] > p1 + paddleWidth)) ||
          (ball[1] >= gameSide && (ball[0] < p2 || ball[0] > p2 + paddleWidth))
        ) {
          updateScore(ball[1] <= 0 ? 0 : 1);
        } else {
          if (
            (ball[1] <= 0 &&
              (ball[0] - p1 < paddleEdge ||
                p1 + paddleWidth - ball[0] < paddleEdge)) ||
            (ball[1] >= gameSide &&
              (ball[0] - p2 < paddleEdge ||
                p2 + paddleWidth - ball[0] < paddleEdge))
          ) {
            ballDxDy[0] *= Math.random() + 1.3;
            ballDxDy[0] = Math.max(Math.min(ballDxDy[0], 6), -6);
            if (Math.abs(ballDxDy[0]) == 6) {
              ballDxDy[0] += Math.sign(ballDxDy[0]) * Math.random();
            }
          } else {
            ballDxDy[0] /= 1.1;
          }
          ballDxDy[1] *= -1;
          ball[1] = ball[1] <= 0 ? 0 : gameSide;
        }
      }
      ball = [
        Math.max(Math.min(ball[0] + ballDxDy[0], gameSide), 0),
        Math.max(Math.min(ball[1] + ballDxDy[1], gameSide), 0),
      ];
      if (ball[0] <= 0 || ball[0] >= gameSide) {
        ballDxDy[0] *= -1;
      }
    }

    function updatePlayers() {
      if (keydown) {
        let p1Adj = 1;
        if (
          (keydown == 37 && !window.RTL_UI) ||
          (keydown == 39 && window.RTL_UI)
        ) {
          p1Adj = -1;
        }
        p1 += p1Adj * 10 * keydownAdj;
      }

      let sign = Math.sign(ballDxDy[0]);
      if (
        (sign > 0 && ball[0] > p2 + paddleWidth / 2) ||
        (sign < 0 && ball[0] < p2 + paddleWidth / 2)
      ) {
        p2 += sign * 3;
      } else if (
        (sign > 0 && ball[0] > p2 + paddleWidth / 1.1) ||
        (sign < 0 && ball[0] < p2 + paddleWidth / 1.1)
      ) {
        p2 += sign * 9;
      }

      if (score >= winScore) {
        p1 = ball[0];
        p2 = ball[0];
      }
      p1 = Math.max(Math.min(p1, gameSide - paddleWidth), 0);
      p2 = Math.max(Math.min(p2, gameSide - paddleWidth), 0);
    }

    function updateScore(adj) {
      if (adj) {
        score += adj;
      } else if (--lives == 0) {
        quit = true;
      }
      ball = ballDef.slice();
      ballDxDy = ballDxDyDef.slice();
      ballDxDy[1] *= score / winScore + 1;
    }

    function draw() {
      let xAdj = window.RTL_UI ? -1 : 1;
      elements["wp-player1"].style.transform =
        "translate(" + xAdj * p1 + "px, -37px)";
      elements["wp-player2"].style.transform =
        "translate(" + xAdj * p2 + "px, " + gameSide + "px)";
      elements["wp-ball"].style.transform =
        "translate(" + xAdj * ball[0] + "px, " + ball[1] + "px)";
      elements["wp-score"].textContent = score;
      elements["wp-lives"].setAttribute("lives", lives);
      if (score >= winScore) {
        let arena = elements.arena;
        let image = "url(chrome://browser/skin/customizableui/whimsy.png)";
        let position = `${
          (window.RTL_UI ? gameSide : 0) + xAdj * ball[0] - 10
        }px ${ball[1] - 10}px`;
        let repeat = "no-repeat";
        let size = "20px";
        if (arena.style.backgroundImage) {
          if (arena.style.backgroundImage.split(",").length >= 160) {
            quit = true;
          }

          image += ", " + arena.style.backgroundImage;
          position += ", " + arena.style.backgroundPosition;
          repeat += ", " + arena.style.backgroundRepeat;
          size += ", " + arena.style.backgroundSize;
        }
        arena.style.backgroundImage = image;
        arena.style.backgroundPosition = position;
        arena.style.backgroundRepeat = repeat;
        arena.style.backgroundSize = size;
      }
    }

    function onkeydown(event) {
      keys.push(event.which);
      if (keys.length > 10) {
        keys.shift();
        let codeEntered = true;
        for (let i = 0; i < keys.length; i++) {
          if (keys[i] != keysCode[i]) {
            codeEntered = false;
            break;
          }
        }
        if (codeEntered) {
          elements.arena.setAttribute("kcode", "true");
          let spacer = document.querySelector(
            "#customization-palette > toolbarpaletteitem"
          );
          spacer.setAttribute("kcode", "true");
        }
      }
      if (event.which == 37  || event.which == 39 ) {
        keydown = event.which;
        keydownAdj *= 1.05;
      }
    }

    function onkeyup(event) {
      if (event.which == 37 || event.which == 39) {
        keydownAdj = 1;
        keydown = 0;
      }
    }

    function uninit() {
      document.removeEventListener("keydown", onkeydown);
      document.removeEventListener("keyup", onkeyup);
      if (rAFHandle) {
        window.cancelAnimationFrame(rAFHandle);
      }
      let arena = elements.arena;
      while (arena.firstChild) {
        arena.firstChild.remove();
      }
      arena.removeAttribute("score");
      arena.removeAttribute("lives");
      arena.removeAttribute("kcode");
      arena.style.removeProperty("background-image");
      arena.style.removeProperty("background-position");
      arena.style.removeProperty("background-repeat");
      arena.style.removeProperty("background-size");
      let spacer = document.querySelector(
        "#customization-palette > toolbarpaletteitem"
      );
      spacer.removeAttribute("kcode");
      elements = null;
      document = null;
      quit = true;
    }

    if (this.uninitWhimsy) {
      return this.uninitWhimsy;
    }

    let ballDef = [10, 10];
    let ball = [10, 10];
    let ballDxDyDef = [2, 2];
    let ballDxDy = [2, 2];
    let score = 0;
    let p1 = 0;
    let p2 = 10;
    let gameSide = 300;
    let paddleEdge = 30;
    let paddleWidth = 84;
    let keydownAdj = 1;
    let keydown = 0;
    let keys = [];
    let keysCode = [38, 38, 40, 40, 37, 39, 37, 39, 66, 65];
    let lives = 5;
    let winScore = 11;
    let quit = false;
    let document = this.#document;
    let rAFHandle = 0;
    let elements = {
      arena: document.getElementById("customization-pong-arena"),
    };

    document.addEventListener("keydown", onkeydown);
    document.addEventListener("keyup", onkeyup);

    for (let id of ["player1", "player2", "ball", "score", "lives"]) {
      let el = document.createXULElement("box");
      el.id = "wp-" + id;
      elements[el.id] = elements.arena.appendChild(el);
    }

    let spacer = this.visiblePalette.querySelector("toolbarpaletteitem");
    for (let player of ["#wp-player1", "#wp-player2"]) {
      let val = "-moz-element(#" + spacer.id + ") no-repeat";
      elements.arena.querySelector(player).style.background = val;
    }

    let window = this.#window;
    rAFHandle = window.requestAnimationFrame(function animate() {
      update();
      draw();
      if (quit) {
        elements["wp-score"].textContent = score;
        elements["wp-lives"] &&
          elements["wp-lives"].setAttribute("lives", lives);
        elements.arena.setAttribute("score", score);
        elements.arena.setAttribute("lives", lives);
      } else {
        rAFHandle = window.requestAnimationFrame(animate);
      }
    });

    return uninit;
  }
}

function __dumpDragData(aEvent, caller) {
  if (!gDebug) {
    return;
  }
  let str =
    "Dumping drag data (" +
    (caller ? caller + " in " : "") +
    "CustomizeMode.sys.mjs) {\n";
  str += "  type: " + aEvent.type + "\n";
  for (let el of ["target", "currentTarget", "relatedTarget"]) {
    if (aEvent[el]) {
      str +=
        "  " +
        el +
        ": " +
        aEvent[el] +
        "(localName=" +
        aEvent[el].localName +
        "; id=" +
        aEvent[el].id +
        ")\n";
    }
  }
  for (let prop in aEvent.dataTransfer) {
    if (typeof aEvent.dataTransfer[prop] != "function") {
      str +=
        "  dataTransfer[" + prop + "]: " + aEvent.dataTransfer[prop] + "\n";
    }
  }
  str += "}";
  lazy.log.debug(str);
}
