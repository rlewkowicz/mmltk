/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PlacesUIUtils: "moz-src:///browser/components/places/PlacesUIUtils.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SessionStore: "resource:///modules/sessionstore/SessionStore.sys.mjs",
  SessionWindowUI: "resource:///modules/sessionstore/SessionWindowUI.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "l10n", () => {
  return new Localization(["browser/recentlyClosed.ftl"], true);
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "closedTabsFromAllWindowsEnabled",
  "browser.sessionstore.closedTabsFromAllWindows"
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "closedTabsFromClosedWindowsEnabled",
  "browser.sessionstore.closedTabsFromClosedWindows"
);

function getClosedTabGroupsById() {
  const closedTabGroups = lazy.SessionStore.getClosedTabGroups();
  const closedTabGroupsById = new Map();
  closedTabGroups.forEach(tabGroup =>
    closedTabGroupsById.set(tabGroup.id, tabGroup)
  );
  return closedTabGroupsById;
}

export var RecentlyClosedTabsAndWindowsMenuUtils = {
  getTabsFragment(aWindow, aTagName) {
    let doc = aWindow.document;
    const isPrivate = lazy.PrivateBrowsingUtils.isWindowPrivate(aWindow);
    const fragment = doc.createDocumentFragment();
    let isEmpty = true;

    if (
      lazy.SessionStore.getClosedTabCount({
        sourceWindow: aWindow,
      })
    ) {
      isEmpty = false;

      const browserWindows = lazy.closedTabsFromAllWindowsEnabled
        ? lazy.SessionStore.getWindows(aWindow)
        : [aWindow];
      const closedTabSets = [];
      for (const win of browserWindows) {
        closedTabSets.push(lazy.SessionStore.getClosedTabDataForWindow(win));
      }

      if (
        !isPrivate &&
        lazy.closedTabsFromClosedWindowsEnabled &&
        lazy.SessionStore.getClosedTabCountFromClosedWindows()
      ) {
        closedTabSets.push(
          lazy.SessionStore.getClosedTabDataFromClosedWindows()
        );
      }

      const closedTabGroupsById = getClosedTabGroupsById();

      let currentGroupId = null;

      closedTabSets.forEach(tabSet => {
        tabSet.forEach((tab, index) => {
          let groupId = tab.closedInTabGroupId;
          if (groupId && closedTabGroupsById.has(groupId)) {
            if (groupId != currentGroupId) {
              if (aTagName == "menuitem") {
                createTabGroupSubmenu(
                  closedTabGroupsById.get(groupId),
                  index,
                  tab,
                  doc,
                  fragment
                );
              } else {
                createTabGroupSubpanel(
                  closedTabGroupsById.get(groupId),
                  index,
                  tab,
                  doc,
                  fragment
                );
              }

              currentGroupId = groupId;
            } else {
            }
          } else {
            createEntry(aTagName, false, index, tab, doc, tab.title, fragment);
            currentGroupId = null;
          }
        });
      });
    }

    if (!isEmpty) {
      createRestoreAllEntry(
        doc,
        fragment,
        false,
        aTagName == "menuitem"
          ? "recently-closed-menu-reopen-all-tabs"
          : "recently-closed-panel-reopen-all-tabs",
        aTagName
      );
    }
    return fragment;
  },

  getWindowsFragment(aWindow, aTagName) {
    let closedWindowData = lazy.SessionStore.getClosedWindowData();
    let doc = aWindow.document;
    let fragment = doc.createDocumentFragment();
    if (closedWindowData.length) {
      for (let i = 0; i < closedWindowData.length; i++) {
        const { selected, tabs, title } = closedWindowData[i];
        const selectedTab = tabs[selected - 1];
        if (selectedTab) {
          const closedAt = closedWindowData[i].closedAt;
          const tabCount = tabs.length;
          const labelArgs = {
            tabCount,
            winTitle: title,
            closedAt,
          };
          const menuLabel = lazy.l10n.formatValueSync(
            "recently-closed-window-panel-tooltip",
            labelArgs
          );
          let tooltipText = null;
          if (aTagName == "toolbarbutton") {
            tooltipText = menuLabel;
          }
          createEntry(
            aTagName,
            true,
            i,
            selectedTab,
            doc,
            menuLabel,
            fragment,
            tooltipText
          );
        }
      }

      createRestoreAllEntry(
        doc,
        fragment,
        true,
        aTagName == "menuitem"
          ? "recently-closed-menu-reopen-all-windows"
          : "recently-closed-panel-reopen-all-windows",
        aTagName
      );
    }
    return fragment;
  },

  onRestoreAllTabsCommand(aEvent) {
    const currentWindow = aEvent.target.documentGlobal;
    const browserWindows = lazy.closedTabsFromAllWindowsEnabled
      ? lazy.SessionStore.getWindows(currentWindow)
      : [currentWindow];
    const closedTabGroupsById = getClosedTabGroupsById();

    const undoAllInTabData = function (tabData, tabMethod, tabGroupMethod) {
      while (tabData.length) {
        let currentTabGroupId = tabData[0].state.groupId;

        if (currentTabGroupId && closedTabGroupsById.has(currentTabGroupId)) {
          let currentTabGroup = closedTabGroupsById.get(currentTabGroupId);
          let splicedTabs = tabData.splice(0, currentTabGroup.tabs.length);
          tabGroupMethod(splicedTabs);
        } else {
          let splicedTabs = tabData.splice(0, 1);
          tabMethod(splicedTabs[0]);
        }
      }
    };

    for (const sourceWindow of browserWindows) {
      let tabData = lazy.SessionStore.getClosedTabDataForWindow(sourceWindow);

      undoAllInTabData(
        tabData,
        _tabs => {
          lazy.SessionStore.undoCloseTab(sourceWindow, 0, currentWindow);
        },
        tabs => {
          lazy.SessionStore.undoCloseTabGroup(
            sourceWindow,
            tabs[0].state.groupId,
            currentWindow
          );
        }
      );
    }
    if (lazy.closedTabsFromClosedWindowsEnabled) {
      let tabData = lazy.SessionStore.getClosedTabDataFromClosedWindows();

      undoAllInTabData(
        tabData,
        tab => {
          lazy.SessionStore.undoCloseTabFromClosedWindow(
            { sourceClosedId: tab.sourceClosedId },
            tab.closedId,
            currentWindow
          );
        },
        tabs => {
          lazy.SessionStore.undoCloseTabGroup(
            { sourceClosedId: tabs[0].sourceClosedId },
            tabs[0].state.groupId,
            currentWindow
          );
        }
      );
    }
  },

  onRestoreAllWindowsCommand() {
    const closedData = lazy.SessionStore.getClosedWindowData();
    for (const { closedId } of closedData) {
      lazy.SessionStore.undoCloseById(closedId);
    }
  },

  _undoCloseMiddleClick(aEvent) {
    if (aEvent.button != 1) {
      return;
    }
    if (aEvent.originalTarget.hasAttribute("source-closed-id")) {
      lazy.SessionStore.undoClosedTabFromClosedWindow(
        {
          sourceClosedId:
            aEvent.originalTarget.getAttribute("source-closed-id"),
        },
        aEvent.originalTarget.getAttribute("value")
      );
    } else {
      lazy.SessionWindowUI.undoCloseTab(
        aEvent.view,
        aEvent.originalTarget.getAttribute("value"),
        aEvent.originalTarget.getAttribute("source-window-id")
      );
    }
    aEvent.view.gBrowser.moveTabToEnd();
    let ancestorPanel = aEvent.target.closest("panel");
    if (ancestorPanel) {
      ancestorPanel.hidePopup();
    }
  },
};

