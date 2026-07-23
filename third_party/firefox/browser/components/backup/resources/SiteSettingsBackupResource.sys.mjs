/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { BackupResource } from "resource:///modules/backup/BackupResource.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "isSanitizeOnShutdownEnabled",
  "privacy.sanitize.sanitizeOnShutdown",
  false
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "isSiteSettingsClearedOnShutdown",
  "privacy.clearOnShutdown_v2.siteSettings",
  false
);

export class SiteSettingsBackupResource extends BackupResource {
  static get key() {
    return "site_settings";
  }

  static get requiresEncryption() {
    return false;
  }

  static get priority() {
    return 1;
  }

  static get canBackupResource() {
    if (!lazy.isSanitizeOnShutdownEnabled) {
      return true;
    }

    return !lazy.isSiteSettingsClearedOnShutdown;
  }

  async backup(
    stagingPath,
    profilePath = PathUtils.profileDir,
    _isEncrypting = false
  ) {
    const sqliteDatabases = ["permissions.sqlite", "content-prefs.sqlite"];
    await BackupResource.copySqliteDatabases(
      profilePath,
      stagingPath,
      sqliteDatabases
    );
    return null;
  }

  async recover(_manifestEntry, recoveryPath, destProfilePath) {
    const simpleCopyFiles = ["permissions.sqlite", "content-prefs.sqlite"];
    await BackupResource.copyFiles(
      recoveryPath,
      destProfilePath,
      simpleCopyFiles
    );

    return null;
  }

}
