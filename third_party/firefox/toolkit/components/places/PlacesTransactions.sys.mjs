/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const TRANSACTIONS_QUEUE_TIMEOUT_MS = 240000; 

import { PlacesUtils } from "resource://gre/modules/PlacesUtils.sys.mjs";

function setTimeout(callback, ms) {
  let timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
  timer.initWithCallback(callback, ms, timer.TYPE_ONE_SHOT);
}

const lazy = {};
ChromeUtils.defineLazyGetter(lazy, "logger", function () {
  return PlacesUtils.getLogger({ prefix: "Transactions" });
});

class TransactionsHistoryArray extends Array {
  constructor() {
    super();

    this._undoPosition = 0;
    this.proxifiedToRaw = new WeakMap();
  }

  get undoPosition() {
    return this._undoPosition;
  }

  get topUndoEntry() {
    return this.undoPosition < this.length ? this[this.undoPosition] : null;
  }
  get topRedoEntry() {
    return this.undoPosition > 0 ? this[this.undoPosition - 1] : null;
  }

  proxifyTransaction(rawTransaction) {
    let proxy = Object.freeze({
      transact(inBatch, batchIndex) {
        return TransactionsManager.transact(this, inBatch, batchIndex);
      },
      toString() {
        return rawTransaction.toString();
      },
    });
    this.proxifiedToRaw.set(proxy, rawTransaction);
    return proxy;
  }

  isProxifiedTransactionObject(value) {
    return this.proxifiedToRaw.has(value);
  }

  getRawTransaction(proxy) {
    return this.proxifiedToRaw.get(proxy);
  }

  add(proxifiedTransaction, forceNewEntry = false) {
    if (!this.isProxifiedTransactionObject(proxifiedTransaction)) {
      throw new Error("aProxifiedTransaction is not a proxified transaction");
    }

    if (!this.length || forceNewEntry) {
      this.clearRedoEntries();
      lazy.logger.debug(`Adding transaction: ${proxifiedTransaction}`);
      this.unshift([proxifiedTransaction]);
    } else {
      lazy.logger.debug(`Adding transaction: ${proxifiedTransaction}`);
      this[this.undoPosition].unshift(proxifiedTransaction);
    }
  }

  clearUndoEntries() {
    +lazy.logger.debug("Clearing undo entries");
    if (this.undoPosition < this.length) {
      this.splice(this.undoPosition);
    }
  }

  clearRedoEntries() {
    lazy.logger.debug("Clearing redo entries");
    if (this.undoPosition > 0) {
      this.splice(0, this.undoPosition);
      this._undoPosition = 0;
    }
  }

  clearAllEntries() {
    lazy.logger.debug("Clearing all entries");
    if (this.length) {
      this.splice(0);
      this._undoPosition = 0;
    }
  }
}

ChromeUtils.defineLazyGetter(
  lazy,
  "TransactionsHistory",
  () => new TransactionsHistoryArray()
);

export var PlacesTransactions = {
  batch(transactionsToBatch, batchName) {
    if (!Array.isArray(transactionsToBatch) || !transactionsToBatch.length) {
      throw new Error("Must pass a non-empty array");
    }
    if (
      transactionsToBatch.some(
        o =>
          !lazy.TransactionsHistory.isProxifiedTransactionObject(o) &&
          typeof o != "function"
      )
    ) {
      throw new Error("Must pass only transactions or functions");
    }
    lazy.logger.debug(
      `Batch ${batchName}: ${transactionsToBatch.length} transactions`
    );
    return TransactionsManager.batch(async function () {
      lazy.logger.debug(`Batch ${batchName}: executing transactions`);
      let accumulatedResults = [];
      for (let txn of transactionsToBatch) {
        try {
          if (typeof txn == "function") {
            txn = txn(accumulatedResults);
          }
          accumulatedResults.push(
            await txn.transact(true, accumulatedResults.length)
          );
        } catch (ex) {
          accumulatedResults.push(undefined);
          lazy.logger.error(`Failed to execute batched transaction: ${ex}`);
        }
      }
      return accumulatedResults;
    });
  },

  undo() {
    lazy.logger.debug("undo() was invoked");
    return TransactionsManager.undo();
  },

  redo() {
    lazy.logger.debug("redo() was invoked");
    return TransactionsManager.redo();
  },

  clearTransactionsHistory(undoEntries = true, redoEntries = true) {
    lazy.logger.debug("clearTransactionsHistory() was invoked");
    return TransactionsManager.clearTransactionsHistory(
      undoEntries,
      redoEntries
    );
  },

  get length() {
    return lazy.TransactionsHistory.length;
  },

  entry(index) {
    if (!Number.isInteger(index) || index < 0 || index >= this.length) {
      throw new Error("Invalid index");
    }

    return lazy.TransactionsHistory[index];
  },

  get undoPosition() {
    return lazy.TransactionsHistory.undoPosition;
  },

  get topUndoEntry() {
    return lazy.TransactionsHistory.topUndoEntry;
  },

  get topRedoEntry() {
    return lazy.TransactionsHistory.topRedoEntry;
  },
};

