/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  Bookmarks: "resource://gre/modules/Bookmarks.sys.mjs",
  History: "resource://gre/modules/History.sys.mjs",
  Sqlite: "resource://gre/modules/Sqlite.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "MOZ_ACTION_REGEX", () => {
  return /^moz-action:([^,]+),(.*)$/;
});

ChromeUtils.defineLazyGetter(lazy, "CryptoHash", () => {
  return Components.Constructor(
    "@mozilla.org/security/hash;1",
    "nsICryptoHash",
    "initWithString"
  );
});


const NEWLINE = AppConstants.platform == "macosx" ? "\n" : "\r\n";

const TIMERS_RESOLUTION_SKEW_MS = 16;

function QI_node(aNode, aIID) {
  try {
    return aNode.QueryInterface(aIID);
  } catch (ex) {}
  return null;
}
function asContainer(aNode) {
  return QI_node(aNode, Ci.nsINavHistoryContainerResultNode);
}
function asQuery(aNode) {
  return QI_node(aNode, Ci.nsINavHistoryQueryResultNode);
}

async function notifyKeywordChange(url, keyword, source) {
  let bookmarks = [];
  await PlacesUtils.bookmarks.fetch({ url }, b => bookmarks.push(b), {
    includeItemIds: true,
  });

  const notifications = bookmarks.map(
    bookmark =>
      new PlacesBookmarkKeyword({
        id: bookmark.itemId,
        itemType: bookmark.type,
        url,
        guid: bookmark.guid,
        parentGuid: bookmark.parentGuid,
        keyword,
        lastModified: bookmark.lastModified,
        source,
        isTagging: false,
      })
  );
  if (notifications.length) {
    PlacesObservers.notifyListeners(notifications);
  }
}

function serializeNode(aNode) {
  let data = {};

  data.title = aNode.title;
  data.id = aNode.itemId;
  data.itemGuid = aNode.bookmarkGuid;
  data.instanceId = PlacesUtils.instanceId;

  let guid = aNode.bookmarkGuid;

  if (
    guid &&
    !PlacesUtils.bookmarks.isVirtualRootItem(guid) &&
    !PlacesUtils.isVirtualLeftPaneItem(guid)
  ) {
    if (aNode.parent) {
      data.parent = aNode.parent.itemId;
      data.parentGuid = aNode.parent.bookmarkGuid;
    }

    data.dateAdded = aNode.dateAdded;
    data.lastModified = aNode.lastModified;
  }

  if (PlacesUtils.nodeIsURI(aNode)) {
    if (!URL.canParse(aNode.uri)) {
      throw new Error(aNode.uri + " is not a valid URL");
    }
    data.type = PlacesUtils.TYPE_X_MOZ_PLACE;
    data.uri = aNode.uri;
    if (aNode.tags) {
      data.tags = aNode.tags;
    }
  } else if (PlacesUtils.nodeIsFolderOrShortcut(aNode)) {
    if (aNode.type == Ci.nsINavHistoryResultNode.RESULT_TYPE_FOLDER_SHORTCUT) {
      data.type = PlacesUtils.TYPE_X_MOZ_PLACE;
      data.uri = aNode.uri;
      data.concreteGuid = PlacesUtils.getConcreteItemGuid(aNode);
    } else {
      data.type = PlacesUtils.TYPE_X_MOZ_PLACE_CONTAINER;
    }
  } else if (PlacesUtils.nodeIsQuery(aNode)) {
    data.type = PlacesUtils.TYPE_X_MOZ_PLACE;
    data.uri = aNode.uri;
  } else if (PlacesUtils.nodeIsSeparator(aNode)) {
    data.type = PlacesUtils.TYPE_X_MOZ_PLACE_SEPARATOR;
  }

  return JSON.stringify(data);
}

const DB_URL_LENGTH_MAX = 65536;
const DB_TITLE_LENGTH_MAX = 4096;
const DB_DESCRIPTION_LENGTH_MAX = 256;
const DB_SITENAME_LENGTH_MAX = 50;

function simpleValidateFunc(boolValidateFn) {
  return (v, input) => {
    if (!boolValidateFn(v, input)) {
      throw new Error("Invalid value");
    }
    return v;
  };
}

const BOOKMARK_VALIDATORS = Object.freeze({
  guid: simpleValidateFunc(v => PlacesUtils.isValidGuid(v)),
  parentGuid: simpleValidateFunc(v => PlacesUtils.isValidGuid(v)),
  guidPrefix: simpleValidateFunc(v => PlacesUtils.isValidGuidPrefix(v)),
  index: simpleValidateFunc(
    v => Number.isInteger(v) && v >= PlacesUtils.bookmarks.DEFAULT_INDEX
  ),
  dateAdded: simpleValidateFunc(v => v.constructor.name == "Date" && !isNaN(v)),
  lastModified: simpleValidateFunc(
    v => v.constructor.name == "Date" && !isNaN(v)
  ),
  type: simpleValidateFunc(
    v =>
      Number.isInteger(v) &&
      [
        PlacesUtils.bookmarks.TYPE_BOOKMARK,
        PlacesUtils.bookmarks.TYPE_FOLDER,
        PlacesUtils.bookmarks.TYPE_SEPARATOR,
      ].includes(v)
  ),
  title: v => {
    if (v === null) {
      return "";
    }
    if (typeof v == "string") {
      return v.slice(0, DB_TITLE_LENGTH_MAX);
    }
    throw new Error("Invalid title");
  },
  url: v => {
    simpleValidateFunc(
      val =>
        (typeof val == "string" && val.length <= DB_URL_LENGTH_MAX) ||
        (val instanceof Ci.nsIURI && val.spec.length <= DB_URL_LENGTH_MAX) ||
        (URL.isInstance(val) && val.href.length <= DB_URL_LENGTH_MAX)
    )(v);
    if (typeof v === "string") {
      return new URL(v);
    }
    if (v instanceof Ci.nsIURI) {
      return URL.fromURI(v);
    }
    return v;
  },
  source: simpleValidateFunc(
    v =>
      Number.isInteger(v) &&
      Object.values(PlacesUtils.bookmarks.SOURCES).includes(v)
  ),
  keyword: simpleValidateFunc(v => typeof v == "string" && !!v.length),
  charset: simpleValidateFunc(v => typeof v == "string" && !!v.length),
  postData: simpleValidateFunc(v => typeof v == "string" && !!v.length),
  tags: simpleValidateFunc(
    v =>
      Array.isArray(v) &&
      v.length &&
      v.every(item => item && typeof item == "string")
  ),
});

const PAGEINFO_VALIDATORS = Object.freeze({
  guid: BOOKMARK_VALIDATORS.guid,
  url: BOOKMARK_VALIDATORS.url,
  title: v => {
    if (v == null || v == undefined) {
      return undefined;
    } else if (typeof v === "string") {
      return v;
    }
    throw new TypeError(
      `title property of PageInfo object: ${v} must be a string if provided`
    );
  },
  previewImageURL: v => {
    if (!v) {
      return null;
    }
    return BOOKMARK_VALIDATORS.url(v);
  },
  description: v => {
    if (typeof v === "string" || v === null) {
      return v ? v.slice(0, DB_DESCRIPTION_LENGTH_MAX) : null;
    }
    throw new TypeError(
      `description property of pageInfo object: ${v} must be either a string or null if provided`
    );
  },
  siteName: v => {
    if (typeof v === "string" || v === null) {
      return v ? v.slice(0, DB_SITENAME_LENGTH_MAX) : null;
    }
    throw new TypeError(
      `siteName property of pageInfo object: ${v} must be either a string or null if provided`
    );
  },
  annotations: v => {
    if (typeof v != "object" || v.constructor.name != "Map") {
      throw new TypeError("annotations must be a Map");
    }

    if (v.size == 0) {
      throw new TypeError("there must be at least one annotation");
    }

    for (let [key, value] of v.entries()) {
      if (typeof key != "string") {
        throw new TypeError("all annotation keys must be strings");
      }
      if (
        typeof value != "string" &&
        typeof value != "number" &&
        typeof value != "boolean" &&
        value !== null &&
        value !== undefined
      ) {
        throw new TypeError(
          "all annotation values must be Boolean, Numbers or Strings"
        );
      }
    }
    return v;
  },
  visits: v => {
    if (!Array.isArray(v) || !v.length) {
      throw new TypeError("PageInfo object must have an array of visits");
    }
    let visits = [];
    for (let inVisit of v) {
      let visit = {
        date: new Date(),
        transition: inVisit.transition || lazy.History.TRANSITIONS.LINK,
      };

      if (!PlacesUtils.history.isValidTransition(visit.transition)) {
        throw new TypeError(
          `transition: ${visit.transition} is not a valid transition type`
        );
      }

      if (inVisit.date) {
        PlacesUtils.history.ensureDate(inVisit.date);
        if (inVisit.date > Date.now() + TIMERS_RESOLUTION_SKEW_MS) {
          throw new TypeError(`date: ${inVisit.date} cannot be a future date`);
        }
        visit.date = inVisit.date;
      }

      if (inVisit.referrer) {
        visit.referrer = PlacesUtils.normalizeToURLOrGUID(inVisit.referrer);
      }
      visits.push(visit);
    }
    return visits;
  },
});

