/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  CustomizableUI:
    "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs",
  OpenInTabsUtils:
    "moz-src:///browser/components/tabbrowser/OpenInTabsUtils.sys.mjs",
  PlacesTransactions: "resource://gre/modules/PlacesTransactions.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});
const ITEM_CHANGED_BATCH_NOTIFICATION_THRESHOLD = 10;

const TAB_DROP_TYPE = "application/x-moz-tabbrowser-tab";

class BookmarkState {
  constructor({
    info,
    tags = "",
    keyword = "",
    isFolder = false,
    children = [],
    autosave = false,
    index,
  }) {
    this._guid = info.itemGuid;
    this._postData = info.postData;
    this._isTagContainer = info.isTag;
    this._bulkTaggingUrls = info.uris?.map(uri => uri.spec);
    this._isFolder = isFolder;
    this._children = children;
    this._autosave = autosave;

    this._originalState = {
      title: this._isTagContainer ? info.tag : info.title,
      uri: info.uri?.spec,
      tags: tags
        .trim()
        .split(/\s*,\s*/)
        .filter(tag => !!tag.length),
      keyword,
      parentGuid: info.parentGuid,
      index,
    };

    this._newState = {};
  }

  async _titleChanged(title) {
    this._newState.title = title;
    await this._maybeSave();
  }

  async _locationChanged(location) {
    this._newState.uri = location;
    await this._maybeSave();
  }

  async _tagsChanged(tags) {
    this._newState.tags = tags;
    await this._maybeSave();
  }

  async _keywordChanged(keyword) {
    this._newState.keyword = keyword;
    await this._maybeSave();
  }

  async _parentGuidChanged(parentGuid) {
    this._newState.parentGuid = parentGuid;
    await this._maybeSave();
  }

  async _maybeSave() {
    if (this._autosave) {
      await this.save();
    }
  }

  async _createBookmark() {
    let transactions = [
      lazy.PlacesTransactions.NewBookmark({
        parentGuid: this.parentGuid,
        tags: this._newState.tags,
        title: this._newState.title ?? this._originalState.title,
        url: this._newState.uri ?? this._originalState.uri,
        index: this._originalState.index,
      }),
    ];
    if (this._newState.keyword) {
      transactions.push(previousResults =>
        lazy.PlacesTransactions.EditKeyword({
          guid: previousResults[0],
          keyword: this._newState.keyword,
          postData: this._postData,
        })
      );
    }
    let results = await lazy.PlacesTransactions.batch(
      transactions,
      "BookmarkState::createBookmark"
    );
    this._guid = results?.[0];
    return this._guid;
  }

  async _createFolder() {
    let transactions = [
      lazy.PlacesTransactions.NewFolder({
        parentGuid: this.parentGuid,
        title: this._newState.title ?? this._originalState.title,
        children: this._children,
        index: this._originalState.index,
        tags: this._newState.tags,
      }),
    ];

    if (this._bulkTaggingUrls) {
      this._appendTagsTransactions({
        transactions,
        newTags: this._newState.tags,
        originalTags: this._originalState.tags,
        urls: this._bulkTaggingUrls,
      });
    }

    let results = await lazy.PlacesTransactions.batch(
      transactions,
      "BookmarkState::save::createFolder"
    );
    this._guid = results[0];
    return this._guid;
  }

  get parentGuid() {
    return this._newState.parentGuid ?? this._originalState.parentGuid;
  }

  async save() {
    if (this._guid === lazy.PlacesUtils.bookmarks.unsavedGuid) {
      return this._isFolder ? this._createFolder() : this._createBookmark();
    }

    if (!Object.keys(this._newState).length) {
      return this._guid;
    }

    if (this._isTagContainer && this._newState.title) {
      await lazy.PlacesTransactions.RenameTag({
        oldTag: this._originalState.title,
        tag: this._newState.title,
      })
        .transact()
        .catch(console.error);
      return this._guid;
    }

    let url = this._newState.uri || this._originalState.uri;
    let transactions = [];

    if (this._newState.uri) {
      transactions.push(
        lazy.PlacesTransactions.EditUrl({
          guid: this._guid,
          url,
        })
      );
    }

    for (const [key, value] of Object.entries(this._newState)) {
      switch (key) {
        case "title":
          transactions.push(
            lazy.PlacesTransactions.EditTitle({
              guid: this._guid,
              title: value,
            })
          );
          break;
        case "tags": {
          this._appendTagsTransactions({
            transactions,
            newTags: value,
            originalTags: this._originalState.tags,
            urls: this._bulkTaggingUrls || [url],
          });
          break;
        }
        case "keyword":
          transactions.push(
            lazy.PlacesTransactions.EditKeyword({
              guid: this._guid,
              keyword: value,
              postData: this._postData,
              oldKeyword: this._originalState.keyword,
            })
          );
          break;
        case "parentGuid":
          transactions.push(
            lazy.PlacesTransactions.Move({
              guid: this._guid,
              newParentGuid: this._newState.parentGuid,
            })
          );
          break;
      }
    }
    if (transactions.length) {
      await lazy.PlacesTransactions.batch(transactions, "BookmarkState::save");
    }

    this._originalState = { ...this._originalState, ...this._newState };
    this._newState = {};
    return this._guid;
  }

