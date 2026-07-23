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

var gHistoryTree;
var gSearchBox;
var gHistoryGrouping = "";

window.addEventListener("load", () => {
  let uidensity = window.top.document.documentElement.getAttribute("uidensity");
  if (uidensity) {
    document.documentElement.setAttribute("uidensity", uidensity);
  }

  gHistoryTree = document.getElementById("historyTree");
  gHistoryTree.addEventListener("click", event =>
    PlacesUIUtils.onSidebarTreeClick(event)
  );
  gHistoryTree.addEventListener("keypress", event =>
    PlacesUIUtils.onSidebarTreeKeyPress(event)
  );
  gHistoryTree.addEventListener("mousemove", event =>
    PlacesUIUtils.onSidebarTreeMouseMove(event)
  );
  gHistoryTree.addEventListener("mouseout", () =>
    PlacesUIUtils.setMouseoverURL("", window)
  );

  gSearchBox = document.getElementById("search-box");
  gSearchBox.addEventListener("MozInputSearch:search", () =>
    searchHistory(gSearchBox.value)
  );

  let viewButton = document.getElementById("viewButton");
  gHistoryGrouping = viewButton.getAttribute("selectedsort");


  if (gHistoryGrouping == "site") {
    document.getElementById("bysite").setAttribute("checked", "true");
  } else if (gHistoryGrouping == "visited") {
    document.getElementById("byvisited").setAttribute("checked", "true");
  } else if (gHistoryGrouping == "lastvisited") {
    document.getElementById("bylastvisited").setAttribute("checked", "true");
  } else if (gHistoryGrouping == "dayandsite") {
    document.getElementById("bydayandsite").setAttribute("checked", "true");
  } else {
    document.getElementById("byday").setAttribute("checked", "true");
  }

  document
    .querySelector("#viewButton > menupopup")
    .addEventListener("command", event => {
      let by = event.target.id.slice(2);
      viewButton.setAttribute("selectedsort", by);
      GroupBy(by);
    });

  let bhTooltip = document.getElementById("bhTooltip");
  bhTooltip.addEventListener("popupshowing", event => {
    window.top.BookmarksEventHandler.fillInBHTooltip(bhTooltip, event);
  });
  bhTooltip.addEventListener("popuphiding", () =>
    bhTooltip.removeAttribute("position")
  );

  searchHistory("");
});

function GroupBy(groupingType) {
  gHistoryGrouping = groupingType;
  searchHistory(gSearchBox.value);
}

function searchHistory(aInput) {
  var query = PlacesUtils.history.getNewQuery();
  var options = PlacesUtils.history.getNewQueryOptions();

  const NHQO = Ci.nsINavHistoryQueryOptions;
  var sortingMode;
  var resultType;

  switch (gHistoryGrouping) {
    case "visited":
      resultType = NHQO.RESULTS_AS_URI;
      sortingMode = NHQO.SORT_BY_VISITCOUNT_DESCENDING;
      break;
    case "lastvisited":
      resultType = NHQO.RESULTS_AS_URI;
      sortingMode = NHQO.SORT_BY_DATE_DESCENDING;
      break;
    case "dayandsite":
      resultType = NHQO.RESULTS_AS_DATE_SITE_QUERY;
      break;
    case "site":
      resultType = NHQO.RESULTS_AS_SITE_QUERY;
      sortingMode = NHQO.SORT_BY_TITLE_ASCENDING;
      break;
    case "day":
    default:
      resultType = NHQO.RESULTS_AS_DATE_QUERY;
      break;
  }

  if (aInput) {
    query.searchTerms = aInput;
    if (gHistoryGrouping != "visited" && gHistoryGrouping != "lastvisited") {
      sortingMode = NHQO.SORT_BY_FRECENCY_DESCENDING;
      resultType = NHQO.RESULTS_AS_URI;
    }
  }

  options.sortingMode = sortingMode;
  options.resultType = resultType;
  options.includeHidden = !!aInput;

  gHistoryTree.load(query, options);

}

window.addEventListener("unload", () => {
  PlacesUIUtils.setMouseoverURL("", window);
});

window.addEventListener("SidebarFocused", () => gSearchBox.focus());
