/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { PromiseWorker } from "resource://gre/modules/workers/PromiseWorker.mjs";

/* eslint-disable mozilla/reject-import-system-module-from-non-system */
import { ArchiveUtils } from "resource:///modules/backup/ArchiveUtils.sys.mjs";
import { ArchiveEncryptor } from "resource:///modules/backup/ArchiveEncryption.sys.mjs";
import { BackupError } from "resource:///modules/backup/BackupError.mjs";
import { ERRORS } from "chrome://browser/content/backup/backup-constants.mjs";

class ArchiveWorker {
  #worker = null;

  constructor() {
    this.#connectToPromiseWorker();
  }

  #generateBoundary() {
    return (
      "----=_Part_" +
      new Date().getTime() +
      "_" +
      Math.random().toString(36).slice(2, 12) +
      "_" +
      Math.random().toString(36).slice(2, 12)
    );
  }

  #computeChunkBase64Bytes(bytes, encrypting) {
    if (encrypting) {
      bytes += ArchiveUtils.TAG_LENGTH_BYTES;
    }

    return 4 * Math.ceil(bytes / 3) + 1;
  }


  async constructArchive({
    archivePath,
    markup,
    backupMetadata,
    compressedBackupSnapshotPath,
    encryptionArgs,
    chunkSize,
  }) {
    let encryptor = null;
    if (encryptionArgs) {
      encryptor = await ArchiveEncryptor.initialize(
        encryptionArgs.publicKey,
        encryptionArgs.backupAuthKey
      );
    }

    let boundary = this.#generateBoundary();

    let jsonBlock;
    if (encryptor) {
      jsonBlock = await encryptor.confirm(
        backupMetadata,
        encryptionArgs.wrappedSecrets,
        encryptionArgs.salt,
        encryptionArgs.nonce
      );
    } else {
      jsonBlock = {
        version: ArchiveUtils.SCHEMA_VERSION,
        encConfig: null,
        meta: backupMetadata,
      };
    }

    let serializedJsonBlock = JSON.stringify(jsonBlock);
    let textEncoder = new TextEncoder();
    let jsonBlockLength = textEncoder.encode(serializedJsonBlock).length;

    await IOUtils.writeUTF8(archivePath, markup);
    await IOUtils.writeUTF8(
      archivePath,
      `
${ArchiveUtils.INLINE_MIME_START_MARKER}
Content-Type: multipart/mixed; boundary="${boundary}"

--${boundary}
Content-Type: application/json; charset=utf-8
Content-Disposition: attachment; filename="archive.json"
Content-Length: ${jsonBlockLength}

${JSON.stringify(jsonBlock)}
`,
      { mode: "append" }
    );

    let compressedBackupSnapshotFile = IOUtils.openFileForSyncReading(
      compressedBackupSnapshotPath
    );
    let totalBytesToRead = compressedBackupSnapshotFile.size;

    let totalNewlines = Math.ceil(totalBytesToRead / chunkSize);

    let fullSizeChunks = totalNewlines - 1;
    let fullSizeChunkBase64Bytes = this.#computeChunkBase64Bytes(
      chunkSize,
      !!encryptor
    );
    let totalBase64Bytes = fullSizeChunks * fullSizeChunkBase64Bytes;

    let leftoverChunkBytes = totalBytesToRead % chunkSize;
    if (leftoverChunkBytes) {
      totalBase64Bytes += this.#computeChunkBase64Bytes(
        leftoverChunkBytes,
        !!encryptor
      );
    } else {
      totalBase64Bytes += fullSizeChunkBase64Bytes;
    }

    await IOUtils.writeUTF8(
      archivePath,
      `--${boundary}
Content-Type: application/octet-stream
Content-Disposition: attachment; filename="archive.zip"
Content-Transfer-Encoding: base64
Content-Length: ${totalBase64Bytes}

`,
      { mode: "append" }
    );

    let currentIndex = 0;
    while (currentIndex < totalBytesToRead) {
      let bytesToRead = Math.min(chunkSize, totalBytesToRead - currentIndex);
      if (bytesToRead <= 0) {
        throw new BackupError(
          "Failed to calculate the right number of bytes to read.",
          ERRORS.FILE_SYSTEM_ERROR
        );
      }

      let buffer = new Uint8Array(bytesToRead);
      compressedBackupSnapshotFile.readBytesInto(buffer, currentIndex);

      let bytesToWrite;

      if (encryptor) {
        let isLastChunk = bytesToRead < chunkSize;
        bytesToWrite = await encryptor.encrypt(buffer, isLastChunk);
      } else {
        bytesToWrite = buffer;
      }

      await IOUtils.writeUTF8(
        archivePath,
        ArchiveUtils.arrayToBase64(bytesToWrite) + "\n",
        {
          mode: "append",
        }
      );

      currentIndex += bytesToRead;
    }

    await IOUtils.writeUTF8(
      archivePath,
      `
--${boundary}
${ArchiveUtils.INLINE_MIME_END_MARKER}
`,
      { mode: "append" }
    );

    compressedBackupSnapshotFile.close();

    return true;
  }


  parseArchiveHeader(archivePath) {
    let syncReadFile = IOUtils.openFileForSyncReading(archivePath);
    let totalBytes = syncReadFile.size;

    const MAX_BYTES_TO_READ = 256;
    let headerBytesToRead = Math.min(
      MAX_BYTES_TO_READ,
      totalBytes - MAX_BYTES_TO_READ
    );
    let headerBuffer = new Uint8Array(headerBytesToRead);
    syncReadFile.readBytesInto(headerBuffer, 0);

    let textDecoder = new TextDecoder();
    let decodedHeader = textDecoder.decode(headerBuffer);
    const EXPECTED_HEADER =
      /^<!DOCTYPE html>[\r\n]+<!-- Version: (\d+) -->[\r\n]+/;
    let headerMatches = decodedHeader.match(EXPECTED_HEADER);
    if (!headerMatches) {
      throw new BackupError("Corrupt archive header", ERRORS.CORRUPTED_ARCHIVE);
    }

    let version = parseInt(headerMatches[1], 10);
    if (version != ArchiveUtils.ARCHIVE_FILE_VERSION) {
      throw new BackupError(
        "Unsupported archive version: " + version,
        ERRORS.UNSUPPORTED_BACKUP_VERSION
      );
    }

    let currentIndex = headerBuffer.byteLength;

    let startByteOffset = 0;
    let oldBuffer = headerBuffer;
    let priorIndex = 0;
    let contentType = null;
    const EXPECTED_MARKER = new RegExp(
      `${ArchiveUtils.INLINE_MIME_START_MARKER}\nContent-Type: (.+)\n\n`
    );

    let textEncoder = new TextEncoder();
    while (currentIndex < totalBytes) {
      let bytesToRead = Math.min(MAX_BYTES_TO_READ, totalBytes - currentIndex);

      if (bytesToRead <= 0) {
        throw new BackupError(
          "Failed to calculate the proper number of bytes to read: " +
            bytesToRead,
          ERRORS.UNKNOWN
        );
      }

      let buffer = new Uint8Array(bytesToRead);
      syncReadFile.readBytesInto(buffer, currentIndex);

      let combinedBuffer = new Uint8Array(
        oldBuffer.byteLength + buffer.byteLength
      );
      combinedBuffer.set(oldBuffer, 0);
      combinedBuffer.set(buffer, oldBuffer.byteLength);

      let decodedString = textDecoder.decode(combinedBuffer);
      let markerMatches = decodedString.match(EXPECTED_MARKER);

      if (markerMatches) {

        let match = markerMatches[0];
        let matchBytes = textEncoder.encode(match).byteLength;
        let matchIndex = decodedString.indexOf(match);

        let numberOfUndecodedCharacters =
          ArchiveUtils.countReplacementCharacters(decodedString);
        let substringUpToMatch = decodedString.slice(
          numberOfUndecodedCharacters,
          matchIndex
        );
        let substringUpToMatchBytes =
          textEncoder.encode(substringUpToMatch).byteLength;

        startByteOffset = priorIndex + substringUpToMatchBytes + matchBytes;
        contentType = markerMatches[1];
        break;
      }

      priorIndex = currentIndex;
      currentIndex += bytesToRead;
      oldBuffer = buffer;
    }

    syncReadFile.close();

    if (!contentType) {
      throw new BackupError(
        "Failed to find embedded data in archive",
        ERRORS.CORRUPTED_ARCHIVE
      );
    }

    return { startByteOffset, contentType };
  }

  #connectToPromiseWorker() {
    this.#worker = new PromiseWorker.AbstractWorker();
    this.#worker.dispatch = (method, args = []) => {
      if (!this[method]) {
        throw new BackupError(
          "Method does not exist: " + method,
          ERRORS.INTERNAL_ERROR
        );
      }
      return this[method](...args);
    };
    this.#worker.close = () => self.close();
    this.#worker.postMessage = (message, ...transfers) => {
      self.postMessage(message, ...transfers);
    };

    self.callMainThread = this.#worker.callMainThread.bind(this.#worker);
    self.addEventListener("message", msg => this.#worker.handleMessage(msg));
    self.addEventListener("unhandledrejection", function (error) {
      throw error.reason;
    });
  }
}

new ArchiveWorker();