function Enqueuer(name) {
  this._promise = Promise.resolve();
  this._name = name;
}
Enqueuer.prototype = {
  enqueue(func) {
    lazy.logger.debug(`${this._name} enqueing`);
    let timeoutPromise = new Promise((resolve, reject) => {
      setTimeout(
        () =>
          reject(
            new Error(
              "PlacesTransaction timeout, most likely caused by unresolved pending work."
            )
          ),
        TRANSACTIONS_QUEUE_TIMEOUT_MS
      );
    });
    let promise = this._promise.then(() =>
      Promise.race([func(), timeoutPromise])
    );

    this._promise = promise.catch(lazy.logger.error);
    return promise;
  },

  get promise() {
    return this._promise;
  },
};

var TransactionsManager = {
  _mainEnqueuer: new Enqueuer("MainEnqueuer"),

  _executedTransactions: new WeakSet(),

  transact(txnProxy, inBatch = false, batchIndex = undefined) {
    let rawTxn = lazy.TransactionsHistory.getRawTransaction(txnProxy);
    if (!rawTxn) {
      throw new Error("|transact| was called with an unexpected object");
    }

    if (this._executedTransactions.has(rawTxn)) {
      throw new Error("Transactions objects may not be recycled.");
    }

    lazy.logger.debug(`transact() enqueue: ${txnProxy}`);

    this._executedTransactions.add(rawTxn);

    return (async () => {
      lazy.logger.debug(`transact execute(): ${txnProxy}`);
      let retval = await rawTxn.execute();

      let forceNewEntry = !inBatch || batchIndex === 0;
      lazy.TransactionsHistory.add(txnProxy, forceNewEntry);

      this._updateCommandsOnActiveWindow();
      return retval;
    })();
  },

  batch(task) {
    return this._mainEnqueuer.enqueue(task);
  },

  undo() {
    let promise = this._mainEnqueuer.enqueue(async () => {
      lazy.logger.debug("Undo execute");
      let entry = lazy.TransactionsHistory.topUndoEntry;
      if (!entry) {
        return;
      }

      for (let txnProxy of entry) {
        try {
          await lazy.TransactionsHistory.getRawTransaction(txnProxy).undo();
        } catch (ex) {
          console.error(ex, "Can't undo a transaction, clearing undo entries.");
          lazy.TransactionsHistory.clearUndoEntries();
          return;
        }
      }
      lazy.TransactionsHistory._undoPosition++;
      this._updateCommandsOnActiveWindow();
    });
    return promise;
  },

  redo() {
    let promise = this._mainEnqueuer.enqueue(async () => {
      lazy.logger.debug("Redo execute");
      let entry = lazy.TransactionsHistory.topRedoEntry;
      if (!entry) {
        return;
      }

      for (let i = entry.length - 1; i >= 0; i--) {
        let transaction = lazy.TransactionsHistory.getRawTransaction(entry[i]);
        try {
          if (transaction.redo) {
            await transaction.redo();
          } else {
            await transaction.execute();
          }
        } catch (ex) {
          console.error(ex, "Can't redo a transaction, clearing redo entries.");
          lazy.TransactionsHistory.clearRedoEntries();
          return;
        }
      }
      lazy.TransactionsHistory._undoPosition--;
      this._updateCommandsOnActiveWindow();
    });
    return promise;
  },

  clearTransactionsHistory(undoEntries, redoEntries) {
    let promise = this._mainEnqueuer.enqueue(function () {
      lazy.logger.debug(`ClearTransactionsHistory execute`);
      if (undoEntries && redoEntries) {
        lazy.TransactionsHistory.clearAllEntries();
      } else if (undoEntries) {
        lazy.TransactionsHistory.clearUndoEntries();
      } else if (redoEntries) {
        lazy.TransactionsHistory.clearRedoEntries();
      } else {
        throw new Error("either aUndoEntries or aRedoEntries should be true");
      }
    });
    return promise;
  },

  _updateCommandsOnActiveWindow() {
    try {
      let win = Services.focus.activeWindow;
      if (win) {
        // @ts-ignore - Bug 1954851
        win.updateCommands("undo");
      }
    } catch (ex) {
      console.error(ex, "Couldn't update undo commands.");
    }
  },
};