  _appendTagsTransactions({
    transactions,
    newTags = [],
    originalTags = [],
    urls,
  }) {
    const addedTags = newTags.filter(tag => !originalTags.includes(tag));
    const removedTags = originalTags.filter(tag => !newTags.includes(tag));
    if (addedTags.length) {
      transactions.push(
        lazy.PlacesTransactions.Tag({
          urls,
          tags: addedTags,
        })
      );
    }
    if (removedTags.length) {
      transactions.push(
        lazy.PlacesTransactions.Untag({
          urls,
          tags: removedTags,
        })
      );
    }
  }
}

export var PlacesUIUtils = {
  BookmarkState,
  LAST_USED_FOLDERS_META_KEY: "bookmarks/lastusedfolders",

  lastContextMenuTriggerNode: null,

  lastContextMenuCommand: null,

  lastBookmarkDialogDeferred: null,


  obfuscateUrlForXulStore(url) {
    if (!url.startsWith("place:")) {
      throw new Error("Method must be used to only obfuscate place: uris!");
    }
    let urlNoProtocol = url.substring(url.indexOf(":") + 1);
    let hashedURL = lazy.PlacesUtils.md5(urlNoProtocol);

    return `place:${hashedURL}`;
  },

  async showBookmarkDialog(aInfo, aParentWindow = null) {
    this.lastBookmarkDialogDeferred = Promise.withResolvers();

    let dialogURL = "chrome://browser/content/places/bookmarkProperties.xhtml";
    let features = "centerscreen,chrome,modal,resizable=no";
    let bookmarkGuid;

    if (!aParentWindow) {
      aParentWindow = Services.wm.getMostRecentWindow(null);
    }

    if (aParentWindow.gDialogBox) {
      await aParentWindow.gDialogBox.open(dialogURL, aInfo);
    } else {
      aParentWindow.openDialog(dialogURL, "", features, aInfo);
    }

    if (aInfo.bookmarkState) {
      bookmarkGuid = await aInfo.bookmarkState.save();
      this.lastBookmarkDialogDeferred.resolve(bookmarkGuid);
      return bookmarkGuid;
    }
    bookmarkGuid = undefined;
    this.lastBookmarkDialogDeferred.resolve(bookmarkGuid);
    return bookmarkGuid;
  },

  async showBookmarkPagesDialog(URIList, hiddenRows = [], win = null) {
    if (!URIList.length) {
      return;
    }

    const bookmarkDialogInfo = { action: "add", hiddenRows };
    if (URIList.length > 1) {
      bookmarkDialogInfo.type = "folder";
      bookmarkDialogInfo.URIList = URIList;
    } else {
      bookmarkDialogInfo.type = "bookmark";
      bookmarkDialogInfo.title = URIList[0].title;
      bookmarkDialogInfo.uri = URIList[0].uri;
    }

    await PlacesUIUtils.showBookmarkDialog(bookmarkDialogInfo, win);
  },

  getViewForNode: function PUIU_getViewForNode(aNode) {
    let node = aNode;

    if (Cu.isDeadWrapper(node)) {
      return null;
    }

    if (node.localName == "panelview" && node._placesView) {
      return node._placesView;
    }

    if (
      node.localName == "menu" &&
      !node._placesNode &&
      node.menupopup._placesView
    ) {
      return node.menupopup._placesView;
    }

    while (Element.isInstance(node)) {
      if (node._placesView) {
        return node._placesView;
      }
      if (
        node.localName == "tree" &&
        node.getAttribute("is") == "places-tree"
      ) {
        return node;
      }

      node = node.parentNode;
    }

    return null;
  },

  getControllerForCommand(win, command) {
    let popupNode = PlacesUIUtils.lastContextMenuTriggerNode;
    if (popupNode) {
      let isManaged = !!popupNode.closest("#managed-bookmarks");
      if (isManaged) {
        return this.managedBookmarksController;
      }
      let view = this.getViewForNode(popupNode);
      if (view && view._contextMenuShown) {
        return view.controllers.getControllerForCommand(command);
      }
    }

    let controller =
      win.top.document.commandDispatcher.getControllerForCommand(command);
    return controller || null;
  },

  updateCommands(win) {
    let controller = this.getControllerForCommand(win, "placesCmd_open");
    for (let command of [
      "placesCmd_open",
      "placesCmd_open:window",
      "placesCmd_open:privatewindow",
      "placesCmd_open:tab",
      "placesCmd_new:folder",
      "placesCmd_new:bookmark",
      "placesCmd_new:separator",
      "placesCmd_show:info",
      "placesCmd_reload",
      "placesCmd_sortBy:name",
      "placesCmd_cut",
      "placesCmd_copy",
      "placesCmd_paste",
      "placesCmd_delete",
      "placesCmd_showInFolder",
    ]) {
      win.goSetCommandEnabled(
        command,
        controller && controller.isCommandEnabled(command)
      );
    }
  },

  doCommand(win, command) {
    let controller = this.getControllerForCommand(win, command);
    if (controller && controller.isCommandEnabled(command)) {
      PlacesUIUtils.lastContextMenuCommand = command;
      controller.doCommand(command);
    }
  },

  markPageAsTyped: function PUIU_markPageAsTyped(aURL) {
    lazy.PlacesUtils.history.markPageAsTyped(
      Services.uriFixup.getFixupURIInfo(aURL).preferredURI
    );
  },

  markPageAsFollowedBookmark: function PUIU_markPageAsFollowedBookmark(aURL) {
    lazy.PlacesUtils.history.markPageAsFollowedBookmark(
      Services.uriFixup.getFixupURIInfo(aURL).preferredURI
    );
  },

  markPageAsFollowedLink: function PUIU_markPageAsFollowedLink(aURL) {
    lazy.PlacesUtils.history.markPageAsFollowedLink(
      Services.uriFixup.getFixupURIInfo(aURL).preferredURI
    );
  },

  async setCharsetForPage(url, charset, window) {
    if (lazy.PrivateBrowsingUtils.isWindowPrivate(window)) {
      return;
    }

    if (charset.toLowerCase() == "utf-8") {
      charset = null;
    }

    await lazy.PlacesUtils.history.update({
      url,
      annotations: new Map([[lazy.PlacesUtils.CHARSET_ANNO, charset]]),
    });
  },

  checkURLSecurity(aURINode, aWindow) {
    if (lazy.PlacesUtils.nodeIsBookmark(aURINode)) {
      return true;
    }

    var uri = Services.io.newURI(aURINode.uri);
    if (uri.schemeIs("javascript") || uri.schemeIs("data")) {
      const [title, errorStr] =
        PlacesUIUtils.promptLocalization.formatValuesSync([
          "places-error-title",
          "places-load-js-data-url-error",
        ]);
      Services.prompt.alert(aWindow, title, errorStr);
      return false;
    }
    return true;
  },

  canUserRemove(aNode) {
    let parentNode = aNode.parent;
    if (!parentNode) {
      return false;
    }

    if (lazy.PlacesUtils.nodeIsQuery(parentNode)) {
      if (lazy.PlacesUtils.nodeIsFolderOrShortcut(aNode)) {
        let guid = lazy.PlacesUtils.getConcreteItemGuid(aNode);
        if (lazy.PlacesUtils.isRootItem(guid)) {
          return false;
        }
      } else if (lazy.PlacesUtils.isVirtualLeftPaneItem(aNode.bookmarkGuid)) {
        return false;
      }
    }

    if (aNode.itemId == -1 || lazy.PlacesUtils.nodeIsQuery(parentNode)) {
      return true;
    }

    return !this.isFolderReadOnly(parentNode);
  },

  isFolderReadOnly(placesNode) {
    if (
      typeof placesNode != "object" ||
      !lazy.PlacesUtils.nodeIsFolderOrShortcut(placesNode)
    ) {
      throw new Error("invalid value for placesNode");
    }

    return (
      lazy.PlacesUtils.getConcreteItemGuid(placesNode) ==
      lazy.PlacesUtils.bookmarks.rootGuid
    );
  },

  openTabset(aItemsToOpen, aEvent, aWindow) {
    if (!aItemsToOpen.length) {
      return;
    }

    let browserWindow = getBrowserWindow(aWindow);
    var urls = [];
    let isPrivate =
      browserWindow && lazy.PrivateBrowsingUtils.isWindowPrivate(browserWindow);
    for (let item of aItemsToOpen) {
      urls.push(item.uri);
      if (isPrivate) {
        continue;
      }

      if (item.isBookmark) {
        this.markPageAsFollowedBookmark(item.uri);
      } else {
        this.markPageAsTyped(item.uri);
      }
    }

    var where = browserWindow
      ? lazy.BrowserUtils.whereToOpenLink(aEvent, false, true)
      : "window";
    if (where == "window") {
      let args = Cc["@mozilla.org/array;1"].createInstance(Ci.nsIMutableArray);
      let stringsToLoad = Cc["@mozilla.org/array;1"].createInstance(
        Ci.nsIMutableArray
      );
      urls.forEach(url =>
        stringsToLoad.appendElement(lazy.PlacesUtils.toISupportsString(url))
      );
      args.appendElement(stringsToLoad);

      let features = "chrome,dialog=no,all";
      if (isPrivate) {
        features += ",private";
      }

      browserWindow = Services.ww.openWindow(
        aWindow,
        AppConstants.BROWSER_CHROME_URL,
        null,
        features,
        args
      );
      return;
    }

    var loadInBackground = where == "tabshifted";
    browserWindow.gBrowser.loadTabs(urls, {
      inBackground: loadInBackground,
      replace: false,
      triggeringPrincipal: Services.scriptSecurityManager.getSystemPrincipal(),
    });
  },

  openMultipleLinksInTabs(nodeOrNodes, event, view) {
    let window = view.ownerWindow;
    let urlsToOpen = [];

    if (lazy.PlacesUtils.nodeIsContainer(nodeOrNodes)) {
      urlsToOpen = lazy.PlacesUtils.getURLsForContainerNode(nodeOrNodes);
    } else {
      for (var i = 0; i < nodeOrNodes.length; i++) {
        if (lazy.PlacesUtils.nodeIsURI(nodeOrNodes[i])) {
          urlsToOpen.push({
            uri: nodeOrNodes[i].uri,
            isBookmark: lazy.PlacesUtils.nodeIsBookmark(nodeOrNodes[i]),
          });
        }
      }
    }
    if (lazy.OpenInTabsUtils.confirmOpenInTabs(urlsToOpen.length, window)) {
      this.openTabset(urlsToOpen, event, window);
    }
  },

  openNodeWithEvent: function PUIU_openNodeWithEvent(aNode, aEvent) {
    let window = aEvent.target.documentGlobal;

    let where = lazy.BrowserUtils.whereToOpenLink(aEvent, false, true);
    if (this.loadBookmarksInTabs && lazy.PlacesUtils.nodeIsBookmark(aNode)) {
      if (where == "current" && !aNode.uri.startsWith("javascript:")) {
        where = "tab";
      }
      let browserWindow = getBrowserWindow(window);
      if (where == "tab" && browserWindow?.gBrowser.selectedTab.isEmpty) {
        where = "current";
      }
    }

    this._openNodeIn(aNode, where, window);
  },

  openNodeIn: function PUIU_openNodeIn(aNode, aWhere, aView, aPrivate) {
    let window = aView.ownerWindow;
    this._openNodeIn(aNode, aWhere, window, { aPrivate });
  },

  _openNodeIn: function PUIU__openNodeIn(
    aNode,
    aWhere,
    aWindow,
    { aPrivate = false, userContextId = undefined } = {}
  ) {
    if (
      aNode &&
      this.checkURLSecurity(aNode, aWindow) &&
      this.isURILike(aNode)
    ) {
      let isBookmark = lazy.PlacesUtils.nodeIsBookmark(aNode);

      if (!lazy.PrivateBrowsingUtils.isWindowPrivate(aWindow)) {
        if (isBookmark) {
          this.markPageAsFollowedBookmark(aNode.uri);
        } else {
          this.markPageAsTyped(aNode.uri);
        }
      } else {
        aPrivate = true;
      }

      const isJavaScriptURL = aNode.uri.startsWith("javascript:");
      aWindow.openTrustedLinkIn(aNode.uri, aWhere, {
        allowPopups: isJavaScriptURL,
        inBackground: this.loadBookmarksInBackground,
        allowInheritPrincipal: isJavaScriptURL,
        private: aPrivate,
        userContextId,
      });
    }
  },

  isURILike(aNode) {
    if (aNode instanceof Ci.nsINavHistoryResultNode) {
      return lazy.PlacesUtils.nodeIsURI(aNode);
    }
    return !!aNode.uri;
  },

  guessUrlSchemeForUI(href) {
    return href.substr(0, href.indexOf(":"));
  },

  getBestTitle: function PUIU_getBestTitle(aNode, aDoNotCutTitle) {
    var title;
    if (!aNode.title && lazy.PlacesUtils.nodeIsURI(aNode)) {
      try {
        var uri = Services.io.newURI(aNode.uri);
        var host = uri.host;
        var fileName = uri.QueryInterface(Ci.nsIURL).fileName;
        if (aDoNotCutTitle) {
          title = host + uri.pathQueryRef;
        } else {
          title =
            host +
            (fileName
              ? (host ? "/" + Services.locale.ellipsis + "/" : "") + fileName
              : uri.pathQueryRef);
        }
      } catch (e) {
        title = "";
      }
    } else {
      title = aNode.title;
    }

    return title || this.promptLocalization.formatValueSync("places-no-title");
  },

  async promiseNodeLikeFromFetchInfo(aFetchInfo) {
    if (aFetchInfo.type == lazy.PlacesUtils.bookmarks.TYPE_SEPARATOR) {
      throw new Error("promiseNodeLike doesn't support separators");
    }

    let parent = {
      bookmarkGuid: aFetchInfo.parentGuid,
      type: Ci.nsINavHistoryResultNode.RESULT_TYPE_FOLDER,
    };

    return Object.freeze({
      bookmarkGuid: aFetchInfo.guid,
      title: aFetchInfo.title,
      uri: aFetchInfo.url !== undefined ? aFetchInfo.url.href : "",

      get type() {
        if (aFetchInfo.type == lazy.PlacesUtils.bookmarks.TYPE_FOLDER) {
          return Ci.nsINavHistoryResultNode.RESULT_TYPE_FOLDER;
        }

        if (!this.uri.length) {
          throw new Error("Unexpected item type");
        }

        if (/^place:/.test(this.uri)) {
          throw new Error("Place URIs are not supported.");
        }

        return Ci.nsINavHistoryResultNode.RESULT_TYPE_URI;
      },

      get parent() {
        return parent;
      },
    });
  },

  async batchUpdatesForNode(resultNode, itemsBeingChanged, functionToWrap) {
    if (!resultNode) {
      return functionToWrap();
    }

    if (itemsBeingChanged > ITEM_CHANGED_BATCH_NOTIFICATION_THRESHOLD) {
      resultNode.onBeginUpdateBatch();
    }

    try {
      return await functionToWrap();
    } finally {
      if (itemsBeingChanged > ITEM_CHANGED_BATCH_NOTIFICATION_THRESHOLD) {
        resultNode.onEndUpdateBatch();
      }
    }
  },

  async handleTransferItems(items, insertionPoint, doCopy, view) {
    let transactions;
    let itemsCount;
    if (insertionPoint.isTag) {
      let urls = items.filter(item => "uri" in item).map(item => item.uri);
      itemsCount = urls.length;
      transactions = [
        lazy.PlacesTransactions.Tag({ urls, tag: insertionPoint.tagName }),
      ];
    } else {
      let insertionIndex = await insertionPoint.getIndex();
      itemsCount = items.length;
      transactions = getTransactionsForTransferItems(
        items,
        insertionIndex,
        insertionPoint.guid,
        !doCopy
      );
    }

    if (!transactions.length) {
      return [];
    }

    let guidsToSelect = await this.batchUpdatesForNode(
      getResultForBatching(view),
      itemsCount,
      async () =>
        lazy.PlacesTransactions.batch(transactions, "handleTransferItems")
    );

    return insertionPoint.isTag ? [] : guidsToSelect.flat();
  },

  onSidebarTreeClick(event) {
    if (event.button == 2) {
      return;
    }

    let tree = event.target.parentNode;
    let cell = tree.getCellAt(event.clientX, event.clientY);
    if (cell.row == -1 || cell.childElt == "twisty") {
      return;
    }

    let win = tree.documentGlobal;
    let rect = tree.getCoordsForCellItem(cell.row, cell.col, "image");
    let isRTL = win.getComputedStyle(tree).direction == "rtl";
    let mouseInGutter = isRTL ? event.clientX > rect.x : event.clientX < rect.x;

    let metaKey =
      AppConstants.platform === "macosx" ? event.metaKey : event.ctrlKey;
    let modifKey = metaKey || event.shiftKey;
    let isContainer = tree.view.isContainer(cell.row);
    let openInTabs =
      isContainer &&
      (event.button == 1 || (event.button == 0 && modifKey)) &&
      lazy.PlacesUtils.hasChildURIs(tree.view.nodeForTreeIndex(cell.row));

    if (event.button == 0 && isContainer && !openInTabs) {
      tree.view.toggleOpenState(cell.row);
    } else if (
      !mouseInGutter &&
      openInTabs &&
      event.originalTarget.localName == "treechildren"
    ) {
      tree.view.selection.select(cell.row);
      this.openMultipleLinksInTabs(tree.selectedNode, event, tree);
    } else if (
      !mouseInGutter &&
      !isContainer &&
      event.originalTarget.localName == "treechildren"
    ) {
      tree.view.selection.select(cell.row);
      this.openNodeWithEvent(tree.selectedNode, event);
    }
  },

  onSidebarTreeKeyPress(event) {
    let node = event.target.selectedNode;
    if (node) {
      if (event.keyCode == event.DOM_VK_RETURN) {
        PlacesUIUtils.openNodeWithEvent(node, event);
      }
    }
  },

  onSidebarTreeMouseMove(event) {
    let treechildren = event.target;
    if (treechildren.localName != "treechildren") {
      return;
    }

    let tree = treechildren.parentNode;
    let cell = tree.getCellAt(event.clientX, event.clientY);

    if (cell.row != -1) {
      let node = tree.view.nodeForTreeIndex(cell.row);
      if (lazy.PlacesUtils.nodeIsURI(node)) {
        this.setMouseoverURL(node.uri, tree.documentGlobal);
        return;
      }
    }
    this.setMouseoverURL("", tree.documentGlobal);
  },

  setMouseoverURL(url, win) {
    if (win.top.XULBrowserWindow) {
      win.top.XULBrowserWindow.setOverLink(url);
    }
  },

  NUM_TOOLBAR_BOOKMARKS_TO_UNHIDE: 3,
  async maybeToggleBookmarkToolbarVisibility(aForceVisible = false) {
    const BROWSER_DOCURL = AppConstants.BROWSER_CHROME_URL;
    let xulStore = Services.xulStore;

    if (
      aForceVisible ||
      !xulStore.hasValue(BROWSER_DOCURL, "PersonalToolbar", "collapsed")
    ) {
      function uncollapseToolbar() {
        Services.obs.notifyObservers(
          null,
          "browser-set-toolbar-visibility",
          JSON.stringify([lazy.CustomizableUI.AREA_BOOKMARKS, "true"])
        );
      }
      let toolbarIsCustomized = xulStore.hasValue(
        BROWSER_DOCURL,
        "PersonalToolbar",
        "currentset"
      );
      if (aForceVisible || toolbarIsCustomized) {
        uncollapseToolbar();
        return;
      }

      let numBookmarksOnToolbar = (
        await lazy.PlacesUtils.bookmarks.fetch(
          lazy.PlacesUtils.bookmarks.toolbarGuid
        )
      ).childCount;
      if (numBookmarksOnToolbar > this.NUM_TOOLBAR_BOOKMARKS_TO_UNHIDE) {
        uncollapseToolbar();
      }
    }
  },

  shouldHideOpenMenuItem(item) {
    if (
      item.hasAttribute("hide-if-disabled-private-browsing") &&
      !lazy.PrivateBrowsingUtils.enabled
    ) {
      return true;
    }

    if (
      item.hasAttribute("hide-if-private-browsing") &&
      lazy.PrivateBrowsingUtils.isWindowPrivate(item.documentGlobal)
    ) {
      return true;
    }

    if (
      item.hasAttribute("hide-if-usercontext-disabled") &&
      !Services.prefs.getBoolPref("privacy.userContext.enabled", false)
    ) {
      return true;
    }

    return false;
  },

  async managedPlacesContextShowing(event) {
    let menupopup = event.target;
    let document = menupopup.ownerDocument;
    let window = menupopup.documentGlobal;
    if (
      menupopup.triggerNode.id == "managed-bookmarks" &&
      !menupopup.triggerNode.menupopup.hasAttribute("hasbeenopened")
    ) {
      await window.PlacesToolbarHelper.populateManagedBookmarks(
        menupopup.triggerNode.menupopup
      );
    }
    Array.from(menupopup.children).forEach(function (child) {
      child.hidden = true;
    });
    this.managedBookmarksController.triggerNode = menupopup.triggerNode;
    let isFolder = menupopup.triggerNode.hasAttribute("container");
    if (isFolder) {
      let openContainerInTabs_menuitem = document.getElementById(
        "placesContext_openContainer:tabs"
      );
      let menuitems = menupopup.triggerNode.menupopup.children;
      let openContainerInTabs = Array.from(menuitems).some(
        menuitem => menuitem.link
      );
      openContainerInTabs_menuitem.disabled = !openContainerInTabs;
      openContainerInTabs_menuitem.hidden = false;
    } else {
      for (let id of [
        "placesContext_open:newtab",
        "placesContext_open:newcontainertab",
        "placesContext_open:newwindow",
        "placesContext_open:newprivatewindow",
      ]) {
        let item = document.getElementById(id);
        item.hidden = this.shouldHideOpenMenuItem(item);
      }
      for (let id of ["placesContext_openSeparator", "placesContext_copy"]) {
        document.getElementById(id).hidden = false;
      }
    }

    event.target.documentGlobal.updateCommands("places");
  },

  placesContextShowing(event) {
    let menupopup =  (event.target);
    if (
      ![
        "placesContext",
        "sidebar-history-context-menu",
        "sidebar-synced-tabs-context-menu",
      ].includes(menupopup.id)
    ) {
      return;
    }

    switch (menupopup.id) {
      case "sidebar-history-context-menu":
      case "sidebar-synced-tabs-context-menu":
        PlacesUIUtils.lastContextMenuTriggerNode =
          menupopup.triggerNode.triggerNode;
        return;
    }

    PlacesUIUtils.lastContextMenuTriggerNode = menupopup.triggerNode;

    if (Services.prefs.getBoolPref("browser.tabs.loadBookmarksInTabs", false)) {
      menupopup.ownerDocument
        .getElementById("placesContext_open")
        .removeAttribute("default");
      menupopup.ownerDocument
        .getElementById("placesContext_open:newtab")
        .setAttribute("default", "true");
    } else {
      menupopup.ownerDocument
        .getElementById("placesContext_open:newtab")
        .removeAttribute("default");
      menupopup.ownerDocument
        .getElementById("placesContext_open")
        .setAttribute("default", "true");
    }

    let isManaged = !!menupopup.triggerNode.closest("#managed-bookmarks");
    if (isManaged) {
      this.managedPlacesContextShowing(event);
      return;
    }
    menupopup._view = this.getViewForNode(menupopup.triggerNode);
    if (!menupopup._view) {
      event.preventDefault();
      return;
    }
    if (!this.openInTabClosesMenu) {
      menupopup.ownerDocument
        .getElementById("placesContext_open:newtab")
        .setAttribute("closemenu", "single");
    }
    if (!menupopup._view.buildContextMenu(menupopup)) {
      event.preventDefault();
    }
  },

  placesContextHiding(event) {
    let menupopup = event.target;
    if (menupopup._view) {
      menupopup._view.destroyContextMenu();
    }

    if (
      [
        "sidebar-history-context-menu",
        "placesContext",
        "sidebar-synced-tabs-context-menu",
        "sidebar-bookmarks-context-menu",
      ].includes(menupopup.id)
    ) {
      PlacesUIUtils.lastContextMenuTriggerNode = null;
      PlacesUIUtils.lastContextMenuCommand = null;
    }
  },

  createContainerTabMenu(event) {
    let window = event.target.documentGlobal;
    return window.createUserContextMenu(event, { isContextMenu: true });
  },

  openInContainerTab(event) {
    PlacesUIUtils.lastContextMenuCommand = "placesCmd_open:newcontainertab";
    let userContextId = parseInt(
      event.target.getAttribute("data-usercontextid")
    );
    let triggerNode = this.lastContextMenuTriggerNode;
    let isManaged = !!triggerNode?.closest("#managed-bookmarks");
    if (isManaged) {
      let window = triggerNode.documentGlobal;
      window.openTrustedLinkIn(triggerNode.link, "tab", { userContextId });
      return;
    }
    let view = this.getViewForNode(triggerNode);
    this._openNodeIn(
      view?.selectedNode || triggerNode,
      "tab",
      view?.ownerWindow || triggerNode.documentGlobal.top,
      {
        userContextId,
      }
    );
  },

  openSelectionInTabs(event) {
    let isManaged =
      !!event.target.parentNode.triggerNode.closest("#managed-bookmarks");
    let controller;
    if (isManaged) {
      controller = this.managedBookmarksController;
    } else {
      controller = PlacesUIUtils.getViewForNode(
        PlacesUIUtils.lastContextMenuTriggerNode
      ).controller;
    }
    controller.openSelectionInTabs(event);
  },

  managedBookmarksController: {
    triggerNode: null,

    openSelectionInTabs(event) {
      let window = event.target.documentGlobal;
      let menuitems = event.target.parentNode.triggerNode.menupopup.children;
      let items = [];
      for (let i = 0; i < menuitems.length; i++) {
        if (menuitems[i].link) {
          let item = {};
          item.uri = menuitems[i].link;
          item.isBookmark = true;
          items.push(item);
        }
      }
      PlacesUIUtils.openTabset(items, event, window);
    },

    isCommandEnabled(command) {
      switch (command) {
        case "placesCmd_copy":
        case "placesCmd_open:window":
        case "placesCmd_open:privatewindow":
        case "placesCmd_open:tab": {
          return true;
        }
      }
      return false;
    },

    doCommand(command) {
      let window = this.triggerNode.documentGlobal;
      switch (command) {
        case "placesCmd_copy": {
          lazy.BrowserUtils.copyLink(
            this.triggerNode.link,
            this.triggerNode.label
          );
          break;
        }
        case "placesCmd_open:privatewindow":
          window.openTrustedLinkIn(this.triggerNode.link, "window", {
            private: true,
          });
          break;
        case "placesCmd_open:window":
          window.openTrustedLinkIn(this.triggerNode.link, "window", {
            private: false,
          });
          break;
        case "placesCmd_open:tab": {
          window.openTrustedLinkIn(this.triggerNode.link, "tab");
        }
      }
    },
  },

  setupSpeculativeConnection(url, window) {
    if (
      !Services.prefs.getBoolPref(
        "browser.places.speculativeConnect.enabled",
        true
      )
    ) {
      return;
    }
    if (!url.startsWith("http")) {
      return;
    }
    try {
      let uri = Services.io.newURI(url);
      Services.io.speculativeConnect(
        uri,
        window.gBrowser.contentPrincipal,
        null,
        false
      );
    } catch (ex) {
    }
  },

  maybeSpeculativeConnectOnMouseDown(event) {
    if (
      event.type == "mousedown" &&
      event.target._placesNode?.uri &&
      event.button != 2
    ) {
      PlacesUIUtils.setupSpeculativeConnection(
        event.target._placesNode.uri,
        event.target.documentGlobal
      );
    }
  },

  getImageURL(icon) {
    try {
      return lazy.PlacesUtils.favicons.getFaviconLinkForIcon(
        Services.io.newURI(icon)
      ).spec;
    } catch (ex) {}
    return lazy.PlacesUtils.favicons.defaultFavicon.spec;
  },

  insertTitleStartDiffs(candidates) {
    function findStartDifference(a, b) {
      let i;
      for (i = PlacesUIUtils.similarTitlesMinChars; i < a.length; i++) {
        if (a[i] != b[i]) {
          return i;
        }
      }
      if (b.length > i) {
        return i;
      }
      return -1;
    }

    let longTitles = new Map();

    for (let candidate of candidates) {
      if (
        !candidate.title ||
        candidate.title.length < this.similarTitlesMinChars
      ) {
        continue;
      }
      let titleBeginning = candidate.title.slice(0, this.similarTitlesMinChars);
      let matches = longTitles.get(titleBeginning);
      if (matches) {
        for (let match of matches) {
          let startDiff = findStartDifference(candidate.title, match.title);
          if (startDiff > 0) {
            candidate.titleDifferentIndex = startDiff;
            if (
              !("titleDifferentIndex" in match) ||
              match.titleDifferentIndex > startDiff
            ) {
              match.titleDifferentIndex = startDiff;
            }
          }
        }

        matches.push(candidate);
      } else {
        longTitles.set(titleBeginning, [candidate]);
      }
    }
  },

};

