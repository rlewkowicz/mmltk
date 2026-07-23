/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  CustomizableUI:
    "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs",
  SessionStore: "resource:///modules/sessionstore/SessionStore.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "gAlwaysOpenPanel",
  "browser.download.alwaysOpenPanel",
  true
);

export var ToolbarContextMenu = {
  updateDownloadsAutoHide(popup) {
    let { document, DownloadsButton } = popup.documentGlobal;
    let checkbox = document.getElementById(
      "toolbar-context-autohide-downloads-button"
    );
    let isDownloads =
      popup.triggerNode &&
      ["downloads-button", "wrapper-downloads-button"].includes(
        popup.triggerNode.id
      );
    checkbox.hidden = !isDownloads;
    checkbox.toggleAttribute(
      "checked",
      DownloadsButton.autoHideDownloadsButton
    );
  },

  onDownloadsAutoHideChange(event) {
    let autoHide = event.target.hasAttribute("checked");
    Services.prefs.setBoolPref("browser.download.autohideButton", autoHide);
  },

  updateDownloadsAlwaysOpenPanel(popup) {
    let { document } = popup.documentGlobal;
    let separator = document.getElementById(
      "toolbarDownloadsAnchorMenuSeparator"
    );
    let checkbox = document.getElementById(
      "toolbar-context-always-open-downloads-panel"
    );
    let isDownloads =
      popup.triggerNode &&
      ["downloads-button", "wrapper-downloads-button"].includes(
        popup.triggerNode.id
      );
    separator.hidden = checkbox.hidden = !isDownloads;
    checkbox.toggleAttribute("checked", lazy.gAlwaysOpenPanel);
  },

  onDownloadsAlwaysOpenPanelChange(event) {
    let alwaysOpen = event.target.hasAttribute("checked");
    Services.prefs.setBoolPref("browser.download.alwaysOpenPanel", alwaysOpen);
  },

  // eslint-disable-next-line complexity
  onViewToolbarsPopupShowing(aEvent, aInsertPoint) {
    var popup = aEvent.target;
    let window = popup.documentGlobal;
    let {
      document,
      BookmarkingUI,
      MozXULElement,
      onViewToolbarCommand,
      showFullScreenViewContextMenuItems,
      gBrowser,
      CustomizationHandler,
      gNavToolbox,
    } = window;

    let toolbarItem = popup.triggerNode;
    while (toolbarItem) {
      let localName = toolbarItem.localName;
      if (localName == "toolbar") {
        toolbarItem = null;
        break;
      }
      if (localName == "toolbarpaletteitem") {
        toolbarItem = toolbarItem.firstElementChild;
        break;
      }
      if (localName == "menupopup") {
        aEvent.preventDefault();
        aEvent.stopPropagation();
        return;
      }
      let parent = toolbarItem.parentElement;
      if (parent) {
        if (
          parent.classList.contains("customization-target") ||
          parent.getAttribute("overflowfortoolbar") || 
          parent.localName == "toolbarpaletteitem" ||
          parent.localName == "toolbar" ||
          parent.id == "vertical-tabs"
        ) {
          break;
        }
      }
      toolbarItem = parent;
    }

    for (var i = popup.children.length - 1; i >= 0; --i) {
      var deadItem = popup.children[i];
      if (deadItem.hasAttribute("toolbarId")) {
        popup.removeChild(deadItem);
      }
    }

    let showTabStripItems = toolbarItem?.id == "tabbrowser-tabs";
    let isVerticalTabStripMenu =
      showTabStripItems && toolbarItem.parentElement.id == "vertical-tabs";

    if (aInsertPoint) {
      aInsertPoint.hidden = isVerticalTabStripMenu;
    }
    document.getElementById("toolbar-context-customize").hidden =
      isVerticalTabStripMenu;

    if (!isVerticalTabStripMenu) {
      MozXULElement.insertFTLIfNeeded("browser/toolbarContextMenu.ftl");
      let firstMenuItem = aInsertPoint || popup.firstElementChild;
      let toolbarNodes = gNavToolbox.querySelectorAll("toolbar");
      for (let toolbar of toolbarNodes) {
        if (!toolbar.hasAttribute("toolbarname")) {
          continue;
        }

        if (toolbar.id == "PersonalToolbar") {
          if (AppConstants.MOZ_PLACES) {
            let menu = BookmarkingUI.buildBookmarksToolbarSubmenu(toolbar);
            popup.insertBefore(menu, firstMenuItem);
          }
        } else {
          let menuItem = document.createXULElement("menuitem");
          menuItem.setAttribute("id", "toggle_" + toolbar.id);
          menuItem.setAttribute("toolbarId", toolbar.id);
          menuItem.setAttribute("type", "checkbox");
          menuItem.setAttribute("label", toolbar.getAttribute("toolbarname"));
          let hidingAttribute =
            toolbar.getAttribute("type") == "menubar"
              ? "autohide"
              : "collapsed";
          menuItem.toggleAttribute(
            "checked",
            !toolbar.hasAttribute(hidingAttribute)
          );
          menuItem.setAttribute("accesskey", toolbar.getAttribute("accesskey"));

          popup.insertBefore(menuItem, firstMenuItem);
          menuItem.addEventListener("command", onViewToolbarCommand);
        }
      }
    }

    let moveToPanel = popup.querySelector(".customize-context-moveToPanel");
    let removeFromToolbar = popup.querySelector(
      ".customize-context-removeFromToolbar"
    );

    let isTitlebarSpacer = toolbarItem?.classList.contains("titlebar-spacer");

    let isMenuBarSpacer =
      toolbarItem?.localName == "spacer" &&
      toolbarItem?.parentElement?.id == "toolbar-menubar";

    showFullScreenViewContextMenuItems(popup);

    let sidebarRevampEnabled = Services.prefs.getBoolPref("sidebar.revamp");
    let showSidebarActions =
      ["tabbrowser-tabs", "sidebar-button"].includes(toolbarItem?.id) ||
      toolbarItem?.localName == "toolbarspring" ||
      isTitlebarSpacer ||
      isMenuBarSpacer;

    let toggleVerticalTabsItem = document.getElementById(
      "toolbar-context-toggle-vertical-tabs"
    );
    toggleVerticalTabsItem.hidden = !showSidebarActions;
    document.l10n.setAttributes(
      toggleVerticalTabsItem,
      gBrowser.tabContainer?.verticalMode
        ? "toolbar-context-turn-off-vertical-tabs"
        : "toolbar-context-turn-on-vertical-tabs"
    );
    document.getElementById("toolbar-context-customize-sidebar").hidden =
      !sidebarRevampEnabled ||
      (toolbarItem?.id != "sidebar-button" &&
        !gBrowser.tabContainer?.verticalMode) ||
      (!["tabbrowser-tabs", "sidebar-button"].includes(toolbarItem?.id) &&
        gBrowser.tabContainer?.verticalMode);
    document.getElementById("sidebarRevampSeparator").hidden =
      !showSidebarActions || isVerticalTabStripMenu;
    document.getElementById("customizationMenuSeparator").hidden =
      toolbarItem?.id == "tabbrowser-tabs" ||
      (toolbarItem?.localName == "toolbarspring" &&
        !CustomizationHandler.isCustomizing()) ||
      isMenuBarSpacer ||
      isTitlebarSpacer;

    if (!moveToPanel || !removeFromToolbar) {
      return;
    }

    for (let node of popup.querySelectorAll(
      'menuitem[contexttype="toolbaritem"]'
    )) {
      node.hidden = showTabStripItems;
    }

    for (let node of popup.querySelectorAll('menuitem[contexttype="tabbar"]')) {
      node.hidden = !showTabStripItems;
    }

    document
      .getElementById("toolbar-context-menu")
      .querySelectorAll("[data-lazy-l10n-id]")
      .forEach(el => {
        el.setAttribute("data-l10n-id", el.getAttribute("data-lazy-l10n-id"));
        el.removeAttribute("data-lazy-l10n-id");
      });

    let menuSeparator = document.getElementById("tabbarItemsMenuSeparator");
    menuSeparator.hidden = false;

    document.getElementById("toolbarNavigatorItemsMenuSeparator").hidden =
      !showTabStripItems;

    let isSpacerItem =
      toolbarItem?.localName.includes("separator") ||
      toolbarItem?.localName.includes("spring") ||
      toolbarItem?.localName.includes("spacer") ||
      toolbarItem?.id.startsWith("customizableui-special");

    let shouldHideCustomizationItems =
      isSpacerItem && !CustomizationHandler.isCustomizing();

    if (shouldHideCustomizationItems) {
      moveToPanel.hidden = true;
      removeFromToolbar.hidden = true;
      menuSeparator.hidden = !showTabStripItems;
    }

    if (toolbarItem?.id != "tabbrowser-tabs") {
      menuSeparator.hidden = true;
    }

    if (showTabStripItems) {
      let multipleTabsSelected = !!gBrowser.multiSelectedTabsCount;
      document.getElementById("toolbar-context-bookmarkSelectedTabs").hidden =
        !multipleTabsSelected;
      document.getElementById("toolbar-context-bookmarkSelectedTab").hidden =
        multipleTabsSelected;
      document.getElementById("toolbar-context-reloadSelectedTabs").hidden =
        !multipleTabsSelected;
      document.getElementById("toolbar-context-reloadSelectedTab").hidden =
        multipleTabsSelected;
      document.getElementById("toolbar-context-selectAllTabs").disabled =
        gBrowser.allTabsSelected();
      let closedCount = lazy.SessionStore.getLastClosedTabCount(window);
      document
        .getElementById("History:UndoCloseTab")
        .toggleAttribute("disabled", closedCount == 0);
      document.l10n.setArgs(
        document.getElementById("toolbar-context-undoCloseTab"),
        { tabCount: closedCount }
      );
      return;
    }

    let movable =
      toolbarItem?.id && lazy.CustomizableUI.isWidgetRemovable(toolbarItem);
    moveToPanel.toggleAttribute(
      "disabled",
      !movable || lazy.CustomizableUI.isSpecialWidget(toolbarItem.id)
    );
    removeFromToolbar.toggleAttribute(
      "disabled",
      !movable || shouldHideCustomizationItems
    );
  },

  hideLeadingSeparatorIfNeeded(popup) {
    let firstVisibleElement = popup.firstElementChild;
    while (firstVisibleElement && firstVisibleElement.hidden) {
      firstVisibleElement = firstVisibleElement.nextElementSibling;
    }

    if (
      firstVisibleElement &&
      firstVisibleElement.localName === "menuseparator"
    ) {
      firstVisibleElement.hidden = true;
    }
  },

  updateCustomizationItemsVisibility(popup) {
    let moveToPanel = popup.querySelector(".customize-context-moveToPanel");
    let removeFromToolbar = popup.querySelector(
      ".customize-context-removeFromToolbar"
    );

    if (
      removeFromToolbar?.hasAttribute("disabled") &&
      moveToPanel.hasAttribute("disabled")
    ) {
      removeFromToolbar.hidden = true;
      moveToPanel.hidden = true;
    }
  },
};