function DefineTransaction(requiredProps = [], optionalProps = []) {
  for (let prop of [...requiredProps, ...optionalProps]) {
    if (!DefineTransaction.inputProps.has(prop)) {
      throw new Error("Property '" + prop + "' is not defined");
    }
  }

  let ctor = function (input) {
    // @ts-ignore - Typescript is not yet able to identify this correctly.
    if (this == PlacesTransactions) {
      return new ctor(input);
    }

    if (requiredProps.length || optionalProps.length) {
      input = DefineTransaction.verifyInput(
        input,
        requiredProps,
        optionalProps
      );
      this.execute = this.execute.bind(this, input);
    }
    return lazy.TransactionsHistory.proxifyTransaction(this);
  };
  return ctor;
}

function simpleValidateFunc(checkFn) {
  return v => {
    if (!checkFn(v)) {
      throw new Error("Invalid value");
    }
    return v;
  };
}

DefineTransaction.strValidate = simpleValidateFunc(v => typeof v == "string");
DefineTransaction.strOrNullValidate = simpleValidateFunc(
  v => typeof v == "string" || v === null
);
DefineTransaction.indexValidate = simpleValidateFunc(
  v => Number.isInteger(v) && v >= PlacesUtils.bookmarks.DEFAULT_INDEX
);
DefineTransaction.guidValidate = simpleValidateFunc(v =>
  /^[a-zA-Z0-9\-_]{12}$/.test(v)
);

function isPrimitive(v) {
  return v === null || (typeof v != "object" && typeof v != "function");
}

function checkProperty(obj, prop, required, checkFn) {
  if (prop in obj) {
    return checkFn(obj[prop]);
  }

  return !required;
}

DefineTransaction.childObjectValidate = function (obj) {
  if (
    obj &&
    checkProperty(obj, "title", false, v => typeof v == "string") &&
    !("type" in obj && obj.type != PlacesUtils.bookmarks.TYPE_BOOKMARK)
  ) {
    obj.url = DefineTransaction.urlValidate(obj.url);
    let validKeys = ["title", "url"];
    if (Object.keys(obj).every(k => validKeys.includes(k))) {
      return obj;
    }
  }
  throw new Error("Invalid child object");
};

DefineTransaction.urlValidate = function (url) {
  if (url instanceof Ci.nsIURI) {
    return URL.fromURI(url);
  }
  return new URL(url);
};

DefineTransaction.inputProps = new Map();
DefineTransaction.defineInputProps = function (
  names,
  validateFn,
  defaultValue
) {
  for (let name of names) {
    this.inputProps.set(name, {
      validateValue(value) {
        if (value === undefined) {
          return defaultValue;
        }
        try {
          return validateFn(value);
        } catch (ex) {
          throw new Error(`Invalid value for input property ${name}: ${ex}`);
        }
      },

      validateInput(input, required) {
        if (required && !(name in input)) {
          throw new Error(`Required input property is missing: ${name}`);
        }
        return this.validateValue(input[name]);
      },

      isArrayProperty: false,
    });
  }
};

DefineTransaction.defineArrayInputProp = function (name, basePropertyName) {
  let baseProp = this.inputProps.get(basePropertyName);
  if (!baseProp) {
    throw new Error(`Unknown input property: ${basePropertyName}`);
  }

  this.inputProps.set(name, {
    validateValue(aValue) {
      if (aValue == undefined) {
        return [];
      }

      if (!Array.isArray(aValue)) {
        throw new Error(`${name} input property value must be an array`);
      }

      let newArray = [];
      for (let item of aValue) {
        newArray.push(baseProp.validateValue(item));
      }
      return newArray;
    },

    validateInput(input, required) {
      if (name in input) {
        if (basePropertyName in input) {
          throw new Error(`It is not allowed to set both ${name} and
                          ${basePropertyName} as  input properties`);
        }
        let array = this.validateValue(input[name]);
        if (required && !array.length) {
          throw new Error(`Empty array passed for required input property:
                           ${name}`);
        }
        return array;
      }
      if (required && !(basePropertyName in input)) {
        throw new Error(`Required input property is missing: ${name}`);
      }

      if (basePropertyName in input) {
        return [baseProp.validateValue(input[basePropertyName])];
      }

      return [];
    },

    isArrayProperty: true,
  });
};

