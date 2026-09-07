/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import { PlacesUtils } from "resource://gre/modules/PlacesUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PlacesBackups: "resource://gre/modules/PlacesBackups.sys.mjs",
});

const OLD_BOOKMARK_QUERY_TRANSLATIONS = {
  PLACES_ROOT: PlacesUtils.bookmarks.rootGuid,
  BOOKMARKS_MENU: PlacesUtils.bookmarks.menuGuid,
  TAGS: PlacesUtils.bookmarks.tagsGuid,
  UNFILED_BOOKMARKS: PlacesUtils.bookmarks.unfiledGuid,
  TOOLBAR: PlacesUtils.bookmarks.toolbarGuid,
  MOBILE_BOOKMARKS: PlacesUtils.bookmarks.mobileGuid,
};

class HashConflictError extends Error {
  constructor(message) {
    super(message);
    this.name = "HashConflictError";
    this.becauseSameHash = true;
  }
}

export var BookmarkJSONUtils = {
  async importFromURL(
    aSpec,
    {
      replace: aReplace = false,
      source: aSource = aReplace
        ? PlacesUtils.bookmarks.SOURCES.RESTORE
        : PlacesUtils.bookmarks.SOURCES.IMPORT,
    } = {}
  ) {
    notifyObservers(PlacesUtils.TOPIC_BOOKMARKS_RESTORE_BEGIN, aReplace);
    let bookmarkCount = 0;
    try {
      let importer = new BookmarkImporter(aReplace, aSource);
      bookmarkCount = await importer.importFromURL(aSpec);

      notifyObservers(PlacesUtils.TOPIC_BOOKMARKS_RESTORE_SUCCESS, aReplace);
    } catch (ex) {
      console.error(`Failed to restore bookmarks from ${aSpec}:`, ex);
      notifyObservers(PlacesUtils.TOPIC_BOOKMARKS_RESTORE_FAILED, aReplace);
      throw ex;
    }
    return bookmarkCount;
  },

  async importFromFile(
    aFilePath,
    {
      replace: aReplace = false,
      source: aSource = aReplace
        ? PlacesUtils.bookmarks.SOURCES.RESTORE
        : PlacesUtils.bookmarks.SOURCES.IMPORT,
    } = {}
  ) {
    notifyObservers(PlacesUtils.TOPIC_BOOKMARKS_RESTORE_BEGIN, aReplace);
    let bookmarkCount = 0;
    try {
      if (!(await IOUtils.exists(aFilePath))) {
        throw new Error("Cannot restore from nonexisting json file");
      }

      let importer = new BookmarkImporter(aReplace, aSource);
      if (aFilePath.endsWith("jsonlz4")) {
        bookmarkCount = await importer.importFromCompressedFile(aFilePath);
      } else {
        bookmarkCount = await importer.importFromURL(
          PathUtils.toFileURI(aFilePath)
        );
      }
      notifyObservers(PlacesUtils.TOPIC_BOOKMARKS_RESTORE_SUCCESS, aReplace);
    } catch (ex) {
      console.error(`Failed to restore bookmarks from ${aFilePath}:`, ex);
      notifyObservers(PlacesUtils.TOPIC_BOOKMARKS_RESTORE_FAILED, aReplace);
      throw ex;
    }
    return bookmarkCount;
  },


  async exportToFile(aFilePath, aOptions = {}) {
    let [bookmarks, count] = await lazy.PlacesBackups.getBookmarksTree();
    let jsonString = JSON.stringify(bookmarks);

    let hash = PlacesUtils.sha256(jsonString, { format: "base64url" });

    if (hash === aOptions.failIfHashIs) {
      throw new HashConflictError("Hash conflict");
    }

    await IOUtils.writeUTF8(aFilePath, jsonString, {
      compress: aOptions.compress,
      tmpPath: PathUtils.join(aFilePath + ".tmp"),
    });
    return { count, hash };
  },
};

function BookmarkImporter(aReplace, aSource) {
  this._replace = aReplace;
  this._source = aSource;
}
BookmarkImporter.prototype = {
  async importFromURL(spec) {
    if (!spec.startsWith("chrome://") && !spec.startsWith("file://")) {
      throw new Error(
        "importFromURL can only be used with chrome:// and file:// URLs"
      );
    }
    let nodes = await (await fetch(spec)).json();

    // @ts-ignore
    if (!nodes.children || !nodes.children.length) {
      return 0;
    }

    return this.import(nodes);
  },

  importFromCompressedFile: async function BI_importFromCompressedFile(
    aFilePath
  ) {
    let result = await IOUtils.readUTF8(aFilePath, { decompress: true });
    return this.importFromJSON(result);
  },

  async importFromJSON(aString) {
    let nodes = PlacesUtils.unwrapNodes(
      aString,
      PlacesUtils.TYPE_X_MOZ_PLACE_CONTAINER
    ).validNodes;
    // @ts-ignore
    if (!nodes.length || !nodes[0].children || !nodes[0].children.length) {
      return 0;
    }

    return this.import(nodes[0]);
  },

  async import(rootNode) {
    let nodes = rootNode.children.filter(node => node.root !== "tagsFolder");

    if (this._replace) {
      await PlacesUtils.bookmarks.eraseEverything({ source: this._source });
    }

    let folderIdToGuidMap = {};

    for (let node of nodes) {
      if (!node.children || !node.children.length) {
        continue;
      } 

      node.source = this._source;

      let folders = translateTreeTypes(node);

      folderIdToGuidMap = Object.assign(folderIdToGuidMap, folders);
    }

    let bookmarkCount = 0;
    for (let node of nodes) {
      if (!node.children || !node.children.length) {
        continue;
      }

      if (!PlacesUtils.bookmarks.userContentRoots.includes(node.guid)) {
        continue;
      }

      fixupSearchQueries(node, folderIdToGuidMap);

      let bookmarks = await PlacesUtils.bookmarks.insertTree(node, {
        fixupOrSkipInvalidEntries: true,
      });
      bookmarkCount += bookmarks.filter(
        bookmark => bookmark.type == PlacesUtils.bookmarks.TYPE_BOOKMARK
      ).length;

      insertFaviconsForTree(node);
    }
    return bookmarkCount;
  },
};