PlacesUIUtils.canLoadToolbarContentPromise = new Promise(resolve => {
  PlacesUIUtils.unblockToolbars = resolve;
});

ChromeUtils.defineLazyGetter(PlacesUIUtils, "PLACES_FLAVORS", () => {
  return [
    lazy.PlacesUtils.TYPE_X_MOZ_PLACE_CONTAINER,
    lazy.PlacesUtils.TYPE_X_MOZ_PLACE_SEPARATOR,
    lazy.PlacesUtils.TYPE_X_MOZ_PLACE,
  ];
});
ChromeUtils.defineLazyGetter(PlacesUIUtils, "URI_FLAVORS", () => {
  return [
    lazy.PlacesUtils.TYPE_X_MOZ_URL,
    TAB_DROP_TYPE,
    lazy.PlacesUtils.TYPE_PLAINTEXT,
  ];
});
ChromeUtils.defineLazyGetter(PlacesUIUtils, "SUPPORTED_FLAVORS", () => {
  return [...PlacesUIUtils.PLACES_FLAVORS, ...PlacesUIUtils.URI_FLAVORS];
});

ChromeUtils.defineLazyGetter(PlacesUIUtils, "promptLocalization", () => {
  return new Localization(
    ["browser/placesPrompts.ftl", "branding/brand.ftl"],
    true
  );
});