export var PlacesUtils = {
  TYPE_X_MOZ_PLACE_CONTAINER: "text/x-moz-place-container",
  TYPE_X_MOZ_PLACE_SEPARATOR: "text/x-moz-place-separator",
  TYPE_X_MOZ_PLACE: "text/x-moz-place",
  TYPE_X_MOZ_URL: "text/x-moz-url",
  TYPE_HTML: "text/html",
  TYPE_PLAINTEXT: "text/plain",
  TYPE_X_MOZ_PLACE_ACTION: "text/x-moz-place-action",

  LMANNO_FEEDURI: "livemark/feedURI",
  LMANNO_SITEURI: "livemark/siteURI",
  CHARSET_ANNO: "URIProperties/characterSet",
  MOBILE_ROOT_ANNO: "mobile/bookmarksRoot",

  TOPIC_SHUTDOWN: "places-shutdown",
  TOPIC_INIT_COMPLETE: "places-init-complete",
  TOPIC_DATABASE_LOCKED: "places-database-locked",
  TOPIC_EXPIRATION_FINISHED: "places-expiration-finished",
  TOPIC_FAVICONS_EXPIRED: "places-favicons-expired",
  TOPIC_VACUUM_STARTING: "places-vacuum-starting",
  TOPIC_BOOKMARKS_RESTORE_BEGIN: "bookmarks-restore-begin",
  TOPIC_BOOKMARKS_RESTORE_SUCCESS: "bookmarks-restore-success",
  TOPIC_BOOKMARKS_RESTORE_FAILED: "bookmarks-restore-failed",

  observers: PlacesObservers,

  virtualAllBookmarksGuid: "allbms_____v",
  virtualHistoryGuid: "history____v",
  virtualDownloadsGuid: "downloads__v",
  virtualTagsGuid: "tags_______v",

  isVirtualLeftPaneItem(guid) {
    return (
      guid == PlacesUtils.virtualAllBookmarksGuid ||
      guid == PlacesUtils.virtualHistoryGuid ||
      guid == PlacesUtils.virtualDownloadsGuid ||
      guid == PlacesUtils.virtualTagsGuid
    );
  },

  asContainer: aNode => asContainer(aNode),
  asQuery: aNode => asQuery(aNode),

  endl: NEWLINE,

  isValidGuid(guid) {
    return typeof guid == "string" && guid && /^[a-zA-Z0-9\-_]{12}$/.test(guid);
  },

  isValidGuidPrefix(guidPrefix) {
    return (
      typeof guidPrefix == "string" &&
      guidPrefix &&
      /^[a-zA-Z0-9\-_]{1,11}$/.test(guidPrefix)
    );
  },

  generateGuidWithPrefix(prefix) {
    return prefix + this.history.makeGuid().substring(prefix.length);
  },

  toURI(url) {
    if (url instanceof Ci.nsIURI) {
      return url;
    }
    if (URL.isInstance(url)) {
      return url.URI;
    }
    return Services.io.newURI(url);
  },

  toPRTime(date) {
    if (
      date.constructor.name == "Date" &&
      typeof date != "number" &&
      !isNaN(date.getTime())
    ) {
      return date.getTime() * 1000;
    } else if (typeof date == "number" && !isNaN(date)) {
      return date * 1000;
    }
    throw new Error("Invalid value passed to toPRTime");
  },

  toDate(time) {
    if (typeof time != "number" || isNaN(time)) {
      throw new Error("Invalid value passed to toDate");
    }
    return new Date(Math.trunc(time / 1000));
  },

  toISupportsString(aString) {
    let s = Cc["@mozilla.org/supports-string;1"].createInstance(
      Ci.nsISupportsString
    );
    s.data = aString;
    return s;
  },

  getFormattedString: function PU_getFormattedString(key, params) {
    return lazy.bundle.formatStringFromName(key, params);
  },

  getString: function PU_getString(key) {
    return lazy.bundle.GetStringFromName(key);
  },

  parseActionUrl(url) {
    if (url instanceof Ci.nsIURI) {
      url = url.spec;
    } else if (URL.isInstance(url)) {
      url = url.href;
    }
    if (!url.startsWith("moz-action:")) {
      return null;
    }

    try {
      let [, type, params] = url.match(lazy.MOZ_ACTION_REGEX);
      let action = {
        type,
        params: JSON.parse(params),
      };
      for (let key in action.params) {
        action.params[key] = decodeURIComponent(action.params[key]);
      }
      return action;
    } catch (ex) {
      console.error(`Invalid action url "${url}"`);
      return null;
    }
  },

  /**
   * Determines if a bookmark folder or folder shortcut is generated by a query.
   *
   * @param {nsINavHistoryResultNode} node
   *   A result node.
   * @returns {boolean}
   *   Whether `node` is a bookmark folder or folder
   *   shortcut generated as result of a query.
   */
  nodeIsQueryGeneratedFolder(node) {
    return (
      node.parent &&
      this.nodeIsFolderOrShortcut(node) &&
      this.nodeIsQuery(node.parent)
    );
  },

  nodeIsFolderOrShortcut(node) {
    return [
      Ci.nsINavHistoryResultNode.RESULT_TYPE_FOLDER,
      Ci.nsINavHistoryResultNode.RESULT_TYPE_FOLDER_SHORTCUT,
    ].includes(node.type);
  },

  nodeIsConcreteFolder(node) {
    return node.type == Ci.nsINavHistoryResultNode.RESULT_TYPE_FOLDER;
  },

  nodeIsBookmark(node) {
    return (
      node.type == Ci.nsINavHistoryResultNode.RESULT_TYPE_URI &&
      (node.itemId != -1 || !!node.bookmarkGuid)
    );
  },

  nodeIsSeparator(node) {
    return node.type == Ci.nsINavHistoryResultNode.RESULT_TYPE_SEPARATOR;
  },

  nodeIsURI(node) {
    return node.type == Ci.nsINavHistoryResultNode.RESULT_TYPE_URI;
  },

  nodeIsQuery(node) {
    return node.type == Ci.nsINavHistoryResultNode.RESULT_TYPE_QUERY;
  },

  *nodeAncestors(aNode) {
    let node = aNode.parent;
    while (node) {
      yield node;
      node = node.parent;
    }
  },

  validateItemProperties(name, validators, props, behavior = {}) {
    if (typeof props != "object" || !props) {
      throw new Error(`${name}: Input should be a valid object`);
    }
    let input = Object.assign({}, props);
    let normalizedInput = {};
    let required = new Set();
    for (let prop in behavior) {
      if (
        behavior[prop].hasOwnProperty("required") &&
        behavior[prop].required
      ) {
        required.add(prop);
      }
      if (
        behavior[prop].hasOwnProperty("requiredIf") &&
        behavior[prop].requiredIf(input)
      ) {
        required.add(prop);
      }
      if (
        behavior[prop].hasOwnProperty("validIf") &&
        input[prop] !== undefined &&
        !behavior[prop].validIf(input)
      ) {
        if (behavior[prop].hasOwnProperty("fixup")) {
          behavior[prop].fixup(input);
        } else {
          throw new Error(
            `${name}: Invalid value for property '${prop}': ${JSON.stringify(
              input[prop]
            )}`
          );
        }
      }
      if (
        behavior[prop].hasOwnProperty("defaultValue") &&
        input[prop] === undefined
      ) {
        input[prop] = behavior[prop].defaultValue;
      }
      if (behavior[prop].hasOwnProperty("replaceWith")) {
        input[prop] = behavior[prop].replaceWith;
      }
    }

    for (let prop in input) {
      if (required.has(prop)) {
        required.delete(prop);
      } else if (input[prop] === undefined) {
        continue;
      }
      if (validators.hasOwnProperty(prop)) {
        try {
          normalizedInput[prop] = validators[prop](input[prop], input);
        } catch (ex) {
          if (
            behavior.hasOwnProperty(prop) &&
            behavior[prop].hasOwnProperty("fixup")
          ) {
            behavior[prop].fixup(input);
            normalizedInput[prop] = input[prop];
          } else {
            throw new Error(
              `${name}: Invalid value for property '${prop}': ${JSON.stringify(
                input[prop]
              )}`
            );
          }
        }
      }
    }
    if (required.size > 0) {
      throw new Error(
        `${name}: The following properties were expected: ${[...required].join(
          ", "
        )}`
      );
    }
    return normalizedInput;
  },

  BOOKMARK_VALIDATORS,
  PAGEINFO_VALIDATORS,

  QueryInterface: ChromeUtils.generateQI(["nsIObserver"]),

  _shutdownFunctions: [],
  registerShutdownFunction(aFunc) {
    if (!this._shutdownFunctions.length) {
      Services.obs.addObserver(this, this.TOPIC_SHUTDOWN);
    }
    this._shutdownFunctions.push(aFunc);
  },

  observe(aSubject, aTopic) {
    switch (aTopic) {
      case this.TOPIC_SHUTDOWN:
        Services.obs.removeObserver(this, this.TOPIC_SHUTDOWN);
        while (this._shutdownFunctions.length) {
          this._shutdownFunctions.shift().apply(this);
        }
        break;
    }
  },

  nodeIsHost(aNode) {
    return (
      aNode.type == Ci.nsINavHistoryResultNode.RESULT_TYPE_QUERY &&
      aNode.parent &&
      asQuery(aNode.parent).queryOptions.resultType ==
        Ci.nsINavHistoryQueryOptions.RESULTS_AS_SITE_QUERY
    );
  },

  nodeIsDay(aNode) {
    var resultType;
    return (
      aNode.type == Ci.nsINavHistoryResultNode.RESULT_TYPE_QUERY &&
      aNode.parent &&
      ((resultType = asQuery(aNode.parent).queryOptions.resultType) ==
        Ci.nsINavHistoryQueryOptions.RESULTS_AS_DATE_QUERY ||
        resultType == Ci.nsINavHistoryQueryOptions.RESULTS_AS_DATE_SITE_QUERY)
    );
  },

  nodeIsTagQuery(aNode) {
    if (aNode.type != Ci.nsINavHistoryResultNode.RESULT_TYPE_QUERY) {
      return false;
    }
    let parent = aNode.parent;
    if (
      parent &&
      PlacesUtils.asQuery(parent).queryOptions.resultType ==
        Ci.nsINavHistoryQueryOptions.RESULTS_AS_TAGS_ROOT
    ) {
      return true;
    }
    if (
      !parent &&
      aNode == aNode.parentResult.root &&
      PlacesUtils.asQuery(aNode).query.tags.length == 1
    ) {
      return true;
    }
    return false;
  },

  containerTypes: [
    Ci.nsINavHistoryResultNode.RESULT_TYPE_FOLDER,
    Ci.nsINavHistoryResultNode.RESULT_TYPE_FOLDER_SHORTCUT,
    Ci.nsINavHistoryResultNode.RESULT_TYPE_QUERY,
  ],

  nodeIsContainer(aNode) {
    return this.containerTypes.includes(aNode.type);
  },

  nodeIsHistoryContainer(aNode) {
    var resultType;
    return (
      this.nodeIsQuery(aNode) &&
      ((resultType = asQuery(aNode).queryOptions.resultType) ==
        Ci.nsINavHistoryQueryOptions.RESULTS_AS_DATE_SITE_QUERY ||
        resultType == Ci.nsINavHistoryQueryOptions.RESULTS_AS_DATE_QUERY ||
        resultType == Ci.nsINavHistoryQueryOptions.RESULTS_AS_SITE_QUERY ||
        this.nodeIsDay(aNode) ||
        this.nodeIsHost(aNode))
    );
  },

  getConcreteItemGuid(aNode) {
    if (aNode.type == Ci.nsINavHistoryResultNode.RESULT_TYPE_FOLDER_SHORTCUT) {
      return asQuery(aNode).targetFolderGuid;
    }
    return aNode.bookmarkGuid;
  },

  getReversedHost(url) {
    return url.host.split("").reverse().join("") + ".";
  },

  copyNode(node) {
    let contentTypes = [
      this.TYPE_X_MOZ_URL,
      this.TYPE_HTML,
      this.TYPE_PLAINTEXT,
    ];

    let xferable = Cc["@mozilla.org/widget/transferable;1"].createInstance(
      Ci.nsITransferable
    );
    xferable.init(null);
    for (let contentType of contentTypes) {
      xferable.addDataFlavor(contentType);
      let data = this.wrapNode(node, contentType);
      xferable.setTransferData(contentType, this.toISupportsString(data));
    }

    Services.clipboard.setData(
      xferable,
      null,
      Ci.nsIClipboard.kGlobalClipboard
    );
  },

  wrapNode(aNode, aType) {
    function gatherTextDataFromNode(node, gatherDataFunc) {
      if (
        PlacesUtils.nodeIsFolderOrShortcut(node) &&
        asQuery(node).queryOptions.excludeItems
      ) {
        let folderRoot = PlacesUtils.getFolderContents(
          PlacesUtils.getConcreteItemGuid(node),
          false,
          true
        ).root;
        try {
          return gatherDataFunc(folderRoot);
        } finally {
          folderRoot.containerOpen = false;
        }
      }
      return gatherDataFunc(node);
    }

    function gatherDataHtml(node, recursiveOpen = true) {
      let htmlEscape = s =>
        s
          .replace(/&/g, "&amp;")
          .replace(/>/g, "&gt;")
          .replace(/</g, "&lt;")
          .replace(/"/g, "&quot;")
          .replace(/'/g, "&apos;");

      let escapedTitle = node.title ? htmlEscape(node.title) : "";

      if (PlacesUtils.nodeIsContainer(node)) {
        asContainer(node);

        let childString = "<DL><DT>" + escapedTitle + "</DT>" + NEWLINE;

        let wasOpen = node.containerOpen;
        if (!wasOpen && recursiveOpen) {
          node.containerOpen = true;
        }
        if (node.containerOpen) {
          for (let i = 0, count = node.childCount; i < count; ++i) {
            let child = node.getChild(i);
            childString +=
              "<DD>" +
              NEWLINE +
              gatherDataHtml(child, PlacesUtils.nodeIsConcreteFolder(child)) +
              "</DD>" +
              NEWLINE;
          }
          node.containerOpen = wasOpen;
        }
        return childString + "</DL>" + NEWLINE;
      }
      if (PlacesUtils.nodeIsURI(node)) {
        return `<A HREF="${node.uri}">${escapedTitle}</A>${NEWLINE}`;
      }
      if (PlacesUtils.nodeIsSeparator(node)) {
        return "<HR>" + NEWLINE;
      }
      return "";
    }

    function gatherDataText(node, recursiveOpen = true) {
      if (PlacesUtils.nodeIsContainer(node)) {
        asContainer(node);

        let childString = node.title + NEWLINE;

        let wasOpen = node.containerOpen;
        if (!wasOpen && recursiveOpen) {
          node.containerOpen = true;
        }
        if (node.containerOpen) {
          for (let i = 0, count = node.childCount; i < count; ++i) {
            let child = node.getChild(i);
            let suffix = i < count - 1 ? NEWLINE : "";
            childString +=
              gatherDataText(child, PlacesUtils.nodeIsConcreteFolder(child)) +
              suffix;
          }
          node.containerOpen = wasOpen;
        }
        return childString;
      }
      if (PlacesUtils.nodeIsURI(node)) {
        return node.uri;
      }
      if (PlacesUtils.nodeIsSeparator(node)) {
        return "--------------------";
      }
      return "";
    }

    switch (aType) {
      case this.TYPE_X_MOZ_PLACE:
      case this.TYPE_X_MOZ_PLACE_SEPARATOR:
      case this.TYPE_X_MOZ_PLACE_CONTAINER: {
        return serializeNode(aNode);
      }
      case this.TYPE_X_MOZ_URL: {
        if (PlacesUtils.nodeIsURI(aNode)) {
          return aNode.uri + NEWLINE + aNode.title;
        }
        if (PlacesUtils.nodeIsContainer(aNode)) {
          // @ts-ignore
          return PlacesUtils.getURLsForContainerNode(aNode)
            .map(item => item.uri + "\n" + item.title)
            .join("\n");
        }
        return "";
      }
      case this.TYPE_HTML: {
        return gatherTextDataFromNode(aNode, gatherDataHtml);
      }
    }

    return gatherTextDataFromNode(aNode, gatherDataText);
  },

  unwrapNodes(blob, type) {
    let validNodes = [];
    let invalidNodes = [];
    switch (type) {
      case this.TYPE_X_MOZ_PLACE:
      case this.TYPE_X_MOZ_PLACE_SEPARATOR:
      case this.TYPE_X_MOZ_PLACE_CONTAINER:
        validNodes = JSON.parse("[" + blob + "]");
        break;
      case this.TYPE_X_MOZ_URL: {
        let parts = blob.split("\n");
        if (parts.length != 1 && parts.length % 2) {
          break;
        }
        for (let i = 0; i < parts.length; i = i + 2) {
          let uriString = parts[i].trimStart();
          if (!uriString) {
            continue;
          }
          let uri = null;
          try {
            uri = Services.io.newURI(uriString);
          } catch (e) {
            console.error(e);
            invalidNodes.push({
              uri: uriString,
            });
          }
          if (!uri || uri.scheme == "place") {
            continue;
          }
          let titleString = "";
          if (parts.length > i + 1) {
            titleString = parts[i + 1];
          } else if (uri instanceof Ci.nsIURL) {
            titleString = uri.fileName;
          }

          validNodes.push({
            uri: uriString,
            title: titleString || uriString,
            type: this.TYPE_X_MOZ_URL,
          });
        }
        break;
      }
      case this.TYPE_PLAINTEXT: {
        let parts = blob.split("\n");
        for (let i = 0; i < parts.length; i++) {
          let uriString = parts[i].trimStart();
          if (uriString.substr(0, 1) == "\x23" || uriString == "") {
            continue;
          }
          try {
            let uri = Services.io.newURI(uriString);
            if (uri.scheme != "place") {
              validNodes.push({
                uri: uriString,
                title: uriString,
                type: this.TYPE_X_MOZ_URL,
              });
            }
          } catch (e) {
            console.error(e);
            invalidNodes.push({
              uri: uriString,
            });
          }
        }
        break;
      }
      default:
        throw Components.Exception("", Cr.NS_ERROR_INVALID_ARG);
    }

    return { validNodes, invalidNodes };
  },

  validatePageInfo(pageInfo, validateVisits = true) {
    return this.validateItemProperties(
      "PageInfo",
      PAGEINFO_VALIDATORS,
      pageInfo,
      {
        url: { requiredIf: b => !b.guid },
        guid: { requiredIf: b => !b.url },
        visits: { requiredIf: () => validateVisits },
      }
    );
  },

  normalizeToURLOrGUID(key) {
    if (typeof key === "string") {
      if (this.isValidGuid(key)) {
        return key;
      }
      return new URL(key);
    }
    if (URL.isInstance(key)) {
      return key;
    }
    if (key instanceof Ci.nsIURI) {
      return URL.fromURI(key);
    }
    throw new TypeError("Invalid url or guid: " + key);
  },

  getFolderContents(aFolderGuid, aExcludeItems, aExpandQueries) {
    if (!this.isValidGuid(aFolderGuid)) {
      throw new Error("aFolderGuid should be a valid GUID.");
    }
    var query = this.history.getNewQuery();
    query.setParents([aFolderGuid]);
    var options = this.history.getNewQueryOptions();
    options.excludeItems = aExcludeItems;
    options.expandQueries = aExpandQueries;

    var result = this.history.executeQuery(query, options);
    result.root.containerOpen = true;
    return result;
  },

  get tagsFolderId() {
    return (this._tagsFolderId ??= this.bookmarks.tagsFolder);
  },

  isRootItem(guid) {
    return (
      guid == PlacesUtils.bookmarks.menuGuid ||
      guid == PlacesUtils.bookmarks.toolbarGuid ||
      guid == PlacesUtils.bookmarks.unfiledGuid ||
      guid == PlacesUtils.bookmarks.tagsGuid ||
      guid == PlacesUtils.bookmarks.rootGuid ||
      guid == PlacesUtils.bookmarks.mobileGuid
    );
  },

  getContainerNodeWithOptions(aNode, aExcludeItems, aExpandQueries) {
    if (!this.nodeIsContainer(aNode)) {
      throw Components.Exception("", Cr.NS_ERROR_INVALID_ARG);
    }

    var excludeItems =
      asQuery(aNode).queryOptions.excludeItems ||
      asQuery(aNode.parentResult.root).queryOptions.excludeItems;
    var expandQueries =
      asQuery(aNode).queryOptions.expandQueries &&
      asQuery(aNode.parentResult.root).queryOptions.expandQueries;

    if (excludeItems == aExcludeItems && expandQueries == aExpandQueries) {
      return aNode;
    }

    var query = {},
      options = {};
    this.history.queryStringToQuery(aNode.uri, query, options);
    options.value.excludeItems = aExcludeItems;
    options.value.expandQueries = aExpandQueries;
    return this.history.executeQuery(query.value, options.value).root;
  },

  hasChildURIs(aNode) {
    if (!this.nodeIsContainer(aNode)) {
      return false;
    }

    let root = this.getContainerNodeWithOptions(aNode, false, true);
    let result = root.parentResult;
    let didSuppressNotifications = false;
    let wasOpen = root.containerOpen;
    if (!wasOpen) {
      didSuppressNotifications = result.suppressNotifications;
      if (!didSuppressNotifications) {
        result.suppressNotifications = true;
      }

      root.containerOpen = true;
    }

    let found = false;
    for (let i = 0, count = root.childCount; i < count && !found; i++) {
      let child = root.getChild(i);
      if (this.nodeIsURI(child)) {
        found = true;
      }
    }

    if (!wasOpen) {
      root.containerOpen = false;
      if (!didSuppressNotifications) {
        result.suppressNotifications = false;
      }
    }
    return found;
  },

  getURLsForContainerNode(aNode) {
    let urls = [];
    if (!this.nodeIsContainer(aNode)) {
      return urls;
    }

    let root = this.getContainerNodeWithOptions(aNode, false, true);
    let result = root.parentResult;
    let wasOpen = root.containerOpen;
    let didSuppressNotifications = false;
    if (!wasOpen) {
      didSuppressNotifications = result.suppressNotifications;
      if (!didSuppressNotifications) {
        result.suppressNotifications = true;
      }

      root.containerOpen = true;
    }

    for (let i = 0, count = root.childCount; i < count; ++i) {
      let child = root.getChild(i);
      if (this.nodeIsURI(child)) {
        urls.push({
          uri: child.uri,
          isBookmark: this.nodeIsBookmark(child),
          title: child.title,
        });
      }
    }

    if (!wasOpen) {
      root.containerOpen = false;
      if (!didSuppressNotifications) {
        result.suppressNotifications = false;
      }
    }
    return urls;
  },

  promiseDBConnection: () => lazy.gAsyncDBConnPromised,

  promiseLargeCacheDBConnection: () => lazy.gAsyncDBLargeCacheConnPromised,
  get largeCacheDBConnDeferred() {
    return gAsyncDBLargeCacheConnDeferred;
  },

  promiseUnsafeWritableDBConnection: () => lazy.gAsyncDBWrapperPromised,

  async withConnectionWrapper(name, task) {
    if (!name) {
      throw new TypeError("Expecting a user-readable name");
    }
    let db = await lazy.gAsyncDBWrapperPromised;
    return db.executeBeforeShutdown(name, task);
  },

  urlWithSizeRef(window, href, size) {
    return (
      href +
      (href.includes("#") ? "&" : "#") +
      "size=" +
      Math.round(size) * window.devicePixelRatio
    );
  },

  async promiseBookmarksTree(aItemGuid = "", aOptions = {}) {
    let createItemInfoObject = async function (
       aRow,
       aIncludeParentGuid
    ) {
      let item = {};
      let copyProps = (...props) => {
        for (let prop of props) {
          let val = aRow.getResultByName(prop);
          if (val !== null) {
            item[prop] = val;
          }
        }
      };
      copyProps("guid", "title", "index", "dateAdded", "lastModified");
      if (aIncludeParentGuid) {
        copyProps("parentGuid");
      }

      let itemId =  (aRow.getResultByName("id"));
      if (aOptions.includeItemIds) {
        item.id = itemId;
      }

      let type =  (aRow.getResultByName("type"));
      item.typeCode = type;
      if (type == Ci.nsINavBookmarksService.TYPE_BOOKMARK) {
        copyProps("charset", "tags", "iconUri");
      }

      switch (type) {
        case PlacesUtils.bookmarks.TYPE_BOOKMARK: {
          item.type = PlacesUtils.TYPE_X_MOZ_PLACE;
          item.uri = URL.parse(
            // @ts-expect-error - Bug 1966462
             aRow.getResultByName("url")
          )?.href;
          if (!item.uri) {
            let error = new Error("Invalid bookmark URL");
            // @ts-ignore
            error.becauseInvalidURL = true;
            throw error;
          }
          let entry = await PlacesUtils.keywords.fetch({ url: item.uri });
          if (entry) {
            item.keyword = entry.keyword;
            item.postData = entry.postData;
          }
          break;
        }
        case PlacesUtils.bookmarks.TYPE_FOLDER:
          item.type = PlacesUtils.TYPE_X_MOZ_PLACE_CONTAINER;
          if (item.guid == PlacesUtils.bookmarks.rootGuid) {
            item.root = "placesRoot";
          } else if (item.guid == PlacesUtils.bookmarks.menuGuid) {
            item.root = "bookmarksMenuFolder";
          } else if (item.guid == PlacesUtils.bookmarks.unfiledGuid) {
            item.root = "unfiledBookmarksFolder";
          } else if (item.guid == PlacesUtils.bookmarks.toolbarGuid) {
            item.root = "toolbarFolder";
          } else if (item.guid == PlacesUtils.bookmarks.mobileGuid) {
            item.root = "mobileFolder";
          }
          break;
        case PlacesUtils.bookmarks.TYPE_SEPARATOR:
          item.type = PlacesUtils.TYPE_X_MOZ_PLACE_SEPARATOR;
          break;
        default:
          console.error(`Unexpected bookmark type ${type}`);
          break;
      }
      return item;
    };

    const QUERY_STR = `/* do not warn (bug no): cannot use an index */
       WITH RECURSIVE
       descendants(fk, level, type, id, guid, parent, parentGuid, position,
                   title, dateAdded, lastModified) AS (
         SELECT b1.fk, 0, b1.type, b1.id, b1.guid, b1.parent,
                (SELECT guid FROM moz_bookmarks WHERE id = b1.parent),
                b1.position, b1.title, b1.dateAdded, b1.lastModified
         FROM moz_bookmarks b1 WHERE b1.guid=:item_guid
         UNION ALL
         SELECT b2.fk, level + 1, b2.type, b2.id, b2.guid, b2.parent,
                descendants.guid, b2.position, b2.title, b2.dateAdded,
                b2.lastModified
         FROM moz_bookmarks b2
         JOIN descendants ON b2.parent = descendants.id AND b2.id <> :tags_folder),
       tagged(place_id, tags) AS (
         SELECT b.fk, group_concat(p.title ORDER BY p.title)
         FROM moz_bookmarks b
         JOIN moz_bookmarks p ON p.id = b.parent
         JOIN moz_bookmarks g ON g.id = p.parent
         WHERE g.guid = '${PlacesUtils.bookmarks.tagsGuid}'
         GROUP BY b.fk
       )
       SELECT d.level, d.id, d.guid, d.parent, d.parentGuid, d.type,
              d.position AS [index], IFNULL(d.title, '') AS title, d.dateAdded,
              d.lastModified, h.url, (SELECT icon_url FROM moz_icons i
                      JOIN moz_icons_to_pages ON icon_id = i.id
                      JOIN moz_pages_w_icons pi ON page_id = pi.id
                      WHERE pi.page_url_hash = hash(h.url) AND pi.page_url = h.url
                      ORDER BY (flags & ${Ci.nsIFaviconService.ICONDATA_FLAGS_RICH}) ASC, width DESC LIMIT 1) AS iconUri,
              (SELECT tags FROM tagged WHERE place_id = h.id) AS tags,
              (SELECT a.content FROM moz_annos a
               JOIN moz_anno_attributes n ON a.anno_attribute_id = n.id
               WHERE place_id = h.id AND n.name = :charset_anno
              ) AS charset
       FROM descendants d
       LEFT JOIN moz_bookmarks b3 ON b3.id = d.parent
       LEFT JOIN moz_places h ON h.id = d.fk
       ORDER BY d.level, d.parent, d.position`;

    if (!aItemGuid) {
      aItemGuid = this.bookmarks.rootGuid;
    }

    let hasExcludeItemsCallback = aOptions.hasOwnProperty(
      "excludeItemsCallback"
    );
    let excludedParents = new Set();
    let shouldExcludeItem = (aItem, aParentGuid) => {
      let exclude =
        excludedParents.has(aParentGuid) ||
        aOptions.excludeItemsCallback(aItem);
      if (exclude) {
        if (aItem.type == this.TYPE_X_MOZ_PLACE_CONTAINER) {
          excludedParents.add(aItem.guid);
        }
      }
      return exclude;
    };

    let rootItem = null;
    let parentsMap = new Map();
    let conn = await this.promiseDBConnection();
    let rows = await conn.executeCached(QUERY_STR, {
      tags_folder: PlacesUtils.tagsFolderId,
      charset_anno: PlacesUtils.CHARSET_ANNO,
      item_guid: aItemGuid,
    });
    let yieldCounter = 0;
    for (let row of rows) {
      let item;
      if (!rootItem) {
        try {
          rootItem = item = await createItemInfoObject(row, true);
          Object.defineProperty(rootItem, "itemsCount", {
            value: 1,
            writable: true,
            enumerable: false,
            configurable: false,
          });
        } catch (ex) {
          console.error("Failed to fetch the data for the root item");
          throw ex;
        }
      } else {
        try {
          item = await createItemInfoObject(row, false);
          let parentGuid = row.getResultByName("parentGuid");
          if (hasExcludeItemsCallback && shouldExcludeItem(item, parentGuid)) {
            continue;
          }

          let parentItem = parentsMap.get(parentGuid);
          if ("children" in parentItem) {
            parentItem.children.push(item);
          } else {
            parentItem.children = [item];
          }

          rootItem.itemsCount++;
        } catch (ex) {
          console.error("Failed to fetch the data for an item ", ex);
          continue;
        }
      }

      if (item.type == this.TYPE_X_MOZ_PLACE_CONTAINER) {
        parentsMap.set(item.guid, item);
      }

      if (++yieldCounter % 50 == 0) {
        await new Promise(resolve => {
          Services.tm.dispatchToMainThread(() => resolve());
        });
      }
    }

    return rootItem;
  },

  *chunkArray(array, chunkLength) {
    if (chunkLength <= 0 || !Number.isInteger(chunkLength)) {
      throw new TypeError("Chunk length must be a positive integer");
    }
    if (!array.length) {
      return;
    }
    if (array.length <= chunkLength) {
      yield array;
      return;
    }
    let startIndex = 0;
    while (startIndex < array.length) {
      yield array.slice(startIndex, (startIndex += chunkLength));
    }
  },

  sqlBindPlaceholders(info, prefix = "", suffix = "") {
    let length = Array.isArray(info) ? info.length : info;
    return new Array(length).fill(prefix + "?" + suffix).join(",");
  },

  md5(data, { format = "base64" } = {}) {
    let hasher = new lazy.CryptoHash("md5");
    let encodedData = new TextEncoder().encode(data);
    hasher.update(encodedData, encodedData.length);
    switch (format) {
      case "hex": {
        let hash = hasher.finish(false);
        return Array.from(hash, (c, i) =>
          hash.charCodeAt(i).toString(16).padStart(2, "0")
        ).join("");
      }
      case "base64":
      default:
        return hasher.finish(true);
    }
  },

  sha256(data, { format = "base64" } = {}) {
    let hasher = new lazy.CryptoHash("sha256");
    if (data instanceof Ci.nsIStringInputStream) {
      hasher.updateFromStream(data, -1);
    } else {
      let encodedData = new TextEncoder().encode(data);
      hasher.update(encodedData, encodedData.length);
    }
    switch (format) {
      case "hex": {
        let hash = hasher.finish(false);
        return Array.from(hash, (c, i) =>
          hash.charCodeAt(i).toString(16).padStart(2, "0")
        ).join("");
      }
      case "base64url":
        return hasher.finish(true).replaceAll("+", "-").replaceAll("/", "_");
      case "base64":
      default:
        return hasher.finish(true);
    }
  },

  async maybeInsertPlace(db, url) {
    await db.executeCached(
      `INSERT OR IGNORE INTO moz_places (url, url_hash, rev_host, hidden, frecency, guid)
      VALUES (:url, hash(:url), :rev_host,
              (CASE WHEN :url BETWEEN 'place:' AND 'place:' || X'FFFF' THEN 1 ELSE 0 END),
              :frecency,
              IFNULL((SELECT guid FROM moz_places WHERE url_hash = hash(:url) AND url = :url),
                      GENERATE_GUID()))
      `,
      {
        url: url.href,
        rev_host: this.getReversedHost(url),
        frecency: url.protocol == "place:" ? 0 : -1,
      }
    );
  },

  async maybeInsertManyPlaces(db, urls) {
    await db.executeCached(
      `INSERT OR IGNORE INTO moz_places (url, url_hash, rev_host, hidden, frecency, guid) VALUES
     (:url, hash(:url), :rev_host,
     (CASE WHEN :url BETWEEN 'place:' AND 'place:' || X'FFFF' THEN 1 ELSE 0 END),
     :frecency,
     IFNULL((SELECT guid FROM moz_places WHERE url_hash = hash(:url) AND url = :url), :maybeguid))`,
      urls.map(url => ({
        url: url.href,
        rev_host: this.getReversedHost(url),
        frecency: url.protocol == "place:" ? 0 : -1,
        maybeguid: this.history.makeGuid(),
      }))
    );
  },

  getLogger({ prefix = "" } = {}) {
    if (!this._loggers) {
      this._loggers = new Map();
    }
    let logger = this._loggers.get(prefix);
    if (!logger) {
      logger = console.createInstance({
        prefix: `Places${prefix ? " - " + prefix : ""}`,
        maxLogLevelPref: "places.loglevel",
      });
      this._loggers.set(prefix, logger);
    }
    return logger;
  },

  tensorToSQLBindable(tensor) {
    if (!tensor) {
      throw new Error("tensorToSQLBindable received an invalid tensor");
    } else if (Array.isArray(tensor)) {
      return new Uint8ClampedArray(new Float32Array(tensor).buffer);
    } else if (tensor instanceof Float32Array) {
      return new Uint8ClampedArray(tensor.buffer);
    } else {
      throw new Error("tensorToSQLBindable received an invalid tensor");
    }
  },

  metadata: {
    cache: new Map(),
    jsonPrefix: "data:application/json;base64,",

    get(key, defaultValue) {
      return PlacesUtils.withConnectionWrapper("PlacesUtils.metadata.get", db =>
        this.getWithConnection(db, key, defaultValue)
      );
    },

    set(key, value) {
      return PlacesUtils.withConnectionWrapper("PlacesUtils.metadata.set", db =>
        this.setWithConnection(db, new Map([[key, value]]))
      );
    },

    setMany(pairs) {
      return PlacesUtils.withConnectionWrapper("PlacesUtils.metadata.set", db =>
        this.setWithConnection(db, pairs)
      );
    },

    delete(...keys) {
      return PlacesUtils.withConnectionWrapper(
        "PlacesUtils.metadata.delete",
        db => this.deleteWithConnection(db, ...keys)
      );
    },

    async getWithConnection(db, key, defaultValue) {
      key = this.canonicalizeKey(key);
      if (this.cache.has(key)) {
        return this.cache.get(key);
      }
      let rows = await db.executeCached(
        `
        SELECT value FROM moz_meta WHERE key = :key`,
        { key }
      );
      let value = null;
      if (rows.length) {
        let row = rows[0];
        let rawValue = row.getResultByName("value");
        if (row.getTypeOfIndex(0) == row.VALUE_TYPE_BLOB) {
          value = new Uint8Array(rawValue);
        } else if (
          typeof rawValue == "string" &&
          rawValue.startsWith(this.jsonPrefix)
        ) {
          try {
            value = JSON.parse(
              this._base64Decode(rawValue.substr(this.jsonPrefix.length))
            );
          } catch (ex) {
            if (defaultValue !== undefined) {
              value = Cu.cloneInto(defaultValue, {});
            } else {
              throw ex;
            }
          }
        } else {
          value = rawValue;
        }
      } else if (defaultValue !== undefined) {
        value = Cu.cloneInto(defaultValue, {});
      } else {
        throw new Error(`No data stored for key ${key}`);
      }
      this.cache.set(key, value);
      return value;
    },

    async setWithConnection(db, pairs) {
      let entriesToSet = [];
      let keysToDelete = Array.from(pairs.entries())
        .filter(([key, value]) => {
          if (value !== null) {
            entriesToSet.push({ key: this.canonicalizeKey(key), value });
            return false;
          }
          return true;
        })
        .map(([key]) => key);
      if (keysToDelete.length) {
        await this.deleteWithConnection(db, ...keysToDelete);
        if (keysToDelete.length == pairs.size) {
          return;
        }
      }

      let params = entriesToSet.reduce((accumulator, { key, value }, i) => {
        accumulator[`key${i}`] = key;
        accumulator[`value${i}`] =
          typeof value == "object" &&
          ChromeUtils.getClassName(value) != "Uint8Array"
            ? this.jsonPrefix + this._base64Encode(JSON.stringify(value))
            : value;
        return accumulator;
      }, {});
      await db.executeCached(
        "REPLACE INTO moz_meta (key, value) VALUES " +
          entriesToSet.map((e, i) => `(:key${i}, :value${i})`).join(),
        params
      );

      entriesToSet.forEach(({ key, value }) => {
        this.cache.set(key, value);
      });
    },

    async deleteWithConnection(db, ...keys) {
      keys = keys.map(this.canonicalizeKey);
      if (!keys.length) {
        return;
      }
      await db.execute(
        `
        DELETE FROM moz_meta
        WHERE key IN (${new Array(keys.length).fill("?").join(",")})`,
        keys
      );
      for (let key of keys) {
        this.cache.delete(key);
      }
    },

    canonicalizeKey(key) {
      if (typeof key != "string" || !/^[a-zA-Z0-9\/_]+$/.test(key)) {
        throw new TypeError("Invalid metadata key: " + key);
      }
      return key.toLowerCase();
    },

    _base64Encode(str) {
      return ChromeUtils.base64URLEncode(new TextEncoder().encode(str), {
        pad: true,
      });
    },

    _base64Decode(str) {
      return new TextDecoder("utf-8").decode(
        ChromeUtils.base64URLDecode(str, { padding: "require" })
      );
    },
  },

  keywords: {

    fetch(keywordOrEntry, onResult = null) {
      if (typeof keywordOrEntry == "string") {
        keywordOrEntry = { keyword: keywordOrEntry };
      }

      if (
        keywordOrEntry === null ||
        typeof keywordOrEntry != "object" ||
        ("keyword" in keywordOrEntry &&
          typeof keywordOrEntry.keyword != "string")
      ) {
        throw new Error("Invalid keyword");
      }

      let hasKeyword = "keyword" in keywordOrEntry;
      let hasUrl = "url" in keywordOrEntry;

      if (!hasKeyword && !hasUrl) {
        throw new Error("At least keyword or url must be provided");
      }
      if (onResult && typeof onResult != "function") {
        throw new Error("onResult callback must be a valid function");
      }

      if (hasUrl) {
        try {
          keywordOrEntry.url = BOOKMARK_VALIDATORS.url(keywordOrEntry.url);
        } catch (ex) {
          throw new Error(keywordOrEntry.url + " is not a valid URL");
        }
      }
      if (hasKeyword) {
        keywordOrEntry.keyword = keywordOrEntry.keyword.trim().toLowerCase();
      }
      let safeOnResult = entry => {
        if (onResult) {
          try {
            onResult(entry);
          } catch (ex) {
            console.error(ex);
          }
        }
      };

      return promiseKeywordsCache().then(cache => {
        let entries = [];
        if (hasKeyword) {
          let entry = cache.get(keywordOrEntry.keyword);
          if (entry) {
            entries.push(entry);
          }
        }
        if (hasUrl) {
          for (let entry of cache.values()) {
            if (entry.url.href == keywordOrEntry.url.href) {
              entries.push(entry);
            }
          }
        }

        entries = entries.filter(e => {
          return (
            (!hasUrl || e.url.href == keywordOrEntry.url.href) &&
            (!hasKeyword || e.keyword == keywordOrEntry.keyword)
          );
        });

        entries.forEach(safeOnResult);
        return entries.length ? entries[0] : null;
      });
    },

    insert(keywordEntry) {
      if (!keywordEntry || typeof keywordEntry != "object") {
        throw new Error("Input should be a valid object");
      }

      if (
        !("keyword" in keywordEntry) ||
        !keywordEntry.keyword ||
        typeof keywordEntry.keyword != "string"
      ) {
        throw new Error("Invalid keyword");
      }
      if (
        "postData" in keywordEntry &&
        keywordEntry.postData &&
        typeof keywordEntry.postData != "string"
      ) {
        throw new Error("Invalid POST data");
      }
      if (!("url" in keywordEntry)) {
        throw new Error("undefined is not a valid URL");
      }

      if (!("source" in keywordEntry)) {
        keywordEntry.source = PlacesUtils.bookmarks.SOURCES.DEFAULT;
      }
      let { keyword, url, source } = keywordEntry;
      keyword = keyword.trim().toLowerCase();
      let postData = keywordEntry.postData || "";
      try {
        url = BOOKMARK_VALIDATORS.url(url);
      } catch (ex) {
        throw new Error(url + " is not a valid URL");
      }

      return PlacesUtils.withConnectionWrapper(
        "PlacesUtils.keywords.insert",
        async db => {
          let cache = await promiseKeywordsCache();

          let oldEntry = cache.get(keyword);
          if (
            oldEntry &&
            oldEntry.url.href == url.href &&
            (oldEntry.postData || "") == postData
          ) {
            return;
          }

          if (oldEntry) {
            await db.executeCached(
              `UPDATE moz_keywords
               SET place_id = (SELECT id FROM moz_places WHERE url_hash = hash(:url) AND url = :url),
                   post_data = :post_data
               WHERE keyword = :keyword
              `,
              { url: url.href, keyword, post_data: postData }
            );
            await notifyKeywordChange(oldEntry.url.href, "", source);
          } else {
            await db.executeTransaction(async () => {
              await PlacesUtils.maybeInsertPlace(db, url);

              let oldKeywords = [];
              for (let entry of cache.values()) {
                if (
                  entry.url.href == url.href &&
                  (entry.postData || "") == postData
                ) {
                  oldKeywords.push(entry.keyword);
                }
              }
              if (oldKeywords.length) {
                for (let oldKeyword of oldKeywords) {
                  await db.executeCached(
                    `DELETE FROM moz_keywords WHERE keyword = :oldKeyword`,
                    { oldKeyword }
                  );
                  cache.delete(oldKeyword);
                }
              }

              await db.executeCached(
                `INSERT INTO moz_keywords (keyword, place_id, post_data)
                 VALUES (:keyword, (SELECT id FROM moz_places WHERE url_hash = hash(:url) AND url = :url), :post_data)
                `,
                { url: url.href, keyword, post_data: postData }
              );
            });
          }

          cache.set(keyword, { keyword, url, postData: postData || null });

          await notifyKeywordChange(url.href, keyword, source);
        }
      );
    },

    remove(keywordOrEntry) {
      if (typeof keywordOrEntry == "string") {
        keywordOrEntry = {
          keyword: keywordOrEntry,
          source: Ci.nsINavBookmarksService.SOURCE_DEFAULT,
        };
      }

      if (
        keywordOrEntry === null ||
        typeof keywordOrEntry != "object" ||
        !keywordOrEntry.keyword ||
        typeof keywordOrEntry.keyword != "string"
      ) {
        throw new Error("Invalid keyword");
      }

      let { keyword, source = Ci.nsINavBookmarksService.SOURCE_DEFAULT } =
        keywordOrEntry;
      keyword = keywordOrEntry.keyword.trim().toLowerCase();
      return PlacesUtils.withConnectionWrapper(
        "PlacesUtils.keywords.remove",
        async db => {
          let cache = await promiseKeywordsCache();
          if (!cache.has(keyword)) {
            return;
          }
          let { url } = cache.get(keyword);
          cache.delete(keyword);

          await db.execute(
            `DELETE FROM moz_keywords WHERE keyword = :keyword`,
            { keyword }
          );

          await notifyKeywordChange(url.href, "", source);
        }
      );
    },

    reassign(oldURL, newURL, source = lazy.Bookmarks.SOURCES.DEFAULT) {
      try {
        oldURL = BOOKMARK_VALIDATORS.url(oldURL);
      } catch (ex) {
        throw new Error(oldURL + " is not a valid source URL");
      }
      try {
        newURL = BOOKMARK_VALIDATORS.url(newURL);
      } catch (ex) {
        throw new Error(newURL + " is not a valid destination URL");
      }
      return PlacesUtils.withConnectionWrapper(
        "PlacesUtils.keywords.reassign",
        async function (db) {
          let keywordsToReassign = [];
          let keywordsToRemove = [];
          let cache = await promiseKeywordsCache();
          for (let [keyword, entry] of cache) {
            if (entry.url.href == oldURL.href) {
              keywordsToReassign.push(keyword);
            }
            if (entry.url.href == newURL.href) {
              keywordsToRemove.push(keyword);
            }
          }
          if (!keywordsToReassign.length) {
            return;
          }

          await db.executeTransaction(async function () {
            await db.executeCached(
              `DELETE FROM moz_keywords WHERE keyword = :keyword`,
              keywordsToRemove.map(keyword => ({ keyword }))
            );

            await db.executeCached(
              `
            UPDATE moz_keywords SET
              place_id = (SELECT id FROM moz_places
                          WHERE url_hash = hash(:newURL) AND
                                url = :newURL)
            WHERE place_id = (SELECT id FROM moz_places
                              WHERE url_hash = hash(:oldURL) AND
                                    url = :oldURL)`,
              { newURL: newURL.href, oldURL: oldURL.href }
            );
          });
          for (let keyword of keywordsToReassign) {
            let entry = cache.get(keyword);
            entry.url = newURL;
          }
          for (let keyword of keywordsToRemove) {
            cache.delete(keyword);
          }

          if (keywordsToReassign.length) {
            await notifyKeywordChange(oldURL.href, "", source);
            await notifyKeywordChange(newURL.href, "", source);
            for (let keyword of keywordsToReassign) {
              await notifyKeywordChange(newURL.href, keyword, source);
            }
          } else if (keywordsToRemove.length) {
            await notifyKeywordChange(oldURL.href, "", source);
          }
        }
      );
    },

    removeFromURLsIfNotBookmarked(urls) {
      let hrefs = new Set();
      for (let url of urls) {
        try {
          url = BOOKMARK_VALIDATORS.url(url);
        } catch (ex) {
          throw new Error(url + " is not a valid URL");
        }
        hrefs.add(url.href);
      }
      return PlacesUtils.withConnectionWrapper(
        "PlacesUtils.keywords.removeFromURLsIfNotBookmarked",
        async function (db) {
          let keywordsByHref = new Map();
          let cache = await promiseKeywordsCache();
          for (let [keyword, entry] of cache) {
            let href = entry.url.href;
            if (!hrefs.has(href)) {
              continue;
            }
            if (!keywordsByHref.has(href)) {
              keywordsByHref.set(href, [keyword]);
              continue;
            }
            let existingKeywords = keywordsByHref.get(href);
            existingKeywords.push(keyword);
          }
          if (!keywordsByHref.size) {
            return;
          }

          let placeInfosToRemove = [];
          let rows = await db.execute(
            `
            SELECT h.id, h.url
            FROM moz_places h
            JOIN moz_keywords k ON k.place_id = h.id
            GROUP BY h.id
            HAVING h.foreign_count = count(*) +
              (SELECT count(*)
               FROM moz_bookmarks b
               JOIN moz_bookmarks p ON b.parent = p.id
               WHERE p.parent = :tags_root AND b.fk = h.id)
            `,
            { tags_root: PlacesUtils.tagsFolderId }
          );
          for (let row of rows) {
            placeInfosToRemove.push({
              placeId: row.getResultByName("id"),
              href: row.getResultByName("url"),
            });
          }
          if (!placeInfosToRemove.length) {
            return;
          }

          await db.execute(
            `DELETE FROM moz_keywords WHERE place_id IN (${Array.from(
              placeInfosToRemove.map(info => info.placeId)
            ).join()})`
          );
          for (let { href } of placeInfosToRemove) {
            let keywords = keywordsByHref.get(href);
            for (let keyword of keywords) {
              cache.delete(keyword);
            }
          }
        }
      );
    },

    eraseEverything() {
      return PlacesUtils.withConnectionWrapper(
        "PlacesUtils.keywords.eraseEverything",
        async function (db) {
          let cache = await promiseKeywordsCache();
          if (!cache.size) {
            return;
          }
          await db.executeCached(`DELETE FROM moz_keywords`);
          cache.clear();
        }
      );
    },

    invalidateCachedKeywords() {
      gKeywordsCachePromise = gKeywordsCachePromise.then(_ => null);
      this.ensureCacheInitialized();
      return gKeywordsCachePromise;
    },
    async ensureCacheInitialized() {
      this._cache = await promiseKeywordsCache();
    },
    _cache: null,
    isKeywordFromCache(keyword) {
      return this._cache?.has(keyword);
    },
  },
};

var gKeywordsCachePromise = Promise.resolve(null);

function promiseKeywordsCache() {
  let promise = gKeywordsCachePromise.then(function (cache) {
    if (cache) {
      return cache;
    }
    return PlacesUtils.withConnectionWrapper(
      "PlacesUtils: promiseKeywordsCache",
      async db => {
        let newCache = new Map();
        let rows = await db.execute(
          `SELECT keyword, url, post_data
           FROM moz_keywords k
           JOIN moz_places h ON h.id = k.place_id
          `
        );
        let brokenKeywords = [];
        for (let row of rows) {
          let keyword = row.getResultByName("keyword");
          let url = URL.parse(row.getResultByName("url"));
          if (url) {
            let entry = {
              keyword,
              url,
              postData: row.getResultByName("post_data") || null,
            };
            newCache.set(keyword, entry);
          } else {
            brokenKeywords.push(keyword);
          }
        }
        if (brokenKeywords.length) {
          await db.execute(
            `DELETE FROM moz_keywords
             WHERE keyword IN (${brokenKeywords.map(kw => JSON.stringify(kw)).join(",")})
            `
          );
        }
        return newCache;
      }
    );
  });
  gKeywordsCachePromise = promise.catch(_ => new Map());
  return promise;
}

ChromeUtils.defineLazyGetter(PlacesUtils, "history", function () {
  let hs = Cc["@mozilla.org/browser/nav-history-service;1"].getService(
    Ci.nsINavHistoryService
  );
  return Object.freeze(
    new Proxy(hs, {
      get(target, name) {
        let property, object;
        if (name in target) {
          property = target[name];
          object = target;
        } else {
          property = lazy.History[name];
          object = lazy.History;
        }
        if (typeof property == "function") {
          return property.bind(object);
        }
        return property;
      },
      set(target, name, val) {
        if (name in target) {
          target[name] = val;
          return true;
        }
        return false;
      },
    })
  );
});

XPCOMUtils.defineLazyServiceGetter(
  PlacesUtils,
  "favicons",
  "@mozilla.org/browser/favicon-service;1",
  Ci.nsIFaviconService
);

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "bmsvc",
  "@mozilla.org/browser/nav-bookmarks-service;1",
  Ci.nsINavBookmarksService
);
ChromeUtils.defineLazyGetter(PlacesUtils, "bookmarks", () => {
  return Object.freeze(
    new Proxy(lazy.Bookmarks, {
      get: (target, name) =>
        lazy.Bookmarks.hasOwnProperty(name)
          ? lazy.Bookmarks[name]
          : lazy.bmsvc[name],
    })
  );
});

XPCOMUtils.defineLazyServiceGetter(
  PlacesUtils,
  "tagging",
  "@mozilla.org/browser/tagging-service;1",
  Ci.nsITaggingService
);

ChromeUtils.defineLazyGetter(lazy, "bundle", function () {
  const PLACES_STRING_BUNDLE_URI = "chrome://places/locale/places.properties";
  return Services.strings.createBundle(PLACES_STRING_BUNDLE_URI);
});

ChromeUtils.defineLazyGetter(PlacesUtils, "instanceId", () => {
  return PlacesUtils.history.makeGuid();
});

function setupDbForShutdown(conn, name) {
  try {
    let state = "0. Not started.";
    let promiseClosed = new Promise((resolve, reject) => {
      try {
        PlacesUtils.history.connectionShutdownClient.jsclient.addBlocker(
          `${name} closing as part of Places shutdown`,
          async function () {
            state = "1. Service has initiated shutdown";

            try {
              await conn.close();
              state = "2. Closed Sqlite.sys.mjs connection.";
              resolve();
            } catch (ex) {
              state = "2. Failed to closed Sqlite.sys.mjs connection: " + ex;
              reject(ex);
            }
          },
          () => state
        );
      } catch (ex) {
        conn.close();
        reject(ex);
      }
    }).catch(console.error);

    lazy.Sqlite.shutdown.addBlocker(
      `${name} must be closed before Sqlite.sys.mjs`,
      () => promiseClosed,
      () => state
    );
  } catch (ex) {
    conn.close();
    throw ex;
  }
}

ChromeUtils.defineLazyGetter(lazy, "gAsyncDBConnPromised", () =>
  lazy.Sqlite.cloneStorageConnection({
    connection: PlacesUtils.history.DBConnection,
    readOnly: true,
  })
    .then(conn => {
      setupDbForShutdown(conn, "PlacesUtils read-only connection");
      return conn;
    })
    .catch(console.error)
);

ChromeUtils.defineLazyGetter(lazy, "gAsyncDBWrapperPromised", () =>
  lazy.Sqlite.wrapStorageConnection({
    connection: PlacesUtils.history.DBConnection,
  })
    .then(conn => {
      setupDbForShutdown(conn, "PlacesUtils wrapped connection");
      return conn;
    })
    .catch(console.error)
);

var gAsyncDBLargeCacheConnDeferred = Promise.withResolvers();
ChromeUtils.defineLazyGetter(lazy, "gAsyncDBLargeCacheConnPromised", () =>
  lazy.Sqlite.cloneStorageConnection({
    connection: PlacesUtils.history.DBConnection,
    readOnly: true,
  })
    .then(async conn => {
      setupDbForShutdown(conn, "PlacesUtils large cache read-only connection");
      await conn.execute("PRAGMA cache_size = -6144"); 
      await conn.execute(`
        CREATE TEMP TABLE IF NOT EXISTS moz_openpages_temp (
          url TEXT NOT NULL,
          userContextId INTEGER NOT NULL,
          groupId TEXT NOT NULL,
          open_count INTEGER,
          PRIMARY KEY (url, userContextId, groupId)
        )`);
      await conn.execute(`
        CREATE TEMP TRIGGER IF NOT EXISTS moz_openpages_temp_afterupdate_trigger
        AFTER UPDATE OF open_count ON moz_openpages_temp FOR EACH ROW
        WHEN NEW.open_count = 0
        BEGIN
          DELETE FROM moz_openpages_temp
          WHERE url = NEW.url
            AND userContextId = NEW.userContextId
            AND groupId = NEW.groupId;
        END`);
      gAsyncDBLargeCacheConnDeferred.resolve(conn);
      return conn;
    })
    .catch(console.error)
);