DefineTransaction.validatePropertyValue = function (prop, input, required) {
  return this.inputProps.get(prop).validateInput(input, required);
};

DefineTransaction.getInputObjectForSingleValue = function (
  input,
  requiredProps,
  optionalProps
) {
  if (
    requiredProps.length > 1 ||
    (!requiredProps.length && optionalProps.length > 1)
  ) {
    throw new Error("Transaction input isn't an object");
  }

  let propName =
    requiredProps.length == 1 ? requiredProps[0] : optionalProps[0];
  let propValue =
    this.inputProps.get(propName).isArrayProperty && !Array.isArray(input)
      ? [input]
      : input;
  return { [propName]: propValue };
};

DefineTransaction.verifyInput = function (
  input,
  requiredProps = [],
  optionalProps = []
) {
  if (!requiredProps.length && !optionalProps.length) {
    return {};
  }

  let isSinglePropertyInput =
    isPrimitive(input) ||
    Array.isArray(input) ||
    input instanceof Ci.nsISupports;
  if (isSinglePropertyInput) {
    input = this.getInputObjectForSingleValue(
      input,
      requiredProps,
      optionalProps
    );
  }

  let fixedInput = {};
  for (let prop of requiredProps) {
    fixedInput[prop] = this.validatePropertyValue(prop, input, true);
  }
  for (let prop of optionalProps) {
    fixedInput[prop] = this.validatePropertyValue(prop, input, false);
  }

  return fixedInput;
};

DefineTransaction.defineInputProps(
  ["url"],
  DefineTransaction.urlValidate,
  null
);
DefineTransaction.defineInputProps(
  ["guid", "parentGuid", "newParentGuid"],
  DefineTransaction.guidValidate
);
DefineTransaction.defineInputProps(
  ["title", "postData"],
  DefineTransaction.strOrNullValidate,
  null
);
DefineTransaction.defineInputProps(
  ["keyword", "oldKeyword", "oldTag", "tag"],
  DefineTransaction.strValidate,
  ""
);
DefineTransaction.defineInputProps(
  ["index", "newIndex"],
  DefineTransaction.indexValidate,
  PlacesUtils.bookmarks.DEFAULT_INDEX
);
DefineTransaction.defineInputProps(
  ["child"],
  DefineTransaction.childObjectValidate
);
DefineTransaction.defineArrayInputProp("guids", "guid");
DefineTransaction.defineArrayInputProp("urls", "url");
DefineTransaction.defineArrayInputProp("tags", "tag");
DefineTransaction.defineArrayInputProp("children", "child");

function createItemsFromBookmarksTree(tree, restoring = false) {
  async function createItem(
    item,
    parentGuid,
    index = PlacesUtils.bookmarks.DEFAULT_INDEX
  ) {
    let guid;
    let info = { parentGuid, index };
    if (restoring) {
      info.guid = item.guid;
      info.dateAdded = PlacesUtils.toDate(item.dateAdded);
      info.lastModified = PlacesUtils.toDate(item.lastModified);
    }
    let shouldResetLastModified = false;
    switch (item.type) {
      case PlacesUtils.TYPE_X_MOZ_PLACE: {
        info.url = item.uri;
        if (typeof item.title == "string") {
          info.title = item.title;
        }

        guid = (await PlacesUtils.bookmarks.insert(info)).guid;

        if ("keyword" in item) {
          let { uri: url, keyword, postData } = item;
          await PlacesUtils.keywords.insert({ url, keyword, postData });
        }
        if ("tags" in item) {
          PlacesUtils.tagging.tagURI(
            Services.io.newURI(item.uri),
            item.tags.split(",")
          );
        }
        break;
      }
      case PlacesUtils.TYPE_X_MOZ_PLACE_CONTAINER: {
        info.type = PlacesUtils.bookmarks.TYPE_FOLDER;
        if (typeof item.title == "string") {
          info.title = item.title;
        }
        guid = (await PlacesUtils.bookmarks.insert(info)).guid;
        if ("children" in item) {
          for (let child of item.children) {
            await createItem(child, guid);
          }
        }
        if (restoring) {
          shouldResetLastModified = true;
        }
        break;
      }
      case PlacesUtils.TYPE_X_MOZ_PLACE_SEPARATOR: {
        info.type = PlacesUtils.bookmarks.TYPE_SEPARATOR;
        guid = (await PlacesUtils.bookmarks.insert(info)).guid;
        break;
      }
    }

    if (shouldResetLastModified) {
      let lastModified = PlacesUtils.toDate(item.lastModified);
      await PlacesUtils.bookmarks.update({ guid, lastModified });
    }

    return guid;
  }
  return createItem(tree, tree.parentGuid, tree.index);
}

