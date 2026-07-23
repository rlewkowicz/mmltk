/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { DeferredTask } from "resource://gre/modules/DeferredTask.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const NOTIFY_TIMEOUT = 200;
const STOREID_PREF_NAME = "toolkit.profiles.storeID";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
  Sqlite: "resource://gre/modules/Sqlite.sys.mjs",
});
ChromeUtils.defineLazyGetter(lazy, "MigrationUtils", () => {
  if (AppConstants.MOZ_BUILD_APP !== "browser") {
    return undefined;
  }

  try {
    let { MigrationUtils } = ChromeUtils.importESModule(
      // eslint-disable-next-line mozilla/no-browser-refs-in-toolkit
      "resource:///modules/MigrationUtils.sys.mjs"
    );
    return MigrationUtils;
  } catch (e) {
    console.error(`Unable to load MigrationUtils.sys.mjs: ${e}`);
  }
  return undefined;
});

function getSharedProfilesStorePath(storeID) {
  return PathUtils.join(
    ProfilesDatastoreServiceClass.PROFILE_GROUPS_DIR,
    `${storeID}.sqlite`
  );
}

async function openDatastoreConnection(path) {
  let connection = await lazy.Sqlite.openConnection({
    path,
    openNotExclusive: true,
  });

  await connection.execute("PRAGMA journal_mode = WAL");
  await connection.execute("PRAGMA wal_autocheckpoint = 16");

  return connection;
}

export async function canDeleteProfile(profile) {
  if (!profile.storeID) {
    return true;
  }

  let dbPath = getSharedProfilesStorePath(profile.storeID);
  if (!(await IOUtils.exists(dbPath))) {
    return true;
  }

  try {
    let connection = await openDatastoreConnection(dbPath);

    try {
      let rows = await connection.executeCached(
        'SELECT COUNT(*) AS "count" FROM "Profiles";'
      );

      let profileCount = rows[0]?.getResultByName("count") ?? 0;

      return profileCount <= 1;
    } finally {
      await connection.close();
    }
  } catch (e) {
    console.error(e);
    return true;
  }
}

async function deleteFile(path) {
  try {
    await IOUtils.remove(path);
  } catch (e) {
  }
}

export async function deleteSharedProfilesStore(storeID) {
  let dbPath = getSharedProfilesStorePath(storeID);
  await deleteFile(dbPath);
  await deleteFile(dbPath + "-shm");
  await deleteFile(dbPath + "-wal");
}

class ProfilesDatastoreServiceClass {
  #connection = null;
  #asyncShutdownBlocker = null;
  #asyncShutdownBarrier = null;
  #initialized = false;
  #storeID = null;
  #initPromise = null;
  #notifyTask = null;
  #profileService = null;
  static #dirSvc = null;

  async getConnection() {
    await this.init();
    return this.#connection;
  }

  get shutdown() {
    return this.#asyncShutdownBarrier?.client;
  }

  async createTables() {
    let currentVersion = await this.#connection.getSchemaVersion();
    if (currentVersion == 7) {
      return;
    }

    if (currentVersion < 1) {
      await this.#connection.executeTransaction(async () => {
        const createProfilesTable = `
            CREATE TABLE IF NOT EXISTS "Profiles" (
              id      INTEGER NOT NULL,
              path    TEXT NOT NULL UNIQUE,
              name    TEXT NOT NULL,
              avatar  TEXT NOT NULL,
              themeId TEXT NOT NULL,
              themeFg TEXT NOT NULL,
              themeBg TEXT NOT NULL,
              PRIMARY KEY(id)
            );`;

        await this.#connection.execute(createProfilesTable);

        const createSharedPrefsTable = `
            CREATE TABLE IF NOT EXISTS "SharedPrefs" (
              id        INTEGER NOT NULL,
              name      TEXT NOT NULL UNIQUE,
              value     BLOB,
              isBoolean INTEGER,
              PRIMARY KEY(id)
            );`;

        await this.#connection.execute(createSharedPrefsTable);
      });

