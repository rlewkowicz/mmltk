/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { PrivateBrowsingUtils } from "resource://gre/modules/PrivateBrowsingUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  CustomizableUI:
    "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs",
  PanelMultiView:
    "moz-src:///browser/components/customizableui/PanelMultiView.sys.mjs",
  RecentlyClosedTabsAndWindowsMenuUtils:
    "resource:///modules/sessionstore/RecentlyClosedTabsAndWindowsMenuUtils.sys.mjs",
  SessionStore: "resource:///modules/sessionstore/SessionStore.sys.mjs",
  ShortcutUtils: "resource://gre/modules/ShortcutUtils.sys.mjs",
});

const kPrefCustomizationDebug = "browser.uiCustomization.debug";

ChromeUtils.defineLazyGetter(lazy, "log", () => {
  let { ConsoleAPI } = ChromeUtils.importESModule(
    "resource://gre/modules/Console.sys.mjs"
  );
  let debug = Services.prefs.getBoolPref(kPrefCustomizationDebug, false);
  let consoleOptions = {
    maxLogLevel: debug ? "all" : "log",
    prefix: "CustomizableWidgets",
  };
  return new ConsoleAPI(consoleOptions);
});

function setAttributes(aNode, aAttrs) {
  let doc = aNode.ownerDocument;
  for (let [name, value] of Object.entries(aAttrs)) {
    if (!value) {
      if (aNode.hasAttribute(name)) {
        aNode.removeAttribute(name);
      }
    } else {
      if (name == "shortcutId") {
        continue;
      }
      if (name == "label" || name == "tooltiptext") {
        let stringId = typeof value == "string" ? value : name;
        let additionalArgs = [];
        if (aAttrs.shortcutId) {
          let shortcut = doc.getElementById(aAttrs.shortcutId);
          if (shortcut) {
            additionalArgs.push(lazy.ShortcutUtils.prettifyShortcut(shortcut));
          }
        }
        value = lazy.CustomizableUI.getLocalizedProperty(
          { id: aAttrs.id },
          stringId,
          additionalArgs
        );
      }
      aNode.setAttribute(name, value);
    }
  }
}

