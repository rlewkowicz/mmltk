/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


export const ArchiveUtils = {
  arrayToBase64(anArray) {
    let result = "";
    let bytes = new Uint8Array(anArray);
    for (let i = 0; i < bytes.length; i++) {
      result += String.fromCharCode(bytes[i]);
    }
    return btoa(result);
  },

  stringToArray(base64Str) {
    let binaryStr = atob(base64Str);
    let len = binaryStr.length;
    let bytes = new Uint8Array(len);
    for (let i = 0; i < len; i++) {
      bytes[i] = binaryStr.charCodeAt(i);
    }
    return bytes;
  },

  get SCHEMA_VERSION() {
    return 2;
  },

  get ARCHIVE_FILE_VERSION() {
    return 1;
  },

  get INLINE_MIME_START_MARKER() {
    return "<!-- Begin inline MIME --";
  },

  get INLINE_MIME_END_MARKER() {
    return "---- End inline MIME -->";
  },

  get ARCHIVE_CHUNK_MAX_BYTES_SIZE() {
    return 1048576; 
  },

  get ARCHIVE_MAX_BYTES_SIZE() {
    return 34359738368; 
  },

  get TAG_LENGTH() {
    return 128;
  },

  get TAG_LENGTH_BYTES() {
    return this.TAG_LENGTH / 8;
  },


  async computeBackupKeys(recoveryCode, salt) {
    let textEncoder = new TextEncoder();
    let recoveryCodeBytes = textEncoder.encode(recoveryCode);

    let keyMaterial = await crypto.subtle.importKey(
      "raw",
      recoveryCodeBytes,
      "PBKDF2",
      false ,
      ["deriveBits"]
    );

    const ITERATIONS = 600_000;

    let backupKeyBits = await crypto.subtle.deriveBits(
      {
        name: "PBKDF2",
        salt,
        iterations: ITERATIONS,
        hash: "SHA-256",
      },
      keyMaterial,
      256
    );

    let backupKeyHKDF = await crypto.subtle.importKey(
      "raw",
      backupKeyBits,
      {
        name: "HKDF",
        hash: "SHA-256",
      },
      false ,
      ["deriveKey", "deriveBits"]
    );

    let backupAuthKey = new Uint8Array(
      await crypto.subtle.deriveBits(
        {
          name: "HKDF",
          salt: new Uint8Array(0), 
          info: textEncoder.encode("backupkey-auth"),
          hash: "SHA-256",
        },
        backupKeyHKDF,
        256
      )
    );

    let backupEncKey = await crypto.subtle.deriveKey(
      {
        name: "HKDF",
        salt: new Uint8Array(0), 
        info: textEncoder.encode("backupkey-enc-key"),
        hash: "SHA-256",
      },
      backupKeyHKDF,
      { name: "AES-GCM", length: 256 },
      false ,
      ["encrypt", "decrypt", "wrapKey"]
    );

    return { backupAuthKey, backupEncKey };
  },


  async computeEncryptionKeys(archiveKeyMaterial, backupAuthKey) {
    let archiveKey = await crypto.subtle.importKey(
      "raw",
      archiveKeyMaterial,
      { name: "HKDF" },
      false, 
      ["deriveKey", "deriveBits"]
    );

    let textEncoder = new TextEncoder();
    let archiveEncKey = await crypto.subtle.deriveKey(
      {
        name: "HKDF",
        salt: backupAuthKey,
        info: textEncoder.encode("archive-enc-key"),
        hash: "SHA-256",
      },
      archiveKey,
      { name: "AES-GCM", length: 256 },
      true ,
      ["decrypt", "encrypt"]
    );

    let authKey = await crypto.subtle.deriveKey(
      {
        name: "HKDF",
        salt: backupAuthKey,
        info: textEncoder.encode("archive-auth-key"),
        hash: "SHA-256",
      },
      archiveKey,
      { name: "HMAC", hash: "SHA-256", length: 256 },
      false ,
      ["sign", "verify"]
    );

    return { archiveEncKey, authKey };
  },

  countReplacementCharacters(str) {
    let count = 0;
    let lengthToCheck = Math.min(4, str.length);

    for (let index = 0; index < lengthToCheck; index += 1) {
      if (str[index] == "\uFFFD") {
        count += 1;
      }
    }

    return count;
  },
};
