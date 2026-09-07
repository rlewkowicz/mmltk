/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function Number_isFinite(num) {
  if (typeof num !== "number") {
    return false;
  }
  return num - num === 0;
}

function Number_isNaN(num) {
  if (typeof num !== "number") {
    return false;
  }
  return num !== num;
}

function Number_isInteger(number) {

  if (typeof number !== "number") {
    return false;
  }

  var integer = std_Math_trunc(number);

  return number - integer === 0;
}

function Number_isSafeInteger(number) {

  if (typeof number !== "number") {
    return false;
  }

  var integer = std_Math_trunc(number);

  if (number - integer !== 0) {
    return false;
  }

  // prettier-ignore
  return -((2 ** 53) - 1) <= integer && integer <= (2 ** 53) - 1;
}

function Global_isNaN(number) {
  return Number_isNaN(TO_NUMBER(number));
}

function Global_isFinite(number) {
  return Number_isFinite(TO_NUMBER(number));
}