export const CustomizableWidgets = [
  {
    id: "history-panelmenu",
    type: "view",
    viewId: "PanelUI-history",
    shortcutId: "key_gotoHistory",
    tooltiptext: "history-panelmenu.tooltiptext2",
    recentlyClosedTabsPanel: "appMenu-library-recentlyClosedTabs",
    recentlyClosedWindowsPanel: "appMenu-library-recentlyClosedWindows",
    handleEvent(event) {
      switch (event.type) {
        case "PanelMultiViewHidden":
          this.onPanelMultiViewHidden(event);
          break;
        case "ViewShowing":
          this.onSubViewShowing(event);
          break;
        case "unload":
          this.onWindowUnload(event);
          break;
        case "command": {
          let { target } = event;
          let { PanelUI, PlacesCommandHook } = target.documentGlobal;
          if (target.id == "appMenuRecentlyClosedTabs") {
            PanelUI.showSubView(this.recentlyClosedTabsPanel, target);
          } else if (target.id == "appMenuRecentlyClosedWindows") {
            PanelUI.showSubView(this.recentlyClosedWindowsPanel, target);
          } else if (target.id == "appMenuSearchHistory") {
            PlacesCommandHook.searchHistory();
          }
          break;
        }
        default:
          throw new Error(`Unsupported event for '${this.id}'`);
      }
    },
    onViewShowing(event) {
      if (this._panelMenuView) {
        return;
      }

      let panelview = event.target;
      let document = panelview.ownerDocument;
      let window = document.defaultView;
      const closedTabCount = lazy.SessionStore.getClosedTabCount();

      lazy.PanelMultiView.getViewNode(
        document,
        "appMenuRecentlyClosedTabs"
      ).disabled = closedTabCount == 0;
      lazy.PanelMultiView.getViewNode(
        document,
        "appMenuRecentlyClosedWindows"
      ).disabled = lazy.SessionStore.getClosedWindowCount(window) == 0;

      lazy.PanelMultiView.getViewNode(
        document,
        "appMenu-restoreSession"
      ).hidden = !lazy.SessionStore.canRestoreLastSession;

      let query =
        "place:queryType=" +
        Ci.nsINavHistoryQueryOptions.QUERY_TYPE_HISTORY +
        "&sort=" +
        Ci.nsINavHistoryQueryOptions.SORT_BY_DATE_DESCENDING +
        "&maxResults=42&excludeQueries=1";

      this._panelMenuView = new window.PlacesPanelview(
        query,
        document.getElementById("appMenu_historyMenu"),
        panelview
      );
      lazy.PanelMultiView.getViewNode(
        document,
        this.recentlyClosedTabsPanel
      ).addEventListener("ViewShowing", this);
      lazy.PanelMultiView.getViewNode(
        document,
        this.recentlyClosedWindowsPanel
      ).addEventListener("ViewShowing", this);
      panelview.panelMultiView.addEventListener("PanelMultiViewHidden", this);
      panelview.addEventListener("command", this);
      window.addEventListener("unload", this);
    },
    onViewHiding() {
      lazy.log.debug("History view is being hidden!");
    },
    onPanelMultiViewHidden(event) {
      let panelMultiView = event.target;
      let document = panelMultiView.ownerDocument;
      if (this._panelMenuView) {
        this._panelMenuView.uninit();
        delete this._panelMenuView;
        lazy.PanelMultiView.getViewNode(
          document,
          this.recentlyClosedTabsPanel
        ).removeEventListener("ViewShowing", this);
        lazy.PanelMultiView.getViewNode(
          document,
          this.recentlyClosedWindowsPanel
        ).removeEventListener("ViewShowing", this);
        lazy.PanelMultiView.getViewNode(
          document,
          this.viewId
        ).removeEventListener("command", this);
      }
      panelMultiView.removeEventListener("PanelMultiViewHidden", this);
    },
    onWindowUnload() {
      if (this._panelMenuView) {
        delete this._panelMenuView;
      }
    },
    onSubViewShowing(event) {
      let panelview = event.target;
      let document = event.target.ownerDocument;
      let window = document.defaultView;

      this._panelMenuView.clearAllContents(panelview);

      const utils = lazy.RecentlyClosedTabsAndWindowsMenuUtils;
      const fragment =
        panelview.id == this.recentlyClosedTabsPanel
          ? utils.getTabsFragment(window, "toolbarbutton")
          : utils.getWindowsFragment(window, "toolbarbutton");
      let elementCount = fragment.childElementCount;
      this._panelMenuView._setEmptyPopupStatus(panelview, !elementCount);
      if (!elementCount) {
        return;
      }

      let body = document.createXULElement("vbox");
      body.className = "panel-subview-body";
      body.appendChild(fragment);
      let separator = document.createXULElement("toolbarseparator");
      let footer;
      while (--elementCount >= 0) {
        let element = body.children[elementCount];
        if (element.tagName != "toolbarbutton") {
          continue;
        }
        lazy.CustomizableUI.addShortcut(element);
        if (element.classList.contains("restoreallitem")) {
          footer = element;
        }
      }
      panelview.appendChild(body);
      panelview.appendChild(separator);
      panelview.appendChild(footer);
    },
  },
  {
    id: "save-page-button",
    l10nId: "toolbar-button-save-page",
    shortcutId: "key_savePage",
    onCreated(aNode) {
      aNode.setAttribute("command", "Browser:SavePage");
    },
  },
  {
    id: "find-button",
    shortcutId: "key_find",
    tooltiptext: "find-button.tooltiptext3",
    onCommand(aEvent) {
      let win = aEvent.target.documentGlobal;
      if (win.gLazyFindCommand) {
        win.gLazyFindCommand("onFindCommand");
      }
    },
  },
  {
    id: "open-file-button",
    l10nId: "toolbar-button-open-file",
    shortcutId: "openFileKb",
    onCreated(aNode) {
      aNode.setAttribute("command", "Browser:OpenFile");
    },
  },
  {
    id: "zoom-controls",
    type: "custom",
    tooltiptext: "zoom-controls.tooltiptext2",
    onBuild(aDocument) {
      let buttons = [
        {
          id: "zoom-out-button",
          command: "cmd_fullZoomReduce",
          label: true,
          closemenu: "none",
          tooltiptext: "tooltiptext2",
          shortcutId: "key_fullZoomReduce",
          class: "toolbarbutton-1 toolbarbutton-combined",
        },
        {
          id: "zoom-reset-button",
          command: "cmd_fullZoomReset",
          closemenu: "none",
          tooltiptext: "tooltiptext2",
          shortcutId: "key_fullZoomReset",
          class: "toolbarbutton-1 toolbarbutton-combined",
        },
        {
          id: "zoom-in-button",
          command: "cmd_fullZoomEnlarge",
          closemenu: "none",
          label: true,
          tooltiptext: "tooltiptext2",
          shortcutId: "key_fullZoomEnlarge",
          class: "toolbarbutton-1 toolbarbutton-combined",
        },
      ];

      let node = aDocument.createXULElement("toolbaritem");
      node.setAttribute("id", "zoom-controls");
      node.setAttribute(
        "label",
        lazy.CustomizableUI.getLocalizedProperty(this, "label")
      );
      node.setAttribute(
        "title",
        lazy.CustomizableUI.getLocalizedProperty(this, "tooltiptext")
      );
      node.setAttribute("removable", "true");
      node.classList.add("chromeclass-toolbar-additional");
      node.classList.add("toolbaritem-combined-buttons");

      buttons.forEach(function (aButton, aIndex) {
        if (aIndex != 0) {
          node.appendChild(aDocument.createXULElement("separator"));
        }
        let btnNode = aDocument.createXULElement("toolbarbutton");
        setAttributes(btnNode, aButton);
        node.appendChild(btnNode);
      });
      return node;
    },
  },
  {
    id: "edit-controls",
    type: "custom",
    tooltiptext: "edit-controls.tooltiptext2",
    onBuild(aDocument) {
      let buttons = [
        {
          id: "cut-button",
          command: "cmd_cut",
          label: true,
          tooltiptext: "tooltiptext2",
          shortcutId: "key_cut",
          class: "toolbarbutton-1 toolbarbutton-combined",
        },
        {
          id: "copy-button",
          command: "cmd_copy",
          label: true,
          tooltiptext: "tooltiptext2",
          shortcutId: "key_copy",
          class: "toolbarbutton-1 toolbarbutton-combined",
        },
        {
          id: "paste-button",
          command: "cmd_paste",
          label: true,
          tooltiptext: "tooltiptext2",
          shortcutId: "key_paste",
          class: "toolbarbutton-1 toolbarbutton-combined",
        },
      ];

      let node = aDocument.createXULElement("toolbaritem");
      node.setAttribute("id", "edit-controls");
      node.setAttribute(
        "label",
        lazy.CustomizableUI.getLocalizedProperty(this, "label")
      );
      node.setAttribute(
        "title",
        lazy.CustomizableUI.getLocalizedProperty(this, "tooltiptext")
      );
      node.setAttribute("removable", "true");
      node.classList.add("chromeclass-toolbar-additional");
      node.classList.add("toolbaritem-combined-buttons");

      buttons.forEach(function (aButton, aIndex) {
        if (aIndex != 0) {
          node.appendChild(aDocument.createXULElement("separator"));
        }
        let btnNode = aDocument.createXULElement("toolbarbutton");
        setAttributes(btnNode, aButton);
        node.appendChild(btnNode);
      });

      let listener = {
        onWidgetInstanceRemoved: (aWidgetId, aDoc) => {
          if (aWidgetId != this.id || aDoc != aDocument) {
            return;
          }
          lazy.CustomizableUI.removeListener(listener);
        },
        onWidgetOverflow(aWidgetNode) {
          if (aWidgetNode == node) {
            node.documentGlobal.updateEditUIVisibility();
          }
        },
        onWidgetUnderflow(aWidgetNode) {
          if (aWidgetNode == node) {
            node.documentGlobal.updateEditUIVisibility();
          }
        },
      };
      lazy.CustomizableUI.addListener(listener);

      return node;
    },
  },
  {
    id: "characterencoding-button",
    l10nId: "repair-text-encoding-button",
    onCommand(aEvent) {
      aEvent.view.BrowserCommands.forceEncodingDetection();
    },
  },
];

let preferencesButton = {
  id: "preferences-button",
  l10nId: "toolbar-settings-button",
  onCommand(aEvent) {
    let win = aEvent.target.documentGlobal;
    win.openPreferences(undefined);
  },
};
CustomizableWidgets.push(preferencesButton);

if (PrivateBrowsingUtils.enabled) {
  CustomizableWidgets.push({
    id: "privatebrowsing-button",
    l10nId: "toolbar-button-new-private-window",
    shortcutId: "key_privatebrowsing",
    onCommand(e) {
      let win = e.target.documentGlobal;
      win.OpenBrowserWindow({ private: true });
    },
  });
}

if (Services.prefs.getBoolPref("browser.tabs.groups.alternateMenu", false)) {
  CustomizableWidgets.push({
    id: "tab-groups-button",
    type: "view",
    viewId: "toolbar-tabGroupsListView",
    l10nId: "toolbar-button-tab-groups",
  });
}