function setTabGroupColorProperties(element, tabGroup) {
  element.style.setProperty(
    "--tab-group-color",
    `var(--tab-group-${tabGroup.color})`
  );
  element.style.setProperty(
    "--tab-group-color-invert",
    `var(--tab-group-${tabGroup.color}-invert)`
  );
  element.style.setProperty(
    "--tab-group-color-pale",
    `var(--tab-group-${tabGroup.color}-pale)`
  );
  element.style.setProperty(
    "--tab-group-background-color",
    `var(--tab-group-${tabGroup.color})`
  );
}

function createTabGroupSubmenu(
  aTabGroup,
  aIndex,
  aSource,
  aDocument,
  aFragment
) {
  let element = aDocument.createXULElement("menu");
  if (aTabGroup.name) {
    element.setAttribute("label", aTabGroup.name);
  } else {
    aDocument.l10n.setAttributes(element, "tab-context-unnamed-group");
  }

  element.classList.add("menu-iconic", "tab-group-icon");
  setTabGroupColorProperties(element, aTabGroup);

  let menuPopup = aDocument.createXULElement("menupopup");

  aTabGroup.tabs.forEach(tab => {
    createEntry(
      "menuitem",
      false,
      aIndex,
      tab,
      aDocument,
      tab.title,
      menuPopup
    );
    aIndex++;
  });

  menuPopup.appendChild(aDocument.createXULElement("menuseparator"));

  let reopenTabGroupItem = aDocument.createXULElement("menuitem");
  aDocument.l10n.setAttributes(
    reopenTabGroupItem,
    "tab-context-reopen-tab-group"
  );
  reopenTabGroupItem.addEventListener("command", () => {
    lazy.SessionStore.undoCloseTabGroup(aSource, aTabGroup.id);
  });
  menuPopup.appendChild(reopenTabGroupItem);

  element.appendChild(menuPopup);
  aFragment.appendChild(element);
}

