/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  RemoteSettings: "resource://services-settings/remote-settings.sys.mjs",
});

const COLLECTION_NAME = "third-party-cookie-blocking-exempt-urls";
const PREF_NAME = "network.cookie.cookieBehavior.optInPartitioning.skip_list";

export class ThirdPartyCookieBlockingExceptionListService {
  classId = Components.ID("{1ee0cc18-c968-4105-a895-bdea08e187eb}");
  QueryInterface = ChromeUtils.generateQI([
    "nsIThirdPartyCookieBlockingExceptionListService",
  ]);

  #rs = null;
  #onSyncCallback = null;

  #prefValueSet = null;
  #rsValueSet = null;

  constructor() {
    this.#rs = lazy.RemoteSettings(COLLECTION_NAME);
  }

  async init() {
    await this.importAllExceptions();

    Services.prefs.addObserver(PREF_NAME, this);

    if (!this.#onSyncCallback) {
      this.#onSyncCallback = this.onSync.bind(this);
      this.#rs.on("sync", this.#onSyncCallback);
    }

    this.onPrefChange();
  }

  shutdown() {
    Services.prefs.removeObserver(PREF_NAME, this);

    if (this.#onSyncCallback) {
      this.#rs.off("sync", this.#onSyncCallback);
      this.#onSyncCallback = null;
    }
  }

  #handleExceptionChange(created = [], deleted = []) {
    if (created.length) {
      Services.cookies.addThirdPartyCookieBlockingExceptions(created);
    }
    if (deleted.length) {
      Services.cookies.removeThirdPartyCookieBlockingExceptions(deleted);
    }
  }

  onSync({ data: { created = [], updated = [], deleted = [] } }) {
    created = created.map(ex =>
      ThirdPartyCookieExceptionEntry.fromRemoteSettingsRecord(ex)
    );
    deleted = deleted.map(ex =>
      ThirdPartyCookieExceptionEntry.fromRemoteSettingsRecord(ex)
    );

    updated.forEach(ex => {
      let newEntry = ThirdPartyCookieExceptionEntry.fromRemoteSettingsRecord(
        ex.new
      );
      let oldEntry = ThirdPartyCookieExceptionEntry.fromRemoteSettingsRecord(
        ex.old
      );

      if (newEntry.equals(oldEntry)) {
        return;
      }
      created.push(newEntry);
      deleted.push(oldEntry);
    });

    this.#rsValueSet ??= new Set();

    for (const site of deleted) {
      this.#rsValueSet.delete(site.serialize());
    }

    for (const site of created) {
      this.#rsValueSet.add(site.serialize());
    }

    this.#handleExceptionChange(created, deleted);
  }

  onPrefChange() {
    let newExceptions = Services.prefs.getStringPref(PREF_NAME, "").split(";");

    newExceptions = newExceptions
      .map(ex => ThirdPartyCookieExceptionEntry.fromString(ex))
      .filter(Boolean);

    if (!this.#prefValueSet) {
      this.#handleExceptionChange({
        data: { created: newExceptions },
        prefUpdate: true,
      });
      this.#prefValueSet = new Set(newExceptions.map(ex => ex.serialize()));
      return;
    }


    let created = [...newExceptions].filter(
      ex => !this.#prefValueSet.has(ex.serialize())
    );

    let newExceptionStringSet = new Set(
      newExceptions.map(ex => ex.serialize())
    );

    let deleted = Array.from(this.#prefValueSet)
      .filter(item => !newExceptionStringSet.has(item))
      .map(ex => ThirdPartyCookieExceptionEntry.fromString(ex));

    if (this.#rsValueSet) {
      deleted = deleted.filter(ex => !this.#rsValueSet.has(ex.serialize()));
    }

    this.#prefValueSet = newExceptionStringSet;

    this.#handleExceptionChange(created, deleted);
  }

  observe(subject, topic, data) {
    if (topic != "nsPref:changed" || data != PREF_NAME) {
      throw new Error(`Unexpected event ${topic} with ${data}`);
    }

    this.onPrefChange();
  }

  async importAllExceptions() {
    try {
      let exceptions = await this.#rs.get();
      if (!exceptions.length) {
        return;
      }
      this.onSync({ data: { created: exceptions } });
    } catch (error) {
      console.error(
        "Error while importing 3pcb exceptions from RemoteSettings",
        error
      );
    }
  }
}

export class ThirdPartyCookieExceptionEntry {
  classId = Components.ID("{8200e12c-416c-42eb-8af5-db9745d2e527}");
  QueryInterface = ChromeUtils.generateQI([
    "nsIThirdPartyCookieExceptionEntry",
  ]);

  constructor(fpSite, tpSite) {
    this.firstPartySite = fpSite;
    this.thirdPartySite = tpSite;
  }

  serialize() {
    return `${this.firstPartySite},${this.thirdPartySite}`;
  }

  equals(other) {
    return (
      this.firstPartySite === other.firstPartySite &&
      this.thirdPartySite === other.thirdPartySite
    );
  }

  static fromString(exStr) {
    if (!exStr) {
      return null;
    }

    let [fpSite, tpSite] = exStr.split(",");
    try {
      fpSite = this.#sanitizeSite(fpSite, true);
      tpSite = this.#sanitizeSite(tpSite);

      return new ThirdPartyCookieExceptionEntry(fpSite, tpSite);
    } catch (e) {
      console.error(
        `Error while constructing 3pcd exception entry from string`,
        exStr
      );
      return null;
    }
  }

  static fromRemoteSettingsRecord(record) {
    try {
      let fpSite = this.#sanitizeSite(record.fpSite, true);
      let tpSite = this.#sanitizeSite(record.tpSite);

      return new ThirdPartyCookieExceptionEntry(fpSite, tpSite);
    } catch (e) {
      console.error(
        `Error while constructing 3pcd exception entry from RemoteSettings record`,
        record
      );
      return null;
    }
  }

  static #sanitizeSite(site, acceptWildcard = false) {
    if (acceptWildcard && site === "*") {
      return "*";
    }

    let uri = Services.io.newURI(site);
    return Services.eTLD.getSite(uri);
  }
}
