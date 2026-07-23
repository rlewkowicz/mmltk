/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class BackupError extends Error {
  name = "BackupError";

  constructor(message, cause) {
    super(message, { cause });
  }

  toMsg() {
    return {
      exn: BackupError.name,
      message: this.message,
      cause: this.cause,
      stack: this.stack,
    };
  }

  static fromMsg(serialized) {
    let error = new BackupError(serialized.message, serialized.cause);
    error.stack = serialized.stack;
    return error;
  }
}
