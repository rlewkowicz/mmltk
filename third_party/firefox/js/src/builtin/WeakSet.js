/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function WeakSetConstructorInit(iterable) {
  var set = this;

  var adder = set.add;

  if (!IsCallable(adder)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, typeof adder);
  }

  for (var nextValue of allowContentIter(iterable)) {
    callContentFunction(adder, set, nextValue);
  }
}
