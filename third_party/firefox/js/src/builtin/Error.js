/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function ErrorToString() {
  var obj = this;
  if (!IsObject(obj)) {
    ThrowTypeError(JSMSG_INCOMPATIBLE_PROTO, "Error", "toString", "value");
  }

  var name = obj.name;
  name = name === undefined ? "Error" : ToString(name);

  var msg = obj.message;
  msg = msg === undefined ? "" : ToString(msg);

  if (name === "") {
    return msg;
  }

  if (msg === "") {
    return name;
  }

  return name + ": " + msg;
}

function ErrorToStringWithTrailingNewline() {
  return FUN_APPLY(ErrorToString, this, []) + "\n";
}