XPCOMUtils.defineLazyPreferenceGetter(
  PlacesUIUtils,
  "similarTitlesMinChars",
  "browser.places.similarTitlesMinChars",
  20
);
XPCOMUtils.defineLazyPreferenceGetter(
  PlacesUIUtils,
  "loadBookmarksInBackground",
  "browser.tabs.loadBookmarksInBackground",
  false
);
XPCOMUtils.defineLazyPreferenceGetter(
  PlacesUIUtils,
  "loadBookmarksInTabs",
  "browser.tabs.loadBookmarksInTabs",
  false
);
XPCOMUtils.defineLazyPreferenceGetter(
  PlacesUIUtils,
  "openInTabClosesMenu",
  "browser.bookmarks.openInTabClosesMenu",
  false
);
XPCOMUtils.defineLazyPreferenceGetter(
  PlacesUIUtils,
  "maxRecentFolders",
  "browser.bookmarks.editDialog.maxRecentFolders",
  7
);

XPCOMUtils.defineLazyPreferenceGetter(
  PlacesUIUtils,
  "defaultParentGuid",
  "browser.bookmarks.defaultLocation",
  "", 
  null,
  async prefValue => {
    if (!prefValue) {
      return lazy.PlacesUtils.bookmarks.toolbarGuid;
    }
    if (["toolbar", "menu", "unfiled"].includes(prefValue)) {
      return lazy.PlacesUtils.bookmarks[prefValue + "Guid"];
    }

    try {
      return await lazy.PlacesUtils.bookmarks
        .fetch({ guid: prefValue })
        .then(bm => bm.guid);
    } catch (ex) {
      return lazy.PlacesUtils.bookmarks.toolbarGuid;
    }
  }
);