var PT = PlacesTransactions;

PT.NewBookmark = DefineTransaction(
  ["parentGuid", "url"],
  ["index", "title", "tags"]
);
PT.NewBookmark.prototype = {
  async execute({ parentGuid, url, index, title, tags }) {
    let info = { parentGuid, index, url, title };
    if (tags.length) {
      let currentTags = PlacesUtils.tagging.getTagsForURI(url.URI);
      tags = tags.filter(t => !currentTags.includes(t));
    }

    async function createItem() {
      info = await PlacesUtils.bookmarks.insert(info);
      if (tags.length) {
        PlacesUtils.tagging.tagURI(url.URI, tags);
      }
    }

    await createItem();

    this.undo = async function () {
      await PlacesUtils.bookmarks.remove(info);
      if (tags.length) {
        PlacesUtils.tagging.untagURI(url.URI, tags);
      }
    };
    this.redo = async function () {
      await createItem();
    };
    return info.guid;
  },
  toString() {
    return "NewBookmark";
  },
};

PT.NewFolder = DefineTransaction(
  ["parentGuid", "title"],
  ["index", "children"]
);
PT.NewFolder.prototype = {
  async execute({ parentGuid, title, index, children }) {
    let folderGuid;
    let info = {
      children: [
        {
          guid: PlacesUtils.history.makeGuid(),
          title,
          type: PlacesUtils.bookmarks.TYPE_FOLDER,
        },
      ],
      guid: parentGuid,
    };

    if (children && children.length) {
      info.children[0].children = children.map(c => {
        c.guid = PlacesUtils.history.makeGuid();
        return c;
      });
    }

    async function createItem() {
      let bmInfo = await PlacesUtils.bookmarks.insertTree(info);
      folderGuid = bmInfo[0].guid;

      if (index != PlacesUtils.bookmarks.DEFAULT_INDEX) {
        bmInfo[0].index = index;
        bmInfo = await PlacesUtils.bookmarks.update(bmInfo[0]);
      }
    }
    await createItem();

    this.undo = async function () {
      await PlacesUtils.bookmarks.remove(folderGuid);
    };
    this.redo = async function () {
      await createItem();
    };
    return folderGuid;
  },
  toString() {
    return "NewFolder";
  },
};

PT.NewSeparator = DefineTransaction(["parentGuid"], ["index"]);
PT.NewSeparator.prototype = {
  async execute(info) {
    info.type = PlacesUtils.bookmarks.TYPE_SEPARATOR;
    info = await PlacesUtils.bookmarks.insert(info);
    this.undo = PlacesUtils.bookmarks.remove.bind(PlacesUtils.bookmarks, info);
    this.redo = PlacesUtils.bookmarks.insert.bind(PlacesUtils.bookmarks, info);
    return info.guid;
  },
  toString() {
    return "NewSeparator";
  },
};

PT.Move = DefineTransaction(["guids", "newParentGuid"], ["newIndex"]);
PT.Move.prototype = {
  async execute({ guids, newParentGuid, newIndex }) {
    let originalInfos = [];
    let index = newIndex;

    for (let guid of guids) {
      let originalInfo = await PlacesUtils.bookmarks.fetch(guid);
      if (!originalInfo) {
        throw new Error("Cannot move a non-existent item");
      }

      originalInfos.push(originalInfo);
    }

    await PlacesUtils.bookmarks.moveToFolder(guids, newParentGuid, index);

    this.undo = async function () {
      for (let info of originalInfos) {
        await PlacesUtils.bookmarks.update(info);
      }
    };
    this.redo = PlacesUtils.bookmarks.moveToFolder.bind(
      PlacesUtils.bookmarks,
      guids,
      newParentGuid,
      index
    );
    return guids;
  },
  toString() {
    return "Move";
  },
};

