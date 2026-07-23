/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  Sqlite: "resource://gre/modules/Sqlite.sys.mjs",
  BackupError: "resource:///modules/backup/BackupError.mjs",
  ERRORS: "chrome://browser/content/backup/backup-constants.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "isBrowsingHistoryEnabled",
  "places.history.enabled",
  true
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "isSanitizeOnShutdownEnabled",
  "privacy.sanitize.sanitizeOnShutdown",
  false
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "isHistoryClearedOnShutdown",
  "privacy.clearOnShutdown_v2.browsingHistoryAndDownloads",
  false
);

export const BYTES_IN_KB = 1000;

export function bytesToFuzzyKilobytes(bytes) {
  let sizeInKb = Math.ceil(bytes / BYTES_IN_KB);
  let nearestTenKb = Math.round(sizeInKb / 10) * 10;
  return Math.max(nearestTenKb, 1);
}

export class BackupResource {
  static get key() {
    throw new lazy.BackupError(
      "BackupResource::key needs to be overridden.",
      lazy.ERRORS.INTERNAL_ERROR
    );
  }

  static get requiresEncryption() {
    throw new lazy.BackupError(
      "BackupResource::requiresEncryption needs to be overridden.",
      lazy.ERRORS.INTERNAL_ERROR
    );
  }

  static get priority() {
    return 0;
  }

  static async getFileSize(filePath) {
    if (!(await IOUtils.exists(filePath))) {
      return null;
    }

    let { size } = await IOUtils.stat(filePath);

    if (size < 0) {
      return null;
    }

    let nearestTenKb = bytesToFuzzyKilobytes(size);

    return nearestTenKb;
  }

  static async getDirectorySize(
    directoryPath,
    { shouldExclude = () => false } = {}
  ) {
    if (!(await IOUtils.exists(directoryPath))) {
      return null;
    }

    let { type } = await IOUtils.stat(directoryPath);

    if (type != "directory") {
      return null;
    }

    let children = await IOUtils.getChildren(directoryPath, {
      ignoreAbsent: true,
    });

    let size = 0;
    for (const childFilePath of children) {
      let { size: childSize, type: childType } =
        await IOUtils.stat(childFilePath);

      if (shouldExclude(childFilePath, childType, directoryPath)) {
        continue;
      }

      if (childSize >= 0) {
        let nearestTenKb = bytesToFuzzyKilobytes(childSize);

        size += nearestTenKb;
      }

      if (childType == "directory") {
        let childDirectorySize = await this.getDirectorySize(childFilePath, {
          shouldExclude,
        });
        if (Number.isInteger(childDirectorySize)) {
          size += childDirectorySize;
        }
      }
    }

    return size;
  }

  static async copySqliteDatabases(sourcePath, destPath, sqliteDatabases) {
    for (let fileName of sqliteDatabases) {
      let sourceFilePath = PathUtils.join(sourcePath, fileName);

      if (!(await IOUtils.exists(sourceFilePath))) {
        continue;
      }

      let destFilePath = PathUtils.join(destPath, fileName);
      let connection;

      try {
        connection = await lazy.Sqlite.openConnection({
          path: sourceFilePath,
          readOnly: true,
        });

        await connection.backup(
          destFilePath,
          BackupResource.SQLITE_PAGES_PER_STEP,
          BackupResource.SQLITE_STEP_DELAY_MS
        );
      } finally {
        await connection?.close();
      }
    }
  }

  static async copyFiles(sourcePath, destPath, fileNames) {
    for (let fileName of fileNames) {
      let sourceFilePath = PathUtils.join(sourcePath, fileName);
      let destFilePath = PathUtils.join(destPath, fileName);
      if (await IOUtils.exists(sourceFilePath)) {
        await IOUtils.copy(sourceFilePath, destFilePath, { recursive: true });
      }
    }
  }


  static get canBackupResource() {
    return true;
  }

  static get backingUpPlaces() {
    if (
      lazy.PrivateBrowsingUtils.permanentPrivateBrowsing ||
      !lazy.isBrowsingHistoryEnabled
    ) {
      return false;
    }

    if (!lazy.isSanitizeOnShutdownEnabled) {
      return true;
    }

    if (lazy.isHistoryClearedOnShutdown) {
      return false;
    }

    return true;
  }

  constructor() {}

  // eslint-disable-next-line no-unused-vars
  async backup(stagingPath, profilePath = null, isEncrypting = false) {
    throw new lazy.BackupError(
      "BackupResource::backup must be overridden",
      lazy.ERRORS.INTERNAL_ERROR
    );
  }

  // eslint-disable-next-line no-unused-vars
  async recover(manifestEntry, recoveryPath, destProfilePath) {
    throw new lazy.BackupError(
      "BackupResource::recover must be overridden",
      lazy.ERRORS.INTERNAL_ERROR
    );
  }

  // eslint-disable-next-line no-unused-vars
  async postRecovery(postRecoveryEntry) {
  }
}

XPCOMUtils.defineLazyPreferenceGetter(
  BackupResource,
  "SQLITE_PAGES_PER_STEP",
  "browser.backup.sqlite.pages_per_step",
  5
);

XPCOMUtils.defineLazyPreferenceGetter(
  BackupResource,
  "SQLITE_STEP_DELAY_MS",
  "browser.backup.sqlite.step_delay_ms",
  250
);
