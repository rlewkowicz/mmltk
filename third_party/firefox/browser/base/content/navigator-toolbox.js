/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

document.addEventListener(
  "DOMContentLoaded",
  () => {
    const navigatorToolbox = document.getElementById("navigator-toolbox");
    const widgetOverflow = document.getElementById("widget-overflow");

    function onPopupShowing(event) {
      switch (event.target.id) {
        case "PlacesChevronPopup":
          document
            .getElementById("PlacesToolbar")
            ._placesView._onChevronPopupShowing(event);
          break;

        case "BMB_bookmarksPopup":
          BookmarkingUI.onPopupShowing(event);
        case "BMB_bookmarksToolbarPopup":
        case "BMB_unsortedBookmarksPopup":
        case "BMB_mobileBookmarksPopup":
          if (!event.target.parentNode._placesView) {
            let placeMap = {
              BMB_bookmarksPopup: PlacesUtils.bookmarks.menuGuid,
              BMB_bookmarksToolbarPopup: PlacesUtils.bookmarks.toolbarGuid,
              BMB_unsortedBookmarksPopup: PlacesUtils.bookmarks.unfiledGuid,
              BMB_mobileBookmarksPopup: PlacesUtils.bookmarks.mobileGuid,
            };
            new PlacesMenu(event, `place:parent=${placeMap[event.target.id]}`);
          }
          break;
      }
    }
    navigatorToolbox.addEventListener("popupshowing", onPopupShowing);
    widgetOverflow.addEventListener("popupshowing", onPopupShowing);

    function onCommand(event) {
      let element = event.target.closest(`
        #bookmarks-toolbar-button,
        #PlacesToolbar,
        #bookmarks-menu-button,
        #BMB_bookmarksPopup,
        #BMB_searchBookmarks,
        #BMB_viewBookmarksToolbar`);
      if (!element) {
        return;
      }

      switch (element.id) {
        case "bookmarks-toolbar-button":
          PlacesToolbarHelper.onPlaceholderCommand();
          break;

        case "PlacesToolbar":
        case "BMB_bookmarksPopup":
          BookmarksEventHandler.onCommand(event);
          break;

        case "bookmarks-menu-button":
          BookmarkingUI.onCommand(event);
          break;

        case "BMB_searchBookmarks":
          PlacesCommandHook.searchBookmarks();
          break;

        case "BMB_viewBookmarksToolbar":
          BookmarkingUI.toggleBookmarksToolbar("bookmarks-widget");
          break;

        default:
          throw new Error(`Missing case for #${element.id}`);
      }
    }
    navigatorToolbox.addEventListener("command", onCommand);
    widgetOverflow.addEventListener("command", onCommand);

    function onMouseDown(event) {
      let element = event.target.closest(`
        #alltabs-button,
        #pageActionButton,
        #downloads-button,
        #library-button
        `);
      if (!element) {
        return;
      }

      switch (element.id) {
        case "alltabs-button":
          gTabsPanel.showAllTabsPanel(event, "alltabs-button");
          break;

        case "pageActionButton":
          BrowserPageActions.mainButtonClicked(event);
          break;

        case "downloads-button":
          DownloadsIndicatorView.onCommand(event);
          break;

        case "library-button":
          PanelUI.showSubView("appMenu-libraryView", element, event);
          break;

        default:
          throw new Error(`Missing case for #${element.id}`);
      }
    }
    navigatorToolbox.addEventListener("mousedown", onMouseDown);
    widgetOverflow.addEventListener("mousedown", onMouseDown);

    function onMouseUp(event) {
      let element = event.target.closest(`
        #PlacesToolbar,
        #BMB_bookmarksPopup
        `);
      if (!element) {
        return;
      }

      switch (element.id) {
        case "PlacesToolbar":
        case "BMB_bookmarksPopup":
          BookmarksEventHandler.onMouseUp(event);
          break;

        default:
          throw new Error(`Missing case for #${element.id}`);
      }
    }
    navigatorToolbox.addEventListener("mouseup", onMouseUp);
    widgetOverflow.addEventListener("mouseup", onMouseUp);

    function onClick(event) {
      const isLeftClick = event.button === 0;

      let element = event.target.closest(`
        #vertical-tabs-newtab-button,
        #tabs-newtab-button,
        #new-tab-button,
        #back-button,
        #forward-button,
        #reload-button ,
        #urlbar-zoom-button,
        #star-button-box,
        #personal-toolbar-empty-description,
        #home-button,
        #PlacesToolbar,
        #BMB_bookmarksPopup,
        #trust-icon-container,
        #tracking-protection-icon-container,
        #identity-icon-box,
        #identity-permission-box
        `);
      if (!element) {
        return;
      }

      switch (element.id) {
        case "vertical-tabs-newtab-button":
        case "tabs-newtab-button":
        case "new-tab-button":
          gBrowser.handleNewTabMiddleClick(element, event);
          break;

        case "back-button":
        case "forward-button":
        case "reload-button":
          checkForMiddleClick(element, event);
          break;

        case "urlbar-zoom-button":
          if (isLeftClick) {
            FullZoom.resetFromURLBar();
          }
          break;

        case "star-button-box":
          BrowserPageActions.doCommandForAction(
            PageActions.actionForID("bookmark"),
            event,
            element
          );
          break;

        case "personal-toolbar-empty-description":
          if (isLeftClick && event.target.localName == "a") {
            PlacesCommandHook.showPlacesOrganizer("BookmarksToolbar");
          }
          break;

        case "home-button":
          BrowserCommands.home(event);
          break;

        case "PlacesToolbar":
          BookmarksEventHandler.onClick(event, element._placesView);
          break;

        case "BMB_bookmarksPopup":
          BookmarksEventHandler.onClick(event, element.parentNode._placesView);
          break;

        case "trust-icon-container":
          gTrustPanelHandler.handleProtectionsButtonEvent(event);
          break;

        case "tracking-protection-icon-container":
          gProtectionsHandler.handleProtectionsButtonEvent(event);
          break;

        case "identity-icon-box":
          if (UrlbarPrefs.get("trustPanel.featureGate")) {
            gTrustPanelHandler.handleProtectionsButtonEvent(event);
            break;
          }
          gIdentityHandler.handleIdentityButtonEvent(event);
          PageProxyClickHandler(event);
          break;

        case "identity-permission-box":
          gPermissionPanel.handleIdentityButtonEvent(event);
          PageProxyClickHandler(event);
          break;

        default:
          throw new Error(`Missing case for #${element.id}`);
      }
    }
    navigatorToolbox.addEventListener("click", onClick);
    widgetOverflow.addEventListener("click", onClick);

    function onKeyPress(event) {
      const isLikeLeftClick = event.key === "Enter" || event.key === " ";

      let element = event.target.closest(`
        #urlbar-zoom-button,
        #star-button-box,
        #personal-toolbar-empty-description,
        #home-button,
        #tracking-protection-icon-container,
        #trust-icon-container,
        #identity-icon-box,
        #identity-permission-box,
        #alltabs-button,
        #pageActionButton,
        #downloads-button,
        #library-button
      `);
      if (!element) {
        return;
      }

      switch (element.id) {
        case "urlbar-zoom-button":
          if (isLikeLeftClick) {
            FullZoom.resetFromURLBar();
          }
          break;

        case "star-button-box":
          BrowserPageActions.doCommandForAction(
            PageActions.actionForID("bookmark"),
            event,
            element
          );
          break;

        case "personal-toolbar-empty-description":
          if (isLikeLeftClick && event.target.localName == "a") {
            PlacesCommandHook.showPlacesOrganizer("BookmarksToolbar");
          }
          break;

        case "home-button":
          if (isLikeLeftClick) {
            BrowserCommands.home(event);
          }
          break;

        case "tracking-protection-icon-container":
          gProtectionsHandler.handleProtectionsButtonEvent(event);
          break;

        case "trust-icon-container":
          gTrustPanelHandler.handleProtectionsButtonEvent(event);
          break;

        case "identity-icon-box":
          gIdentityHandler.handleIdentityButtonEvent(event);
          break;

        case "identity-permission-box":
          gPermissionPanel.handleIdentityButtonEvent(event);
          break;

        case "alltabs-button":
          gTabsPanel.showAllTabsPanel(event, "alltabs-button");
          break;

        case "pageActionButton":
          BrowserPageActions.mainButtonClicked(event);
          break;

        case "downloads-button":
          DownloadsIndicatorView.onCommand(event);
          break;

        case "library-button":
          PanelUI.showSubView("appMenu-libraryView", element, event);
          break;

        default:
          throw new Error(`Missing case for #${element.id}`);
      }
    }
    navigatorToolbox.addEventListener("keypress", onKeyPress, {
      capture: true,
    });
    widgetOverflow.addEventListener("keypress", onKeyPress, { capture: true });

    function onDragAndDrop(event) {
      let element = event.target.closest(`
        #new-tab-button,
        #downloads-button,
        #new-window-button,
        #bookmarks-menu-button,
        #home-button
      `);
      if (!element) {
        return;
      }

      switch (element.id) {
        case "new-tab-button":
          if (event.type === "dragenter" || event.type === "dragover") {
            ToolbarDropHandler.onDragOver(event);
          } else if (event.type === "drop") {
            ToolbarDropHandler.onDropNewTabButtonObserver(event);
          }
          break;

        case "downloads-button":
          if (event.type === "dragenter" || event.type === "dragover") {
            DownloadsIndicatorView.onDragOver(event);
          } else if (event.type === "drop") {
            DownloadsIndicatorView.onDrop(event);
          }
          break;

        case "new-window-button":
          if (event.type === "dragenter" || event.type === "dragover") {
            ToolbarDropHandler.onDragOver(event);
          } else if (event.type === "drop") {
            ToolbarDropHandler.onDropNewWindowButtonObserver(event);
          }
          break;

        case "bookmarks-menu-button":
          switch (event.type) {
            case "dragenter":
              PlacesMenuDNDHandler.onDragEnter(event);
              break;
            case "dragover":
              PlacesMenuDNDHandler.onDragOver(event);
              break;
            case "dragleave":
              PlacesMenuDNDHandler.onDragLeave(event);
              break;
            case "drop":
              PlacesMenuDNDHandler.onDrop(event);
              break;
          }
          break;

        case "home-button":
          if (event.type === "dragenter" || event.type === "dragover") {
            if (HomePage.locked) {
              return;
            }
            ToolbarDropHandler.onDragOver(event);
            event.dropEffect = "link";
          } else if (event.type == "drop") {
            ToolbarDropHandler.onDropHomeButtonObserver(event);
          }
          break;

        default:
          throw new Error(`Missing case for #${element.id}`);
      }
    }

    navigatorToolbox.addEventListener("dragenter", onDragAndDrop);
    widgetOverflow.addEventListener("dragenter", onDragAndDrop);
    navigatorToolbox.addEventListener("dragover", onDragAndDrop);
    widgetOverflow.addEventListener("dragover", onDragAndDrop);
    navigatorToolbox.addEventListener("dragleave", onDragAndDrop);
    widgetOverflow.addEventListener("dragleave", onDragAndDrop);
    navigatorToolbox.addEventListener("drop", onDragAndDrop);
    widgetOverflow.addEventListener("drop", onDragAndDrop);

    document
      .getElementById("identity-box")
      .addEventListener("dragstart", event => {
        gIdentityHandler.onDragStart(event);
      });
    document
      .getElementById("trust-icon-container")
      .addEventListener("dragstart", event => {
        gIdentityHandler.onDragStart(event);
      });

    let trackingProtectionIconContainer = document.getElementById(
      "tracking-protection-icon-container"
    );
    trackingProtectionIconContainer.addEventListener("focus", () => {
      gProtectionsHandler.onTrackingProtectionIconHoveredOrFocused();
    });
    trackingProtectionIconContainer.addEventListener("mouseover", () => {
      gProtectionsHandler.onTrackingProtectionIconHoveredOrFocused();
    });
  },
  { once: true }
);