function canMoveUnwrappedNode(unwrappedNode) {
  if (
    (unwrappedNode.concreteGuid &&
      lazy.PlacesUtils.isRootItem(unwrappedNode.concreteGuid)) ||
    (unwrappedNode.guid && lazy.PlacesUtils.isRootItem(unwrappedNode.guid))
  ) {
    return false;
  }

  let parentGuid = unwrappedNode.parentGuid;
  if (parentGuid == lazy.PlacesUtils.bookmarks.rootGuid) {
    return false;
  }

  return true;
}

function getResultForBatching(viewOrElement) {
  if (
    viewOrElement &&
    Element.isInstance(viewOrElement) &&
    viewOrElement.id === "placesList"
  ) {
    viewOrElement =
      viewOrElement.ownerDocument.getElementById("placeContent") ||
      viewOrElement;
  }

  if (viewOrElement && viewOrElement.result) {
    return viewOrElement.result;
  }

  return null;
}

function getTransactionsForTransferItems(
  items,
  insertionIndex,
  insertionParentGuid,
  doMove
) {
  let canMove = true;
  for (let item of items) {
    if (!PlacesUIUtils.SUPPORTED_FLAVORS.includes(item.type)) {
      throw new Error(`Unsupported '${item.type}' data type`);
    }

    if (
      !("instanceId" in item) ||
      item.instanceId != lazy.PlacesUtils.instanceId
    ) {
      if (item.type == lazy.PlacesUtils.TYPE_X_MOZ_PLACE_CONTAINER) {
        throw new Error(
          "Can't copy a container from a legacy-transactions build"
        );
      }
      if (PlacesUIUtils.PLACES_FLAVORS.includes(item.type)) {
        console.error(
          "Tried to move an unmovable Places " +
            "node, reverting to a copy operation."
        );
      }

      canMove = false;
    }

    if (doMove && canMove) {
      canMove = canMoveUnwrappedNode(item);
    }
  }

  if (doMove && !canMove) {
    doMove = false;
  }

  if (doMove) {
    return [
      lazy.PlacesTransactions.Move({
        guids: items.map(item => item.itemGuid),
        newParentGuid: insertionParentGuid,
        newIndex: insertionIndex,
      }),
    ];
  }

  return getTransactionsForCopy(items, insertionIndex, insertionParentGuid);
}

