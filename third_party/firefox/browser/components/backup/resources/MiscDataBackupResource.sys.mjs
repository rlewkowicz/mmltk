/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { BackupResource } from "resource:///modules/backup/BackupResource.sys.mjs";

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  ASRouterStorage: "resource:///modules/asrouter/ASRouterStorage.sys.mjs",
  ProfileAge: "resource://gre/modules/ProfileAge.sys.mjs",
});

const SNIPPETS_TABLE_NAME = "snippets";
const FILES_FOR_BACKUP = [
  "enumerate_devices.txt",
  "protections.sqlite",
  "SiteSecurityServiceState.bin",
];

export class MiscDataBackupResource extends BackupResource {
  static get key() {
    return "miscellaneous";
  }

  static get requiresEncryption() {
    return false;
  }

  async backup(
    stagingPath,
    profilePath = PathUtils.profileDir,
    _isEncrypting = false
  ) {
    const files = ["enumerate_devices.txt", "SiteSecurityServiceState.bin"];
    await BackupResource.copyFiles(profilePath, stagingPath, files);

    const sqliteDatabases = ["protections.sqlite"];
    await BackupResource.copySqliteDatabases(
      profilePath,
      stagingPath,
      sqliteDatabases
    );


    let storage = new lazy.ASRouterStorage({
      storeNames: [SNIPPETS_TABLE_NAME],
    });
    let snippetsTable = await storage.getDbTable(SNIPPETS_TABLE_NAME);
    let snippetsObj = {};
    for (let key of await snippetsTable.getAllKeys()) {
      snippetsObj[key] = await snippetsTable.get(key);
    }
    let snippetsBackupFile = PathUtils.join(
      stagingPath,
      "activity-stream-snippets.json"
    );
    await IOUtils.writeJSON(snippetsBackupFile, snippetsObj);

    return null;
  }

  async recover(_manifestEntry, recoveryPath, destProfilePath) {
    await BackupResource.copyFiles(
      recoveryPath,
      destProfilePath,
      FILES_FOR_BACKUP
    );

    let profileAge = await lazy.ProfileAge(destProfilePath);
    await profileAge.recordRecoveredFromBackup();

    let snippetsBackupFile = PathUtils.join(
      recoveryPath,
      "activity-stream-snippets.json"
    );
    return { snippetsBackupFile };
  }

  async postRecovery(postRecoveryEntry) {
    let { snippetsBackupFile } = postRecoveryEntry;

    if (!IOUtils.exists(snippetsBackupFile)) {
      return;
    }

    let snippetsData = await IOUtils.readJSON(snippetsBackupFile);
    let storage = new lazy.ASRouterStorage({
      storeNames: [SNIPPETS_TABLE_NAME],
    });
    let snippetsTable = await storage.getDbTable(SNIPPETS_TABLE_NAME);
    for (let key in snippetsData) {
      let value = snippetsData[key];
      await snippetsTable.set(key, value);
    }
  }

}