function createTabGroupSubpanel(
  aTabGroup,
  aIndex,
  aSource,
  aDocument,
  aFragment
) {
  let element = aDocument.createXULElement("toolbarbutton");
  if (aTabGroup.name) {
    element.setAttribute("label", aTabGroup.name);
  } else {
    aDocument.l10n.setAttributes(element, "tab-context-unnamed-group");
  }

  element.classList.add(
    "subviewbutton",
    "subviewbutton-iconic",
    "subviewbutton-nav",
    "tab-group-icon"
  );
  element.setAttribute("closemenu", "none");
  setTabGroupColorProperties(element, aTabGroup);

  const panelviewId = `closed-tabs-tab-group-${aTabGroup.id}`;
  let panelview = aDocument.getElementById(panelviewId);

  if (panelview) {
    panelview.remove();
  }

  panelview = aDocument.createXULElement("panelview");
  panelview.id = panelviewId;
  let panelBody = aDocument.createXULElement("vbox");
  panelBody.className = "panel-subview-body";

  aTabGroup.tabs.forEach(tab => {
    createEntry(
      "toolbarbutton",
      false,
      aIndex,
      tab,
      aDocument,
      tab.title,
      panelBody
    );
    aIndex++;
  });

  panelview.appendChild(panelBody);
  panelview.appendChild(aDocument.createXULElement("toolbarseparator"));

  let reopenTabGroupItem = aDocument.createXULElement("toolbarbutton");
  aDocument.l10n.setAttributes(
    reopenTabGroupItem,
    "tab-context-reopen-tab-group"
  );
  reopenTabGroupItem.classList.add(
    "reopentabgroupitem",
    "subviewbutton",
    "panel-subview-footer-button"
  );
  reopenTabGroupItem.addEventListener("command", () => {
    lazy.SessionStore.undoCloseTabGroup(aSource, aTabGroup.id);
  });

  panelview.appendChild(reopenTabGroupItem);

  element.addEventListener("command", () => {
    aDocument.documentGlobal.PanelUI.showSubView(panelview.id, element);
  });

  aFragment.appendChild(panelview);
  aFragment.appendChild(element);
}

