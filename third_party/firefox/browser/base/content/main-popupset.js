/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

document.addEventListener(
  "DOMContentLoaded",
  () => {
    let mainPopupSet = document.getElementById("mainPopupSet");
    // eslint-disable-next-line complexity
    mainPopupSet.addEventListener("command", event => {
      switch (event.target.id) {
        case "context_openANewTab":
          window.focus();
          gBrowser.addAdjacentNewTab(TabContextMenu.contextTab);
          break;
        case "context_moveTabToNewGroup":
          TabContextMenu.moveTabsToNewGroup();
          break;
        case "context_ungroupTab":
          for (const tab of TabContextMenu.contextTabs) {
            if (tab.group) {
              gBrowser.ungroupTab(tab);
            }
          }
          break;
        case "context_reloadTab":
          gBrowser.reloadTab(TabContextMenu.contextTab);
          break;
        case "context_reloadSelectedTabs":
          gBrowser.reloadMultiSelectedTabs();
          break;
        case "context_playTab":
          TabContextMenu.contextTab.resumeDelayedMedia();
          break;
        case "context_playSelectedTabs":
          gBrowser.resumeDelayedMediaOnMultiSelectedTabs(
            TabContextMenu.contextTab
          );
          break;
        case "context_toggleMuteTab":
          TabContextMenu.contextTab.toggleMuteAudio();
          break;
        case "context_toggleMuteSelectedTabs":
          gBrowser.toggleMuteAudioOnMultiSelectedTabs(
            TabContextMenu.contextTab
          );
          break;
        case "context_pinTab":
          gBrowser.pinTab(TabContextMenu.contextTab);
          break;
        case "context_unpinTab":
          gBrowser.unpinTab(TabContextMenu.contextTab);
          break;
        case "context_pinSelectedTabs":
          gBrowser.pinMultiSelectedTabs();
          break;
        case "context_unpinSelectedTabs":
          gBrowser.unpinMultiSelectedTabs();
          break;
        case "context_duplicateTab":
          duplicateTabIn(TabContextMenu.contextTab, "tab");
          break;
        case "context_duplicateTabs":
          TabContextMenu.duplicateSelectedTabs();
          break;
        case "context_bookmarkSelectedTabs":
          PlacesCommandHook.bookmarkTabs(gBrowser.selectedTabs);
          break;
        case "context_bookmarkTab":
          PlacesCommandHook.bookmarkTabs([TabContextMenu.contextTab]);
          break;
        case "context_moveToStart":
          gBrowser.moveTabsToStart(TabContextMenu.contextTab);
          break;
        case "context_moveToEnd":
          gBrowser.moveTabsToEnd(TabContextMenu.contextTab);
          break;
        case "context_openTabInWindow":
          gBrowser.replaceTabsWithWindow(TabContextMenu.contextTab);
          break;
        case "context_selectAllTabs":
          gBrowser.selectAllTabs();
          break;
        case "context_closeTab":
          TabContextMenu.closeContextTabs();
          break;
        case "context_closeDuplicateTabs":
          gBrowser.removeDuplicateTabs(TabContextMenu.contextTab);
          break;
        case "context_closeTabsToTheStart":
          gBrowser.removeTabsToTheStartFrom(TabContextMenu.contextTab);
          break;
        case "context_closeTabsToTheEnd":
          gBrowser.removeTabsToTheEndFrom(TabContextMenu.contextTab);
          break;
        case "context_closeOtherTabs":
          gBrowser.removeAllTabsBut(TabContextMenu.contextTab);
          break;
        case "context_unloadTab":
          TabContextMenu.explicitUnloadTabs();
          break;
        case "context_fullscreenAutohide":
          FullScreen.setAutohide();
          break;
        case "context_fullscreenExit":
          BrowserCommands.fullScreen();
          break;

        case "open-tab-group-context-menu_moveToNewWindow":
          {
            let { tabGroupId } = event.target.parentElement.triggerNode.dataset;
            let tabGroup = gBrowser.getTabGroupById(tabGroupId);
            tabGroup.documentGlobal.gBrowser.replaceGroupWithWindow(tabGroup);
          }
          break;
        case "open-tab-group-context-menu_moveToThisWindow":
          {
            let { tabGroupId } = event.target.parentElement.triggerNode.dataset;
            let otherTabGroup = gBrowser.getTabGroupById(tabGroupId);
            let adoptedTabGroup = gBrowser.adoptTabGroup(otherTabGroup, {
              tabIndex: gBrowser.tabs.length,
            });
            adoptedTabGroup.select();
          }
          break;
        case "open-tab-group-context-menu_delete":
          {
            let { tabGroupId } = event.target.parentElement.triggerNode.dataset;
            let tabGroup = gBrowser.getTabGroupById(tabGroupId);
            tabGroup.documentGlobal.gBrowser.removeTabGroup(tabGroup);
          }
          break;

        case "saved-tab-group-context-menu_openInThisWindow":
          {
            let { tabGroupId } = event.target.parentElement.triggerNode.dataset;
            SessionStore.openSavedTabGroup(tabGroupId, window);
          }
          break;
        case "saved-tab-group-context-menu_openInNewWindow":
          {
            let { tabGroupId } = event.target.parentElement.triggerNode.dataset;
            let tabGroup = SessionStore.openSavedTabGroup(tabGroupId, window);
            gBrowser.replaceGroupWithWindow(tabGroup);
          }
          break;
        case "saved-tab-group-context-menu_delete":
          {
            let { tabGroupId } = event.target.parentElement.triggerNode.dataset;
            SessionStore.forgetSavedTabGroup(tabGroupId);
          }
          break;
        case "editBookmarkPanelDoneButton":
          StarUI.panel.hidePopup();
          break;
        case "editBookmarkPanelRemoveButton":
          StarUI.removeBookmarkButtonCommand();
          break;

        case "toolbar-context-move-to-panel":
          gCustomizeMode.addToPanel(
            event.target.parentNode.triggerNode,
            "toolbar-context-menu"
          );
          break;
        case "toolbar-context-autohide-downloads-button":
          ToolbarContextMenu.onDownloadsAutoHideChange(event);
          break;
        case "toolbar-context-remove-from-toolbar":
          gCustomizeMode.removeFromArea(
            event.target.parentNode.triggerNode,
            "toolbar-context-menu"
          );
          break;
        case "toolbar-context-always-open-downloads-panel":
          ToolbarContextMenu.onDownloadsAlwaysOpenPanelChange(event);
          break;
        case "toolbar-context-reloadSelectedTab":
        case "toolbar-context-reloadSelectedTabs":
          gBrowser.reloadMultiSelectedTabs();
          break;
        case "toolbar-context-bookmarkSelectedTab":
        case "toolbar-context-bookmarkSelectedTabs":
          PlacesCommandHook.bookmarkTabs(gBrowser.selectedTabs);
          break;
        case "toolbar-context-selectAllTabs":
          gBrowser.selectAllTabs();
          break;
        case "toolbar-context-customize":
          gCustomizeMode.enter();
          break;
        case "toolbar-context-full-screen-autohide":
          FullScreen.setAutohide();
          break;
        case "toolbar-context-full-screen-exit":
          BrowserCommands.fullScreen();
          break;

        case "customizationPanelItemContextMenuPin":
          gCustomizeMode.addToPanel(
            event.target.parentNode.triggerNode,
            "panelitem-context"
          );
          break;

        case "customizationPanelItemContextMenuUnpin":
          gCustomizeMode.addToToolbar(
            event.target.parentNode.triggerNode,
            "panelitem-context"
          );
          break;

        case "customizationPanelItemContextMenuRemove":
          gCustomizeMode.removeFromArea(
            event.target.parentNode.triggerNode,
            "panelitem-context"
          );
          break;

      }
    });

    const userContextIcons = document.getElementById("userContext-icons");
    userContextIcons.addEventListener("click", event => {
      if (event.button !== 0) {
        return;
      }
      document
        .getElementById("userContext-indicator-menu")
        .openPopup(userContextIcons, "after_start", 0, 0, false, false, event);
    });


    document
      .getElementById("context_reopenInContainerPopupMenu")
      .addEventListener("command", event => {
        TabContextMenu.reopenInContainer(event);
      });

    for (let menu of [
      document.getElementById("context_moveTabToGroupPopupMenu"),
      document.getElementById("context_moveTabOptions"),
    ]) {
      menu.addEventListener("command", event => {
        if (event.target.id == "context_moveTabToGroupNewGroup") {
          TabContextMenu.moveTabsToNewGroup();
          return;
        }

        const tabGroupId = event.target.getAttribute("tab-group-id");
        if (!tabGroupId) {
          return;
        }
        const group = gBrowser.getTabGroupById(tabGroupId);
        if (group) {
          TabContextMenu.moveTabsToGroup(group);
        }

        if (SessionStore.getSavedTabGroup(tabGroupId)) {
          TabContextMenu.addTabsToSavedGroup(tabGroupId);
        }
      });
    }

    document
      .getElementById("backForwardMenu")
      .addEventListener("command", event => {
        BrowserCommands.gotoHistoryIndex(event);
        event.stopPropagation();
      });



    mainPopupSet.addEventListener("popupshowing", event => {
      switch (event.target.id) {
        case "context_reopenInContainerPopupMenu":
          TabContextMenu.createReopenInContainerMenu(event);
          break;
        case "backForwardMenu":
          FillHistoryMenu(event);
          break;
        case "new-tab-button-popup":
          CreateContainerTabMenu(event);
          break;
        case "toolbar-context-menu":
          ToolbarContextMenu.onViewToolbarsPopupShowing(
            event,
            document.getElementById("viewToolbarsMenuSeparator")
          );
          ToolbarContextMenu.updateDownloadsAutoHide(event.target);
          ToolbarContextMenu.updateDownloadsAlwaysOpenPanel(event.target);

          ToolbarContextMenu.updateCustomizationItemsVisibility(event.target);
          ToolbarContextMenu.hideLeadingSeparatorIfNeeded(event.target);
          break;
        case "tabbrowser-tab-tooltip":
          gBrowser.createTooltip(event);
          break;
        case "dynamic-shortcut-tooltip":
          DynamicShortcutTooltip.updateText(event.target);
          break;
        case "customizationPanelItemContextMenu":
          gCustomizeMode.onPanelContextMenuShowing(event);
          break;
        case "bhTooltip":
          BookmarksEventHandler.fillInBHTooltip(event.target, event);
          break;
      }
    });

    document
      .getElementById("tabContextMenu")
      .addEventListener("popupshowing", event => {
        if (event.target.id == "tabContextMenu") {
          TabContextMenu.updateContextMenu(event.target);
        }
      });

    document
      .getElementById("open-tab-group-context-menu")
      .addEventListener("popupshowing", event => {
        if (event.target.id == "open-tab-group-context-menu") {
          let { tabGroupId } = event.target.triggerNode.dataset;
          let tabGroup = gBrowser.getTabGroupById(tabGroupId);
          let tabGroupIsInThisWindow = tabGroup.ownerDocument == document;
          event.target.querySelector(
            "#open-tab-group-context-menu_moveToThisWindow"
          ).disabled = tabGroupIsInThisWindow;

          let groupAloneInWindow =
            tabGroup.tabs.length ==
            tabGroup.documentGlobal.gBrowser.openTabs.length;
          event.target.querySelector(
            "#open-tab-group-context-menu_moveToNewWindow"
          ).disabled = groupAloneInWindow;
        }
      });


    mainPopupSet.addEventListener("popuphiding", event => {
      switch (event.target.id) {
        case "tabbrowser-tab-tooltip":
        case "bhTooltip":
          event.target.removeAttribute("position");
          break;
      }
    });

  },
  { once: true }
);
