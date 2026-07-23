/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


function wrapRequest(request) {
  return new Promise((resolve, reject) => {
    request.onsuccess = () => {
      resolve(request.result);
    };
    request.onerror = () => {
      reject(request.error);
    };
  });
}

function forwardGetters(cls, target, props) {
  for (let prop of props) {
    Object.defineProperty(cls.prototype, prop, {
      get() {
        return this[target][prop];
      },
    });
  }
}

function forwardProps(cls, target, props) {
  for (let prop of props) {
    Object.defineProperty(cls.prototype, prop, {
      get() {
        return this[target][prop];
      },
      set(value) {
        this[target][prop] = value;
      },
    });
  }
}

function wrapMethods(cls, target, methods) {
  for (let method of methods) {
    cls.prototype[method] = function (...args) {
      return wrapRequest(this[target][method](...args));
    };
  }
}

function forwardMethods(cls, target, methods) {
  for (let method of methods) {
    cls.prototype[method] = function (...args) {
      return this[target][method](...args);
    };
  }
}

export class Cursor {
  constructor(cursorRequest, source) {
    this.cursorRequest = cursorRequest;
    this.source = source;
    this.cursor = null;
  }

  get done() {
    return !this.cursor;
  }

  async awaitRequest() {
    this.cursor = await wrapRequest(this.cursorRequest);
    return this;
  }
}

function defineCursorUpdateMethods(cls, methods) {
  for (let method of methods) {
    cls.prototype[method] = async function (...args) {
      const promise = this.awaitRequest();
      this.cursor[method](...args);
      await promise;
    };
  }
}

defineCursorUpdateMethods(Cursor, [
  "advance",
  "continue",
  "continuePrimaryKey",
]);

forwardGetters(Cursor, "cursor", ["direction", "key", "primaryKey"]);
wrapMethods(Cursor, "cursor", ["delete", "update"]);

export class CursorWithValue extends Cursor {}

forwardGetters(CursorWithValue, "cursor", ["value"]);

class Cursed {
  constructor(cursed) {
    this.cursed = cursed;
  }

  openCursor(...args) {
    const cursor = new CursorWithValue(this.cursed.openCursor(...args), this);
    return cursor.awaitRequest();
  }

  openKeyCursor(...args) {
    const cursor = new Cursor(this.cursed.openKeyCursor(...args), this);
    return cursor.awaitRequest();
  }
}

wrapMethods(Cursed, "cursed", [
  "count",
  "get",
  "getAll",
  "getAllKeys",
  "getKey",
]);

class Index extends Cursed {
  constructor(index, objectStore) {
    super(index);

    this.objectStore = objectStore;
    this.index = index;
  }
}

forwardGetters(Index, "index", [
  "isAutoLocale",
  "keyPath",
  "locale",
  "multiEntry",
  "name",
  "unique",
]);

export class ObjectStore extends Cursed {
  constructor(store) {
    super(store);

    this.store = store;
  }

  createIndex(...args) {
    return new Index(this.store.createIndex(...args), this);
  }

  index(...args) {
    return new Index(this.store.index(...args), this);
  }
}

wrapMethods(ObjectStore, "store", ["add", "clear", "delete", "put"]);

forwardMethods(ObjectStore, "store", ["deleteIndex"]);

export class Transaction {
  constructor(transaction) {
    this.transaction = transaction;

    this._completionPromise = new Promise((resolve, reject) => {
      transaction.oncomplete = resolve;
      transaction.onerror = () => {
        reject(transaction.error);
      };
      transaction.onabort = () => {
        const error =
          transaction.error ||
          new DOMException("The operation has been aborted", "AbortError");
        reject(error);
      };
    });
  }

  objectStore(name) {
    return new ObjectStore(this.transaction.objectStore(name));
  }

  promiseComplete() {
    return this._completionPromise;
  }
}

forwardGetters(Transaction, "transaction", [
  "db",
  "mode",
  "error",
  "objectStoreNames",
]);

forwardMethods(Transaction, "transaction", ["abort"]);

export class IndexedDB {
  static open(dbName, version, onupgradeneeded = null) {
    let request = indexedDB.open(dbName, version);
    return this._wrapOpenRequest(request, onupgradeneeded);
  }

  static openForPrincipal(principal, dbName, options, onupgradeneeded = null) {
    const request = indexedDB.openForPrincipal(principal, dbName, options);
    return this._wrapOpenRequest(request, onupgradeneeded);
  }

  static _wrapOpenRequest(request, onupgradeneeded = null) {
    request.onupgradeneeded = event => {
      let db = new this(request.result);
      if (onupgradeneeded) {
        onupgradeneeded(db, event);
      } else {
        db.onupgradeneeded(event);
      }
    };

    return wrapRequest(request).then(db => new this(db));
  }

  constructor(db) {
    this.db = db;
  }

  onupgradeneeded() {}

  transaction(storeNames, mode) {
    return new Transaction(this.db.transaction(storeNames, mode));
  }

  objectStore(storeName, mode) {
    let transaction = this.transaction([storeName], mode);
    return transaction.objectStore(storeName);
  }

  createObjectStore(...args) {
    return new ObjectStore(this.db.createObjectStore(...args));
  }
}

for (let method of ["cmp", "deleteDatabase"]) {
  IndexedDB[method] = function (...args) {
    return indexedDB[method](...args);
  };
}

forwardMethods(IndexedDB, "db", [
  "addEventListener",
  "close",
  "deleteObjectStore",
  "hasEventListener",
  "removeEventListener",
]);

forwardGetters(IndexedDB, "db", ["name", "objectStoreNames", "version"]);

forwardProps(IndexedDB, "db", [
  "onabort",
  "onclose",
  "onerror",
  "onversionchange",
]);
