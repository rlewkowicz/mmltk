/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { setText } = ChromeUtils.importESModule(
  "resource://gre/modules/psm/pippki.sys.mjs"
);



var gCert;

function onLoad() {
  gCert = window.arguments[0].QueryInterface(Ci.nsIX509Cert);

  document.addEventListener("dialogaccept", onDialogAccept);
  document.addEventListener("dialogcancel", onDialogCancel);

  let bundle = document.getElementById("pippki_bundle");
  let caName = gCert.commonName;
  if (!caName.length) {
    caName = bundle.getString("unnamedCA");
  }

  setText(
    document,
    "trustHeader",
    bundle.getFormattedString("newCAMessage1", [caName])
  );

}

function onDialogAccept() {
  let checkSSL = document.getElementById("trustSSL");
  let checkEmail = document.getElementById("trustEmail");

  let retVals = window.arguments[1].QueryInterface(Ci.nsIWritablePropertyBag2);
  retVals.setPropertyAsBool("importConfirmed", true);
  retVals.setPropertyAsBool("trustForSSL", checkSSL.checked);
  retVals.setPropertyAsBool("trustForEmail", checkEmail.checked);
}

function onDialogCancel() {
  let retVals = window.arguments[1].QueryInterface(Ci.nsIWritablePropertyBag2);
  retVals.setPropertyAsBool("importConfirmed", false);
}

window.addEventListener("load", () => onLoad());