      await this.#connection.setSchemaVersion(1);
    }

    if (currentVersion < 2) {
      await this.#connection.executeTransaction(async () => {
        const createEnrollmentsTable = `
          CREATE TABLE IF NOT EXISTS "NimbusEnrollments" (
            id             INTEGER NOT NULL,
            profileId      INTEGER NOT NULL,
            slug           TEXT NOT NULL,
            branchSlug     TEXT NOT NULL,
            recipe         JSONB,
            active         BOOLEAN NOT NULL,
            unenrollReason TEXT,
            lastSeen       TEXT NOT NULL,
            setPrefs       JSONB,
            prefFlips      JSONB,
            source         TEXT NOT NULL,
            PRIMARY KEY(id),
            UNIQUE (profileId, slug) ON CONFLICT FAIL
          );
        `;

        await this.#connection.execute(createEnrollmentsTable);
      });

      await this.#connection.setSchemaVersion(2);
    }

    if (currentVersion < 3) {
      await this.#connection.executeTransaction(async () => {
        await this.#connection.execute("DELETE FROM NimbusEnrollments;");
      });
      await this.#connection.setSchemaVersion(3);
    }

    if (currentVersion < 4) {
      await this.#connection.executeTransaction(async () => {
        await this.#connection.execute("DELETE FROM NimbusEnrollments;");
      });
      await this.#connection.setSchemaVersion(4);
    }

    if (currentVersion < 5) {
      await this.#connection.executeTransaction(async () => {
        const createHeartbeatTable = `
            CREATE TABLE IF NOT EXISTS "Heartbeats" (
              id              INTEGER NOT NULL,
              recipeId        TEXT NOT NULL UNIQUE,
              lastShown       INTEGER,
              lastInteraction	INTEGER,
              PRIMARY KEY(id)
            );`;

        await this.#connection.execute(createHeartbeatTable);
      });

      await this.#connection.setSchemaVersion(5);
    }

    if (currentVersion < 6) {
      await this.#connection.executeTransaction(async () => {
        const createMessageImpressionsTable = `
          CREATE TABLE IF NOT EXISTS "MessagingSystemMessageImpressions" (
            id                  INTEGER PRIMARY KEY,
            messageId           TEXT UNIQUE NOT NULL,
            impressions         JSONB
          );
        `;

        const createMessageBlocklistTable = `
          CREATE TABLE IF NOT EXISTS "MessagingSystemMessageBlocklist" (
            id                  INTEGER PRIMARY KEY,
            messageId           TEXT UNIQUE NOT NULL
          );
        `;

        await this.#connection.execute(createMessageImpressionsTable);
        await this.#connection.execute(createMessageBlocklistTable);
      });

      await this.#connection.setSchemaVersion(6);
    }

    if (currentVersion < 7) {
      await this.#connection.executeTransaction(async () => {
        const createNimbusSyncTable = `
          CREATE TABLE IF NOT EXISTS "NimbusSyncTimestamps" (
            id                  INTEGER NOT NULL,
            profileId           TEXT NOT NULL,
            collection          TEXT NOT NULL,
            lastModified        INTEGER NOT NULL,
            PRIMARY KEY(id),
            UNIQUE (profileId, collection) ON CONFLICT FAIL
          );
        `;

        await this.#connection.execute(createNimbusSyncTable);
      });

      await this.#connection.setSchemaVersion(7);
    }
  }

  notify() {
    this.#notifyTask.arm();
  }

  #datastoreChanged(source) {
    Services.obs.notifyObservers(null, "pds-datastore-changed", source);
  }

  get storeID() {
    return new Promise(resolve => {
      this.init().then(() => {
        resolve(this.#storeID);
      });
    });
  }

  get initialized() {
    return this.#initialized;
  }

  static get PROFILE_GROUPS_DIR() {
    if (this.#dirSvc && "ProfileGroups" in this.#dirSvc) {
      return this.#dirSvc.ProfileGroups;
    }

    return PathUtils.join(
      ProfilesDatastoreServiceClass.getDirectory("UAppData").path,
      "Profile Groups"
    );
  }

  overrideDirectoryService(dirSvc) {
    if (!false) {
      return;
    }

    ProfilesDatastoreServiceClass.#dirSvc = dirSvc;
  }

  static getDirectory(id) {
    if (this.#dirSvc) {
      if (id in this.#dirSvc) {
        return this.#dirSvc[id].clone();
      }
    }

    return Services.dirsvc.get(id, Ci.nsIFile);
  }

  async resetProfileService(profileService) {
    if (!false) {
      return;
    }

    await this.uninit();
    this.#profileService =
      profileService ??
      Cc["@mozilla.org/toolkit/profile-service;1"].getService(
        Ci.nsIToolkitProfileService
      );
    await this.init();
  }

  get toolkitProfileService() {
    return this.#profileService;
  }

  constructor() {
    this.#asyncShutdownBlocker = () => this.uninit();
    this.#asyncShutdownBarrier = new lazy.AsyncShutdown.Barrier(
      "ProfilesDatastoreService: waiting for clients to finish pending writes"
    );
    this.#profileService = Cc[
      "@mozilla.org/toolkit/profile-service;1"
    ].getService(Ci.nsIToolkitProfileService);
  }

  init() {
    if (!this.#initPromise) {
      this.#initPromise = this.#init().finally(
        () => (this.#initPromise = null)
      );
    }

    return this.#initPromise;
  }

  async #init() {
    if (this.#initialized) {
      return;
    }

    this.#storeID = Services.startup.startingUp
      ? this.#profileService.currentProfile?.storeID
      : Services.prefs.getStringPref(STOREID_PREF_NAME, "");

    try {
      lazy.AsyncShutdown.profileChangeTeardown.addBlocker(
        "ProfilesDatastoreService uninit",
        this.#asyncShutdownBlocker
      );
    } catch (ex) {
      console.error(ex);
      return;
    }

    this.#notifyTask = new DeferredTask(async () => {
      this.#datastoreChanged("local");
    }, NOTIFY_TIMEOUT);

    try {
      await this.#initConnection();
    } catch (e) {
      console.error(e);

      await this.uninit();
      return;
    }

    this.#initialized = true;
  }

  async getStartupMigrationConnection() {
    if (!lazy.MigrationUtils?.isStartupMigration) {
      return null;
    }

    this.#storeID = Services.env.get("SELECTABLE_PROFILE_RESET_STORE_ID");
    await this.#initConnection();
    return this.#connection;
  }

  async uninit() {
    if (this.#asyncShutdownBarrier) {
      await this.#asyncShutdownBarrier.wait();
    }

    lazy.AsyncShutdown.profileChangeTeardown.removeBlocker(
      this.#asyncShutdownBlocker
    );

    if (this.#notifyTask.isArmed) {
      this.#notifyTask.disarm();
      this.#datastoreChanged("shutdown");
    }

    await this.closeConnection();

    this.#storeID = null;

    this.#initialized = false;
  }

  async #initConnection() {
    if (this.#connection) {
      return;
    }

    let path = await this.getProfilesStorePath();

    this.#connection = await openDatastoreConnection(path);

    await this.createTables();
  }

  async closeConnection() {
    if (!this.#connection) {
      return;
    }

    try {
      await this.#connection.close();
    } catch (ex) {}
    this.#connection = null;
  }

  maybeCreateStoreID() {
    if (this.#storeID) {
      return;
    }

    const storageID = Services.uuid
      .generateUUID()
      .toString()
      .replace("{", "")
      .split("-")[0];

    this.#storeID = storageID;
    Services.prefs.setStringPref(STOREID_PREF_NAME, storageID);
  }

  async getProfilesStorePath() {
    this.maybeCreateStoreID();

    if (
      !lazy.MigrationUtils?.isStartupMigration &&
      !this.#profileService.currentProfile
    ) {
      return PathUtils.join(
        ProfilesDatastoreServiceClass.getDirectory("ProfD").path,
        `${this.#storeID}.sqlite`
      );
    }

    await IOUtils.makeDirectory(
      ProfilesDatastoreServiceClass.PROFILE_GROUPS_DIR
    );

    return getSharedProfilesStorePath(this.#storeID);
  }
}

const ProfilesDatastoreService = new ProfilesDatastoreServiceClass();
export { ProfilesDatastoreService };