PT.EditTitle = DefineTransaction(["guid", "title"]);
PT.EditTitle.prototype = {
  async execute({ guid, title }) {
    let originalInfo = await PlacesUtils.bookmarks.fetch(guid);
    if (!originalInfo) {
      throw new Error("cannot update a non-existent item");
    }

    let updateInfo = { guid, title };
    updateInfo = await PlacesUtils.bookmarks.update(updateInfo);

    this.undo = PlacesUtils.bookmarks.update.bind(
      PlacesUtils.bookmarks,
      originalInfo
    );
    this.redo = PlacesUtils.bookmarks.update.bind(
      PlacesUtils.bookmarks,
      updateInfo
    );
  },
  toString() {
    return "EditTitle";
  },
};

PT.EditUrl = DefineTransaction(["guid", "url"]);
PT.EditUrl.prototype = {
  async execute({ guid, url }) {
    let originalInfo = await PlacesUtils.bookmarks.fetch(guid);
    if (!originalInfo) {
      throw new Error("cannot update a non-existent item");
    }
    if (originalInfo.type != PlacesUtils.bookmarks.TYPE_BOOKMARK) {
      throw new Error("Cannot edit url for non-bookmark items");
    }

    let uri = url.URI;
    let originalURI = originalInfo.url.URI;
    let originalTags = PlacesUtils.tagging.getTagsForURI(originalURI);
    let updatedInfo = { guid, url };
    let newURIAdditionalTags = null;

    async function updateItem() {
      updatedInfo = await PlacesUtils.bookmarks.update(updatedInfo);
      if (originalTags.length) {
        if (!(await PlacesUtils.bookmarks.fetch({ url: originalInfo.url }))) {
          PlacesUtils.tagging.untagURI(originalURI, originalTags);
        }
        let currentNewURITags = PlacesUtils.tagging.getTagsForURI(uri);
        newURIAdditionalTags = originalTags.filter(
          t => !currentNewURITags.includes(t)
        );
        if (newURIAdditionalTags && newURIAdditionalTags.length) {
          PlacesUtils.tagging.tagURI(uri, newURIAdditionalTags);
        }
      }
    }
    await updateItem();

    this.undo = async function () {
      await PlacesUtils.bookmarks.update(originalInfo);
      if (originalTags.length) {
        if (
          newURIAdditionalTags &&
          !!newURIAdditionalTags.length &&
          !(await PlacesUtils.bookmarks.fetch({ url }))
        ) {
          PlacesUtils.tagging.untagURI(uri, newURIAdditionalTags);
        }
        PlacesUtils.tagging.tagURI(originalURI, originalTags);
      }
    };

    this.redo = async function () {
      await updateItem();
    };
  },
  toString() {
    return "EditUrl";
  },
};

PT.EditKeyword = DefineTransaction(
  ["guid", "keyword"],
  ["postData", "oldKeyword"]
);
PT.EditKeyword.prototype = {
  async execute({ guid, keyword, postData, oldKeyword }) {
    let url;
    let oldKeywordEntry;
    if (oldKeyword) {
      oldKeywordEntry = await PlacesUtils.keywords.fetch(oldKeyword);
      url = oldKeywordEntry.url;
      await PlacesUtils.keywords.remove(oldKeyword);
    }

    if (keyword) {
      if (!url) {
        url = (await PlacesUtils.bookmarks.fetch(guid)).url;
      }
      await PlacesUtils.keywords.insert({
        url,
        keyword,
        postData: postData || (oldKeywordEntry ? oldKeywordEntry.postData : ""),
      });
    }

    this.undo = async function () {
      if (keyword) {
        await PlacesUtils.keywords.remove(keyword);
      }
      if (oldKeywordEntry) {
        await PlacesUtils.keywords.insert(oldKeywordEntry);
      }
    };
  },
  toString() {
    return "EditKeyword";
  },
};

PT.SortByName = DefineTransaction(["guid"]);
PT.SortByName.prototype = {
  async execute({ guid }) {
    let sortingMethod = (node_a, node_b) => {
      if (
        PlacesUtils.nodeIsContainer(node_a) &&
        !PlacesUtils.nodeIsContainer(node_b)
      ) {
        return -1;
      }
      if (
        !PlacesUtils.nodeIsContainer(node_a) &&
        PlacesUtils.nodeIsContainer(node_b)
      ) {
        return 1;
      }
      return node_a.title.localeCompare(node_b.title);
    };
    let oldOrderGuids = [];
    let newOrderGuids = [];
    let preSepNodes = [];

    let root = PlacesUtils.getFolderContents(guid, false, false).root;
    for (let i = 0, count = root.childCount; i < count; ++i) {
      let node = root.getChild(i);
      oldOrderGuids.push(node.bookmarkGuid);
      if (PlacesUtils.nodeIsSeparator(node)) {
        if (preSepNodes.length) {
          preSepNodes.sort(sortingMethod);
          newOrderGuids.push(...preSepNodes.map(n => n.bookmarkGuid));
          preSepNodes = [];
        }
        newOrderGuids.push(node.bookmarkGuid);
      } else {
        preSepNodes.push(node);
      }
    }
    root.containerOpen = false;
    if (preSepNodes.length) {
      preSepNodes.sort(sortingMethod);
      newOrderGuids.push(...preSepNodes.map(n => n.bookmarkGuid));
    }
    await PlacesUtils.bookmarks.reorder(guid, newOrderGuids);

    this.undo = async function () {
      await PlacesUtils.bookmarks.reorder(guid, oldOrderGuids);
    };
    this.redo = async function () {
      await PlacesUtils.bookmarks.reorder(guid, newOrderGuids);
    };
  },
  toString() {
    return "SortByName";
  },
};

