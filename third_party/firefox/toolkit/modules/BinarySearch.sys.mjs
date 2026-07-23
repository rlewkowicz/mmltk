/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

export var BinarySearch = Object.freeze({
  indexOf(comparator, array, target) {
    let [found, idx] = this.search(comparator, array, target);
    return found ? idx : -1;
  },

  insertionIndexOf(comparator, array, target) {
    return this.search(comparator, array, target)[1];
  },

  search(comparator, array, target) {
    let low = 0;
    let high = array.length - 1;
    while (low <= high) {
      let mid = (low + high) >> 1;
      let cmp = comparator(target, array[mid]);
      if (cmp == 0) {
        return [true, mid];
      }
      if (cmp < 0) {
        high = mid - 1;
      } else {
        low = mid + 1;
      }
    }
    return [false, low];
  },
});