function notifyObservers(topic, replace) {
  Services.obs.notifyObservers(null, topic, replace ? "json" : "json-append");
}

function fixupSearchQueries(aNode, aFolderIdMap) {
  if (aNode.url && aNode.url.startsWith("place:")) {
    aNode.url = fixupQuery(aNode.url, aFolderIdMap);
  }
  if (aNode.children) {
    for (let child of aNode.children) {
      fixupSearchQueries(child, aFolderIdMap);
    }
  }
}

function fixupQuery(aQueryURL, aFolderIdMap) {
  let invalid = false;
  let convert = function (str, existingFolderId) {
    let guid;
    if (
      Object.keys(OLD_BOOKMARK_QUERY_TRANSLATIONS).includes(existingFolderId)
    ) {
      guid = OLD_BOOKMARK_QUERY_TRANSLATIONS[existingFolderId];
    } else {
      guid = aFolderIdMap[existingFolderId];
      if (!guid) {
        invalid = true;
        return `invalidOldParentId=${existingFolderId}`;
      }
    }
    return `parent=${guid}`;
  };

  let url = aQueryURL.replace(/folder=([A-Za-z0-9_]+)/g, convert);
  if (invalid) {
    url += "&excludeItems=1";
  }
  return url;
}

const rootToFolderGuidMap = {
  placesRoot: PlacesUtils.bookmarks.rootGuid,
  bookmarksMenuFolder: PlacesUtils.bookmarks.menuGuid,
  unfiledBookmarksFolder: PlacesUtils.bookmarks.unfiledGuid,
  toolbarFolder: PlacesUtils.bookmarks.toolbarGuid,
  mobileFolder: PlacesUtils.bookmarks.mobileGuid,
};

function fixupRootFolderGuid(node) {
  if (!node.guid && node.root && node.root in rootToFolderGuidMap) {
    node.guid = rootToFolderGuidMap[node.root];
  }
}

function translateTreeTypes(node) {
  let folderIdToGuidMap = {};

  if (node.uri) {
    node.url = node.uri;
    delete node.uri;
  }

  switch (node.type) {
    case PlacesUtils.TYPE_X_MOZ_PLACE_CONTAINER: {
      node.type = PlacesUtils.bookmarks.TYPE_FOLDER;

      let isMobileFolder =
        node.annos &&
        node.annos.some(anno => anno.name == PlacesUtils.MOBILE_ROOT_ANNO);
      if (isMobileFolder) {
        node.guid = PlacesUtils.bookmarks.mobileGuid;
      } else {
        fixupRootFolderGuid(node);
      }

      folderIdToGuidMap[node.id] = node.guid;
      break;
    }
    case PlacesUtils.TYPE_X_MOZ_PLACE:
      node.type = PlacesUtils.bookmarks.TYPE_BOOKMARK;
      break;
    case PlacesUtils.TYPE_X_MOZ_PLACE_SEPARATOR:
      node.type = PlacesUtils.bookmarks.TYPE_SEPARATOR;
      if ("title" in node) {
        delete node.title;
      }
      break;
    default:
      console.error("Unexpected bookmark type", node.type);
      break;
  }

  if (node.dateAdded) {
    node.dateAdded = PlacesUtils.toDate(node.dateAdded);
  }

  if (node.lastModified) {
    let lastModified = PlacesUtils.toDate(node.lastModified);
    if (lastModified >= node.dateAdded) {
      node.lastModified = lastModified;
    } else {
      delete node.lastModified;
    }
  }

  if (node.tags) {
    node.tags = node.tags
      .split(",")
      .filter(
        aTag =>
          !!aTag.length && aTag.length <= PlacesUtils.bookmarks.MAX_TAG_LENGTH
      );

    if (!node.tags.length) {
      delete node.tags;
    }
  }

  if (node.postData == null) {
    delete node.postData;
  }

  if (!node.children) {
    return folderIdToGuidMap;
  }

  node.children = node.children.sort((a, b) => {
    return a.index - b.index;
  });

  for (let child of node.children) {
    let folders = translateTreeTypes(child);
    folderIdToGuidMap = Object.assign(folderIdToGuidMap, folders);
  }

  return folderIdToGuidMap;
}

function insertFaviconForNode(node) {
  if (!node.icon && !node.iconUri) {
    return;
  }

  try {
    let faviconDataURI = Services.io.newURI(node.icon || node.iconUri);
    if (!faviconDataURI.schemeIs("data")) {
      return;
    }

    PlacesUtils.favicons
      .setFaviconForPage(
        Services.io.newURI(node.url),
        Services.io.newURI(node.iconUri ?? "fake-favicon-uri:" + node.url),
        faviconDataURI
      )
      .catch(console.error);
  } catch (ex) {
    console.error("Failed to import favicon data:", ex);
  }
}

function insertFaviconsForTree(nodeTree) {
  insertFaviconForNode(nodeTree);

  if (nodeTree.children) {
    for (let child of nodeTree.children) {
      insertFaviconsForTree(child);
    }
  }
}
