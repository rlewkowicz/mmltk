/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


function promisify(fn, ...args) {
  return new Promise((resolve, reject) => {
    fn({ resolve, reject }, ...args);
  });
}

export class KeyValueService {
  static RecoveryStrategy = {
    ERROR: Ci.nsIKeyValueService.ERROR,
    DISCARD: Ci.nsIKeyValueService.DISCARD,
    RENAME: Ci.nsIKeyValueService.RENAME,
  };

  static #service = Cc["@mozilla.org/key-value-service;1"].getService(
    Ci.nsIKeyValueService
  );

  static async getOrCreate(dir, name) {
    return new KeyValueDatabase(
      await promisify(this.#service.getOrCreate, dir, name)
    );
  }

  static async getOrCreateWithOptions(
    dir,
    name,
    { strategy = Ci.nsIKeyValueService.RENAME } = {}
  ) {
    return new KeyValueDatabase(
      await promisify(this.#service.getOrCreateWithOptions, dir, name, strategy)
    );
  }
}

export class SQLiteKeyValueService {
  static Importer = {
    RKV_SAFE_MODE: "rkv-safe-mode",
  };

  static #service = Cc["@mozilla.org/sqlite-key-value-service;1"].getService(
    Ci.nsIKeyValueService
  );

  static async getOrCreate(dir, name) {
    return new KeyValueDatabase(
      await promisify(this.#service.getOrCreate, dir, name)
    );
  }

  static createImporter(type, dir) {
    return new KeyValueImporter(this.#service.createImporter(type, dir));
  }
}

export class KeyValueImporter {
  static ConflictPolicy = {
    ERROR: Ci.nsIKeyValueImporter.ERROR_ON_CONFLICT,
    IGNORE: Ci.nsIKeyValueImporter.IGNORE_ON_CONFLICT,
    REPLACE: Ci.nsIKeyValueImporter.REPLACE_ON_CONFLICT,
  };

  static CleanupPolicy = {
    KEEP: Ci.nsIKeyValueImporter.KEEP_AFTER_IMPORT,
    DELETE: Ci.nsIKeyValueImporter.DELETE_AFTER_IMPORT,
  };

  #importer;

  constructor(importer) {
    this.#importer = importer;
  }

  get type() {
    return this.#importer.type;
  }

  get path() {
    return this.#importer.path;
  }

  addPath(dir) {
    return this.#importer.addPath(dir);
  }

  addDatabase(name) {
    return this.#importer.addDatabase(name);
  }

  addAllDatabases() {
    return this.#importer.addAllDatabases();
  }

  import() {
    return promisify(this.#importer.import);
  }
}

class KeyValueDatabase {
  constructor(database) {
    this.database = database;
  }

  isEmpty() {
    return promisify(this.database.isEmpty);
  }

  count() {
    return promisify(this.database.count);
  }

  size() {
    return promisify(this.database.size);
  }

  put(key, value) {
    return promisify(this.database.put, key, value);
  }

  writeMany(pairs) {
    if (!pairs) {
      throw new Error("writeMany(): unexpected argument.");
    }

    let entries;

    if (
      pairs instanceof Map ||
      pairs instanceof Array ||
      typeof pairs[Symbol.iterator] === "function"
    ) {
      try {
        const map = pairs instanceof Map ? pairs : new Map(pairs);
        entries = Array.from(map, ([key, value]) => ({ key, value }));
      } catch (error) {
        throw new Error("writeMany(): unexpected argument.");
      }
    } else if (typeof pairs === "object") {
      entries = Array.from(Object.entries(pairs), ([key, value]) => ({
        key,
        value,
      }));
    } else {
      throw new Error("writeMany(): unexpected argument.");
    }

    if (entries.length) {
      return promisify(this.database.writeMany, entries);
    }
    return Promise.resolve();
  }

  has(key) {
    return promisify(this.database.has, key);
  }

  get(key, defaultValue) {
    return promisify(this.database.get, key, defaultValue);
  }

  delete(key) {
    return promisify(this.database.delete, key);
  }

  deleteRange(fromKey, toKey) {
    return promisify(this.database.deleteRange, fromKey, toKey);
  }

  clear() {
    return promisify(this.database.clear);
  }

  async enumerate(fromKey, toKey) {
    return new KeyValueEnumerator(
      await promisify(this.database.enumerate, fromKey, toKey)
    );
  }

  async close() {
    return promisify(this.database.close);
  }
}

class KeyValueEnumerator {
  constructor(enumerator) {
    this.enumerator = enumerator;
  }

  hasMoreElements() {
    return this.enumerator.hasMoreElements();
  }

  getNext() {
    return this.enumerator.getNext();
  }

  *[Symbol.iterator]() {
    while (this.enumerator.hasMoreElements()) {
      yield this.enumerator.getNext();
    }
  }
}
