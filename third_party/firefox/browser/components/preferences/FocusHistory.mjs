/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class FocusHistory {
  #focused = new Map();

  save(historyId) {
    if (!document) {
      return;
    }
    const el = document.activeElement;
    if (!el || el === document.body || el === document.documentElement) {
      this.#focused.delete(historyId);
      return;
    }
    this.#focused.set(historyId, new WeakRef(el));
  }

  restore(historyId) {
    const el =  (
      this.#focused.get(historyId)?.deref()
    );
    if (el?.isConnected) {
      el.focus({ preventScroll: true });
    }
  }
}
