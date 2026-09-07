/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "logConsole", function () {
  return console.createInstance({
    prefix: "BackupService::ArchiveEncryption",
    maxLogLevel: Services.prefs.getBoolPref("browser.backup.log", false)
      ? "Debug"
      : "Warn",
  });
});

ChromeUtils.defineESModuleGetters(lazy, {
  ArchiveUtils: "resource:///modules/backup/ArchiveUtils.sys.mjs",
  OSKeyStore: "resource://gre/modules/OSKeyStore.sys.mjs",
  BackupError: "resource:///modules/backup/BackupError.mjs",
  ERRORS: "chrome://browser/content/backup/backup-constants.mjs",
});

export class ArchiveEncryptionState {
  static #isInternalConstructing = false;

  #state = null;

  static get VERSION() {
    return 1;
  }

  static get GENERATED_RECOVERY_CODE_LENGTH() {
    return 14;
  }

  get publicKey() {
    return this.#state.publicKey;
  }

  get backupAuthKey() {
    return this.#state.backupAuthKey;
  }

  get salt() {
    return this.#state.salt;
  }

  get nonce() {
    return this.#state.nonce;
  }

  get wrappedSecrets() {
    return this.#state.wrappedSecrets;
  }

  constructor() {
    if (!ArchiveEncryptionState.#isInternalConstructing) {
      throw new lazy.BackupError(
        "ArchiveEncryptionState is not constructable.",
        lazy.ERRORS.UNKNOWN
      );
    }
    ArchiveEncryptionState.#isInternalConstructing = false;
  }

  async #enable(recoveryCode = null) {
    lazy.logConsole.debug("Creating new enabled ArchiveEncryptionState");

    lazy.logConsole.debug("Generating an RSA-OEAP keyPair");
    let keyPair = await crypto.subtle.generateKey(
      {
        name: "RSA-OAEP",
        modulusLength: 2048,
        publicExponent: new Uint8Array([1, 0, 1]),
        hash: { name: "SHA-256" },
      },
      true ,
      ["encrypt", "decrypt"]
    );

    if (!recoveryCode) {
      recoveryCode = "";
      const charset =
        "ABCDEFGH#JKLMN@PQRSTUVWXYZabcdefgh=jklmn+pqrstuvwxyz%!23456789";
      let highestMultiple =
        Math.floor((255  - 1) / charset.length) *
        charset.length;

      while (
        recoveryCode.length <
        ArchiveEncryptionState.GENERATED_RECOVERY_CODE_LENGTH
      ) {
        let randomValue = new Uint8Array(1);
        crypto.getRandomValues(randomValue);
        if (randomValue > highestMultiple) {
          continue;
        }
        let randomIndex = randomValue % charset.length;
        recoveryCode += charset[randomIndex];
      }
    }

    lazy.logConsole.debug("Creating salt");
    let textEncoder = new TextEncoder();
    const SALT_SUFFIX = textEncoder.encode(
      "backupkey-v" + ArchiveEncryptionState.VERSION
    );
    let saltPrefix = new Uint8Array(32);
    crypto.getRandomValues(saltPrefix);

    let salt = new Uint8Array(saltPrefix.length + SALT_SUFFIX.length);
    salt.set(saltPrefix);
    salt.set(SALT_SUFFIX, saltPrefix.length);

    let { backupAuthKey, backupEncKey } =
      await lazy.ArchiveUtils.computeBackupKeys(recoveryCode, salt);

    lazy.logConsole.debug("Encrypting secrets with encKey");
    const NONCE_SIZE = 96;
    let nonce = crypto.getRandomValues(new Uint8Array(NONCE_SIZE));

    let secrets = JSON.stringify({
      privateKey: await crypto.subtle.exportKey("jwk", keyPair.privateKey),
      OSKeyStoreSecret: await lazy.OSKeyStore.exportRecoveryPhrase(),
    });
    let secretsBytes = textEncoder.encode(secrets);

    let wrappedSecrets = new Uint8Array(
      await crypto.subtle.encrypt(
        {
          name: "AES-GCM",
          iv: nonce,
        },
        backupEncKey,
        secretsBytes
      )
    );

    this.#state = {
      publicKey: keyPair.publicKey,
      salt,
      backupAuthKey,
      nonce,
      wrappedSecrets,
    };

    return recoveryCode;
  }

  async serialize() {
    let publicKey = await crypto.subtle.exportKey("jwk", this.#state.publicKey);
    let salt = lazy.ArchiveUtils.arrayToBase64(this.#state.salt);
    let backupAuthKey = lazy.ArchiveUtils.arrayToBase64(
      this.#state.backupAuthKey
    );
    let nonce = lazy.ArchiveUtils.arrayToBase64(this.#state.nonce);
    let wrappedSecrets = lazy.ArchiveUtils.arrayToBase64(
      this.#state.wrappedSecrets
    );
    let result = {
      publicKey,
      salt,
      backupAuthKey,
      nonce,
      wrappedSecrets,
      version: ArchiveEncryptionState.VERSION,
    };

    return result;
  }

  async #deserialize(stateData) {
    lazy.logConsole.debug(
      "Deserializing from state with version ",
      stateData.version
    );

    if (stateData.version != ArchiveEncryptionState.VERSION) {
      throw new lazy.BackupError(
        "The ArchiveEncryptionState version is from a newer version.",
        lazy.ERRORS.UNSUPPORTED_BACKUP_VERSION
      );
    }

    let publicKey = await crypto.subtle.importKey(
      "jwk",
      stateData.publicKey,
      { name: "RSA-OAEP", hash: "SHA-256" },
      true ,
      ["encrypt"]
    );
    let backupAuthKey = lazy.ArchiveUtils.stringToArray(
      stateData.backupAuthKey
    );
    let salt = lazy.ArchiveUtils.stringToArray(stateData.salt);
    let nonce = lazy.ArchiveUtils.stringToArray(stateData.nonce);
    let wrappedSecrets = lazy.ArchiveUtils.stringToArray(
      stateData.wrappedSecrets
    );

    this.#state = {
      publicKey,
      backupAuthKey,
      salt,
      nonce,
      wrappedSecrets,
    };
  }


  static async initialize(stateDataOrRecoveryCode) {
    ArchiveEncryptionState.#isInternalConstructing = true;
    let instance = new ArchiveEncryptionState();
    if (typeof stateDataOrRecoveryCode == "object") {
      await instance.#deserialize(stateDataOrRecoveryCode);
      return { instance };
    }
    let recoveryCode = await instance.#enable(stateDataOrRecoveryCode);
    return { instance, recoveryCode };
  }
}
