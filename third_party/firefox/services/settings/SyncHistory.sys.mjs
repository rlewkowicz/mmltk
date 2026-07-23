/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  KeyValueService: "moz-src:///toolkit/components/kvstore/kvstore.sys.mjs",
});

export class SyncHistory {
  #store;

  constructor(source, { size } = { size: 100 }) {
    this.source = source;
    this.size = size;
  }

  async store(timestamp, status, infos = {}) {
    const rkv = await this.#init();
    if (!Number.isInteger(timestamp)) {
      throw new Error(`Invalid timestamp value ${timestamp}`);
    }
    const key = `v1-${this.source}\t${timestamp}`;
    const value = { timestamp, status, infos };
    await rkv.put(key, JSON.stringify(value));
    const allEntries = await this.list();
    for (let i = this.size; i < allEntries.length; i++) {
      let { timestamp: entryTimestamp } = allEntries[i];
      await rkv.delete(`v1-${this.source}\t${entryTimestamp}`);
    }
  }

  async list() {
    const rkv = await this.#init();
    const entries = [];
    for (const { value } of await rkv.enumerate(
      `v1-${this.source}`,
      `v1-${this.source}\n`
    )) {
      try {
        const stored = JSON.parse(value);
        entries.push({ ...stored, datetime: new Date(stored.timestamp) });
      } catch (e) {
        console.error(e);
      }
    }
    entries.sort((a, b) => (a.timestamp > b.timestamp ? -1 : 1));
    return entries;
  }

  async last() {
    return (await this.list())[0];
  }

  async clear() {
    const rkv = await this.#init();
    await rkv.clear();
  }

  async #init() {
    if (!this.#store) {
      const dir = PathUtils.join(PathUtils.profileDir, "settings");
      await IOUtils.makeDirectory(dir);
      this.#store = await lazy.KeyValueService.getOrCreate(dir, "synchistory");
    }
    return this.#store;
  }
}
