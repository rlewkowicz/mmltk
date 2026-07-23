/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class ScrollOffsets {
  #nextHistoryEntryId = Math.floor(Math.random() * 2 ** 32);

  newHistoryEntryId() {
    return ++this.#nextHistoryEntryId;
  }

  #key = null;

  #offsets = new Map();

  #scrollContainer;

  canRestore = true;

  constructor(scrollContainer = document.documentElement) {
    this.#scrollContainer = scrollContainer;
  }

  setView(historyEntryId) {
    this.#key = historyEntryId;
    this.canRestore = true;
  }

  getPosition() {
    if (!this.canRestore) {
      return { top: 0, left: 0 };
    }
    return {
      top: this.#scrollContainer.scrollTop,
      left: this.#scrollContainer.scrollLeft,
    };
  }

  save() {
    if (this.#key) {
      this.#offsets.set(this.#key, this.getPosition());
    }
  }

  restore() {
    let saved = this.#key ? this.#offsets.get(this.#key) : null;
    this.#scrollContainer.scrollTo({
      top: saved?.top ?? 0,
      left: saved?.left ?? 0,
      behavior: "auto",
    });
  }
}