function getTransactionsForCopy(items, insertionIndex, insertionParentGuid) {
  let transactions = [];
  let index = insertionIndex;

  for (let item of items) {
    let transaction;
    let guid = item.itemGuid;

    if (
      PlacesUIUtils.PLACES_FLAVORS.includes(item.type) &&
      "instanceId" in item &&
      item.instanceId == lazy.PlacesUtils.instanceId &&
      guid &&
      !lazy.PlacesUtils.bookmarks.isVirtualRootItem(guid) &&
      !lazy.PlacesUtils.isVirtualLeftPaneItem(guid)
    ) {
      transaction = lazy.PlacesTransactions.Copy({
        guid,
        newIndex: index,
        newParentGuid: insertionParentGuid,
      });
    } else if (item.type == lazy.PlacesUtils.TYPE_X_MOZ_PLACE_SEPARATOR) {
      transaction = lazy.PlacesTransactions.NewSeparator({
        index,
        parentGuid: insertionParentGuid,
      });
    } else {
      let title =
        item.type != lazy.PlacesUtils.TYPE_PLAINTEXT ? item.title : item.uri;
      transaction = lazy.PlacesTransactions.NewBookmark({
        index,
        parentGuid: insertionParentGuid,
        title,
        url: item.uri,
      });
    }

    transactions.push(transaction);

    if (index != -1) {
      index++;
    }
  }
  return transactions;
}

function getBrowserWindow(aWindow) {
  return aWindow &&
    aWindow.document.documentElement.getAttribute("windowtype") ==
      "navigator:browser"
    ? aWindow
    : lazy.BrowserWindowTracker.getTopWindow();
}