function createEntry(
  aTagName,
  aIsWindowsFragment,
  aIndex,
  aClosedTab,
  aDocument,
  aMenuLabel,
  aFragment,
  aTooltipText
) {
  let element = aDocument.createXULElement(aTagName);

  element.setAttribute("label", aMenuLabel);
  if (aTooltipText) {
    element.setAttribute("tooltiptext", aTooltipText);
    element.setAttribute("aria-description", aTooltipText);
  }
  if (aClosedTab.image) {
    const iconURL = lazy.PlacesUIUtils.getImageURL(aClosedTab.image);
    element.setAttribute("image", ChromeUtils.encodeURIForSrcset(iconURL));
  }

  if (aIsWindowsFragment) {
    element.addEventListener("command", () =>
      lazy.SessionWindowUI.undoCloseWindow(aIndex)
    );
  } else if (typeof aClosedTab.sourceClosedId == "number") {
    let sourceClosedId = aClosedTab.sourceClosedId;
    element.setAttribute("source-closed-id", sourceClosedId);
    element.setAttribute("value", aClosedTab.closedId);
    element.addEventListener(
      "command",
      () => {
        lazy.SessionStore.undoClosedTabFromClosedWindow(
          { sourceClosedId },
          aClosedTab.closedId
        );
      },
      { once: true }
    );
  } else {
    let sourceWindowId = aClosedTab.sourceWindowId;
    element.setAttribute("value", aIndex);
    element.setAttribute("source-window-id", sourceWindowId);
    element.addEventListener("command", event =>
      lazy.SessionWindowUI.undoCloseTab(
        event.target.documentGlobal,
        aIndex,
        sourceWindowId
      )
    );
  }

  if (aTagName == "menuitem") {
    element.setAttribute(
      "class",
      "menuitem-iconic bookmark-item menuitem-with-favicon"
    );
  } else if (aTagName == "toolbarbutton") {
    element.setAttribute(
      "class",
      "subviewbutton subviewbutton-iconic bookmark-item"
    );
  }

  let tabData;
  tabData = aIsWindowsFragment ? aClosedTab : aClosedTab.state;
  let activeIndex = (tabData.index || tabData.entries.length) - 1;
  if (activeIndex >= 0 && tabData.entries[activeIndex]) {
    element.setAttribute("targetURI", tabData.entries[activeIndex].url);
  }

  if (!aIsWindowsFragment && aTagName != "menuitem") {
    element.addEventListener(
      "click",
      RecentlyClosedTabsAndWindowsMenuUtils._undoCloseMiddleClick
    );
  }

  if (aIndex == 0) {
    element.setAttribute(
      "key",
      aIsWindowsFragment
        ? "key_undoCloseWindow"
        : "key_restoreLastClosedTabOrWindowOrSession"
    );
  }

  aFragment.appendChild(element);
}

function createRestoreAllEntry(
  aDocument,
  aFragment,
  aIsWindowsFragment,
  aRestoreAllLabel,
  aTagName
) {
  let restoreAllElements = aDocument.createXULElement(aTagName);
  restoreAllElements.classList.add("restoreallitem");

  if (aTagName == "toolbarbutton") {
    restoreAllElements.classList.add(
      "subviewbutton",
      "panel-subview-footer-button"
    );
  }

  restoreAllElements.setAttribute(
    "label",
    lazy.l10n.formatValueSync(aRestoreAllLabel)
  );

  restoreAllElements.addEventListener(
    "command",
    aIsWindowsFragment
      ? RecentlyClosedTabsAndWindowsMenuUtils.onRestoreAllWindowsCommand
      : RecentlyClosedTabsAndWindowsMenuUtils.onRestoreAllTabsCommand
  );

  if (aTagName == "menuitem") {
    aFragment.appendChild(aDocument.createXULElement("menuseparator"));
  }

  aFragment.appendChild(restoreAllElements);
}
