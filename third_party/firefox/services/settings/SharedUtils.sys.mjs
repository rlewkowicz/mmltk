/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


export var SharedUtils = {
  async checkContentHash(buffer, size, hash) {
    const bytes = new Uint8Array(buffer);
    if (bytes.length !== size) {
      return false;
    }
    const hashBuffer = await crypto.subtle.digest("SHA-256", bytes);
    const hashBytes = new Uint8Array(hashBuffer);
    const toHex = b => b.toString(16).padStart(2, "0");
    const hashStr = Array.from(hashBytes, toHex).join("");
    return hashStr == hash;
  },

  async loadJSONDump(bucket, collection) {
    const jsonBucket = bucket.replace("-preview", "");
    const fileURI = `resource://app/defaults/settings/${jsonBucket}/${collection}.json`;
    let response;
    try {
      response = await fetch(fileURI);
    } catch (e) {
      return { data: null, timestamp: null };
    }
    return response.json();
  },
};
