/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { BackupResource } from "resource:///modules/backup/BackupResource.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BackupService: "resource:///modules/backup/BackupService.sys.mjs",
  OSKeyStore: "resource://gre/modules/OSKeyStore.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "nativeOSKeyStore",
  "@mozilla.org/security/oskeystore;1",
  Ci.nsIOSKeyStore
);

export class CredentialsAndSecurityBackupResource extends BackupResource {
  static get key() {
    return "credentials_and_security";
  }

  static get requiresEncryption() {
    return true;
  }

  async backup(
    stagingPath,
    profilePath = PathUtils.profileDir,
    _isEncrypting = false
  ) {
    const simpleCopyFiles = [
      "pkcs11.txt",
      "logins.json",
      "logins-backup.json",
      "autofill-profiles.json",
    ];
    await BackupResource.copyFiles(profilePath, stagingPath, simpleCopyFiles);

    const sqliteDatabases = ["cert9.db", "key4.db", "credentialstate.sqlite"];
    await BackupResource.copySqliteDatabases(
      profilePath,
      stagingPath,
      sqliteDatabases
    );

    return null;
  }

  async recover(_manifestEntry, recoveryPath, destProfilePath) {
    const AUTOFILL_RECORDS_PATH = PathUtils.join(
      recoveryPath,
      "autofill-profiles.json"
    );

    const files = [
      "pkcs11.txt",
      "logins.json",
      "logins-backup.json",
      "cert9.db",
      "key4.db",
      "credentialstate.sqlite",
    ];

    if (await IOUtils.exists(AUTOFILL_RECORDS_PATH)) {
      await this.encryptAutofillData(AUTOFILL_RECORDS_PATH);
      files.push("autofill-profiles.json");
    }

    await BackupResource.copyFiles(recoveryPath, destProfilePath, files);

    return null;
  }

  async encryptAutofillData(AUTOFILL_RECORDS_PATH) {
    let autofillRecords = await IOUtils.readJSON(AUTOFILL_RECORDS_PATH);

    for (let creditCard of autofillRecords.creditCards) {
      let oldEncryptedCard = creditCard["cc-number-encrypted"];
      if (oldEncryptedCard) {
        let plaintextCard;
        if (
          await lazy.nativeOSKeyStore.asyncSecretAvailable(
            lazy.BackupService.RECOVERY_OSKEYSTORE_LABEL
          )
        ) {
          let plaintextCardBytes =
            await lazy.nativeOSKeyStore.asyncDecryptBytes(
              lazy.BackupService.RECOVERY_OSKEYSTORE_LABEL,
              oldEncryptedCard
            );
          plaintextCard = String.fromCharCode.apply(String, plaintextCardBytes);
        } else {
          plaintextCard = await lazy.OSKeyStore.decrypt(
            oldEncryptedCard,
            "backup_cc"
          );
        }

        let newEncryptedCard = await lazy.OSKeyStore.encrypt(plaintextCard);
        creditCard["cc-number-encrypted"] = newEncryptedCard;
      }
    }

    await IOUtils.writeJSON(AUTOFILL_RECORDS_PATH, autofillRecords);
  }

}
