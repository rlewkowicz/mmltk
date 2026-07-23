/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

var { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);

ChromeUtils.defineESModuleGetters(this, {
  PlacesTransactions: "resource://gre/modules/PlacesTransactions.sys.mjs",
  PlacesUIUtils: "moz-src:///browser/components/places/PlacesUIUtils.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

XPCOMUtils.defineLazyScriptGetter(
  this,
  "PlacesTreeView",
  "chrome://browser/content/places/treeView.js"
);
XPCOMUtils.defineLazyScriptGetter(
  this,
  ["PlacesInsertionPoint", "PlacesController", "PlacesControllerDragHelper"],
  "chrome://browser/content/places/controller.js"
);
window.addEventListener("load", () => {
  let uidensity = window.top.document.documentElement.getAttribute("uidensity");
  if (uidensity) {
    document.documentElement.setAttribute("uidensity", uidensity);
  }

  let view = document.getElementById("bookmarks-view");
  view.place =
    "place:type=" + Ci.nsINavHistoryQueryOptions.RESULTS_AS_ROOTS_QUERY;
  view.addEventListener("keypress", event =>
    PlacesUIUtils.onSidebarTreeKeyPress(event)
  );
  view.addEventListener("click", event =>
    PlacesUIUtils.onSidebarTreeClick(event)
  );
  view.addEventListener("mousemove", event =>
    PlacesUIUtils.onSidebarTreeMouseMove(event)
  );
  view.addEventListener("mouseout", () =>
    PlacesUIUtils.setMouseoverURL("", window)
  );

  document
    .getElementById("search-box")
    .addEventListener("MozInputSearch:search", searchBookmarks);

  let bhTooltip = document.getElementById("bhTooltip");
  bhTooltip.addEventListener("popupshowing", event => {
    window.top.BookmarksEventHandler.fillInBHTooltip(bhTooltip, event);
  });
  bhTooltip.addEventListener("popuphiding", () =>
    bhTooltip.removeAttribute("position")
  );

  document
    .getElementById("sidebar-panel-close")
    .addEventListener("click", closeSidebarPanel);

});

function searchBookmarks(event) {
  let { value } = event.currentTarget;

  var tree = document.getElementById("bookmarks-view");
  if (!value) {
    // eslint-disable-next-line no-self-assign
    tree.place = tree.place;
  } else {
    tree.applyFilter(value, PlacesUtils.bookmarks.userContentRoots);
  }
}

window.addEventListener("unload", () => {
  PlacesUIUtils.setMouseoverURL("", window);
  if (window === PlacesUIUtils.lastContextMenuTriggerNode?.documentGlobal) {
    PlacesUIUtils.lastContextMenuTriggerNode = null;
    PlacesUIUtils.lastContextMenuCommand = null;
  }
});

function closeSidebarPanel(e) {
  e.preventDefault();
  let view = e.target.getAttribute("view");
  window.browsingContext.embedderWindowGlobal.browsingContext.window.SidebarController.toggle(
    view
  );
}

window.addEventListener("SidebarFocused", () =>
  document.getElementById("search-box").focus()
);