PT.Remove = DefineTransaction(["guids"]);
PT.Remove.prototype = {
  async execute({ guids }) {
    let removedItems = [];

    for (let guid of guids) {
      try {
        removedItems.push(await PlacesUtils.promiseBookmarksTree(guid));
      } catch (ex) {
        if (!ex.becauseInvalidURL) {
          throw new Error(`Failed to get info for the guid: ${guid}: ${ex}`);
        }
        removedItems.push({ guid });
      }
    }

    let removeThem = async function () {
      if (removedItems.length) {
        await PlacesUtils.bookmarks.remove(
          removedItems.map(info => ({ guid: info.guid }))
        );
      }
    };
    await removeThem();

    this.undo = async function () {
      let createdItems = [];
      for (let info of removedItems) {
        try {
          await createItemsFromBookmarksTree(info, true);
          createdItems.push(info);
        } catch (ex) {
          console.error(`Unable to undo removal of ${info.guid}`);
        }
      }
      removedItems = createdItems;
    };
    this.redo = removeThem;
  },
  toString() {
    return "Remove";
  },
};

PT.Tag = DefineTransaction(["urls", "tags"]);
PT.Tag.prototype = {
  async execute({ urls, tags }) {
    let onUndo = [],
      onRedo = [];
    for (let url of urls) {
      if (!(await PlacesUtils.bookmarks.fetch({ url }))) {
        let createTxn = lazy.TransactionsHistory.getRawTransaction(
          PT.NewBookmark({
            url,
            tags,
            parentGuid: PlacesUtils.bookmarks.unfiledGuid,
          })
        );
        await createTxn.execute();
        onUndo.unshift(createTxn.undo.bind(createTxn));
        onRedo.push(createTxn.redo.bind(createTxn));
      } else {
        let uri = url.URI;
        let currentTags = PlacesUtils.tagging.getTagsForURI(uri);
        let newTags = tags.filter(t => !currentTags.includes(t));
        if (newTags.length) {
          PlacesUtils.tagging.tagURI(uri, newTags);
          onUndo.unshift(() => {
            PlacesUtils.tagging.untagURI(uri, newTags);
          });
          onRedo.push(() => {
            PlacesUtils.tagging.tagURI(uri, newTags);
          });
        }
      }
    }
    this.undo = async function () {
      for (let f of onUndo) {
        await f();
      }
    };
    this.redo = async function () {
      for (let f of onRedo) {
        await f();
      }
    };
  },
  toString() {
    return "Tag";
  },
};

PT.Untag = DefineTransaction(["urls"], ["tags"]);
PT.Untag.prototype = {
  execute({ urls, tags }) {
    let onUndo = [],
      onRedo = [];
    for (let url of urls) {
      let uri = url.URI;
      let tagsToRemove;
      let tagsSet = PlacesUtils.tagging.getTagsForURI(uri);
      if (tags.length) {
        tagsToRemove = tags.filter(t => tagsSet.includes(t));
      } else {
        tagsToRemove = tagsSet;
      }
      if (tagsToRemove.length) {
        PlacesUtils.tagging.untagURI(uri, tagsToRemove);
      }
      onUndo.unshift(() => {
        if (tagsToRemove.length) {
          PlacesUtils.tagging.tagURI(uri, tagsToRemove);
        }
      });
      onRedo.push(() => {
        if (tagsToRemove.length) {
          PlacesUtils.tagging.untagURI(uri, tagsToRemove);
        }
      });
    }
    this.undo = async function () {
      for (let f of onUndo) {
        await f();
      }
    };
    this.redo = async function () {
      for (let f of onRedo) {
        await f();
      }
    };
  },
  toString() {
    return "Untag";
  },
};

