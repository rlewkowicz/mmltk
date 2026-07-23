/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { BackupResource } from "resource:///modules/backup/BackupResource.sys.mjs";
const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PlacesDBUtils: "resource://gre/modules/PlacesDBUtils.sys.mjs",
});

export class PlacesBackupResource extends BackupResource {
  static get key() {
    return "places";
  }

  static get requiresEncryption() {
    return false;
  }

  static get priority() {
    return 1;
  }

  static get canBackupResource() {
    return BackupResource.backingUpPlaces;
  }

  async backup(
    stagingPath,
    profilePath = PathUtils.profileDir,
    _isEncrypting = false
  ) {
    let copies = [
      BackupResource.copySqliteDatabases(profilePath, stagingPath, [
        "places.sqlite",
      ]),
      BackupResource.copySqliteDatabases(profilePath, stagingPath, [
        "favicons.sqlite",
      ]),
    ];
    await Promise.all(copies);

    await lazy.PlacesDBUtils.removeDownloadsMetadataFromDb(
      PathUtils.join(stagingPath, "places.sqlite")
    );

    return null;
  }

  async recover(manifestEntry, recoveryPath, destProfilePath) {
    const simpleCopyFiles = ["places.sqlite", "favicons.sqlite"];
    await BackupResource.copyFiles(
      recoveryPath,
      destProfilePath,
      simpleCopyFiles
    );
    return null;
  }

}