PT.RenameTag = DefineTransaction(["oldTag", "tag"]);
PT.RenameTag.prototype = {
  async execute({ oldTag, tag }) {
    let onUndo = [],
      onRedo = [];
    let urls = new Set();
    await PlacesUtils.bookmarks.fetch({ tags: [oldTag] }, b => urls.add(b.url));
    if (urls.size > 0) {
      let urlsAsArray = Array.from(urls);
      let tagTxn = lazy.TransactionsHistory.getRawTransaction(
        PT.Tag({ urls: urlsAsArray, tags: [tag] })
      );
      await tagTxn.execute();
      onUndo.unshift(tagTxn.undo.bind(tagTxn));
      onRedo.push(tagTxn.redo.bind(tagTxn));
      let untagTxn = lazy.TransactionsHistory.getRawTransaction(
        PT.Untag({ urls: urlsAsArray, tags: [oldTag] })
      );
      await untagTxn.execute();
      onUndo.unshift(untagTxn.undo.bind(untagTxn));
      onRedo.push(untagTxn.redo.bind(untagTxn));

      let db = await PlacesUtils.promiseDBConnection();
      let rows = await db.executeCached(
        `
        SELECT h.url, b.guid, b.title
        FROM moz_places h
        JOIN moz_bookmarks b ON b.fk = h.id
        WHERE url_hash BETWEEN hash("place", "prefix_lo")
                           AND hash("place", "prefix_hi")
          AND url LIKE :tagQuery
      `,
        { tagQuery: "%tag=%" }
      );
      for (let row of rows) {
        let url = row.getResultByName("url");
        try {
          url = new URL(url);
          let urlParams = new URLSearchParams(url.pathname);
          let tags = urlParams.getAll("tag");
          if (!tags.includes(oldTag)) {
            continue;
          }
          if (tags.length > 1) {
            urlParams.delete("tag");
            urlParams.set("tag", tag);
            url = new URL(
              url.protocol +
                urlParams +
                "&tag=" +
                tags.filter(t => t != oldTag).join("&tag=")
            );
          } else {
            urlParams.set("tag", tag);
            url = new URL(url.protocol + urlParams);
          }
        } catch (ex) {
          console.error(
            "Invalid bookmark url: " + row.getResultByName("url") + ": " + ex
          );
          continue;
        }
        let guid = row.getResultByName("guid");
        let title = row.getResultByName("title");

        let editUrlTxn = lazy.TransactionsHistory.getRawTransaction(
          PT.EditUrl({ guid, url })
        );
        await editUrlTxn.execute();
        onUndo.unshift(editUrlTxn.undo.bind(editUrlTxn));
        onRedo.push(editUrlTxn.redo.bind(editUrlTxn));
        if (title == oldTag) {
          let editTitleTxn = lazy.TransactionsHistory.getRawTransaction(
            PT.EditTitle({ guid, title: tag })
          );
          await editTitleTxn.execute();
          onUndo.unshift(editTitleTxn.undo.bind(editTitleTxn));
          onRedo.push(editTitleTxn.redo.bind(editTitleTxn));
        }
      }
    }
    this.undo = async function () {
      for (let f of onUndo) {
        await f();
      }
    };
    this.redo = async function () {
      for (let f of onRedo) {
        await f();
      }
    };
  },
  toString() {
    return "RenameTag";
  },
};

PT.Copy = DefineTransaction(["guid", "newParentGuid"], ["newIndex"]);
PT.Copy.prototype = {
  async execute({ guid, newParentGuid, newIndex }) {
    let creationInfo = null;
    try {
      creationInfo = await PlacesUtils.promiseBookmarksTree(guid);
    } catch (ex) {
      throw new Error(
        "Failed to get info for the specified item (guid: " +
          guid +
          "). Ex: " +
          ex
      );
    }
    creationInfo.parentGuid = newParentGuid;
    creationInfo.index = newIndex;

    let newItemGuid = await createItemsFromBookmarksTree(creationInfo, false);
    let newItemInfo = null;
    this.undo = async function () {
      if (!newItemInfo) {
        newItemInfo = await PlacesUtils.promiseBookmarksTree(newItemGuid);
      }
      await PlacesUtils.bookmarks.remove(newItemGuid);
    };
    this.redo = async function () {
      await createItemsFromBookmarksTree(newItemInfo, true);
    };

    return newItemGuid;
  },
  toString() {
    return "Copy";
  },
};
