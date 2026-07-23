/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


export function setText(doc, id, value) {
  let element = doc.getElementById(id);
  if (!element) {
    return;
  }
  if (element.hasChildNodes()) {
    element.firstChild.remove();
  }
  element.appendChild(doc.createTextNode(value));
}

function getPKCS7Array(certArray) {
  let certdb = Cc["@mozilla.org/security/x509certdb;1"].getService(
    Ci.nsIX509CertDB
  );
  let pkcs7String = certdb.asPKCS7Blob(certArray);
  let pkcs7Array = new Uint8Array(pkcs7String.length);
  for (let i = 0; i < pkcs7Array.length; i++) {
    pkcs7Array[i] = pkcs7String.charCodeAt(i);
  }
  return pkcs7Array;
}

export function getPEMString(cert) {
  var derb64 = cert.getBase64DERString();
  var wrapped = derb64.replace(/(\S{64}(?!$))/g, "$1\r\n");
  return (
    "-----BEGIN CERTIFICATE-----\r\n" +
    wrapped +
    "\r\n-----END CERTIFICATE-----\r\n"
  );
}

export function alertPromptService(window, title, message) {
  // eslint-disable-next-line mozilla/use-services
  var ps = Cc["@mozilla.org/prompter;1"].getService(Ci.nsIPromptService);
  ps.alert(window, title, message);
}

const DEFAULT_CERT_EXTENSION = "crt";

function certToFilename(cert) {
  let filename = cert.displayName;

  filename = filename
    .replace(/\s/g, "")
    .replace(/\./g, "_")
    .replace(/\\/g, "")
    .replace(/\//g, "");

  return `${filename}.${DEFAULT_CERT_EXTENSION}`;
}

export async function exportToFile(parent, document, cert) {
  if (!cert) {
    return;
  }

  let results = await asyncDetermineUsages(cert);
  let chain = getBestChain(results);
  if (!chain) {
    chain = [cert];
  }

  let formats = {
    base64: "*.crt; *.pem",
    "base64-chain": "*.crt; *.pem",
    der: "*.der",
    pkcs7: "*.p7c",
    "pkcs7-chain": "*.p7c",
  };
  let [saveCertAs, ...formatLabels] = await document.l10n.formatValues(
    ["save-cert-as", ...Object.keys(formats).map(f => "cert-format-" + f)].map(
      id => ({ id })
    )
  );

  var fp = Cc["@mozilla.org/filepicker;1"].createInstance(Ci.nsIFilePicker);
  fp.init(parent.browsingContext, saveCertAs, Ci.nsIFilePicker.modeSave);
  fp.defaultString = certToFilename(cert);
  fp.defaultExtension = DEFAULT_CERT_EXTENSION;
  for (let format of Object.values(formats)) {
    fp.appendFilter(formatLabels.shift(), format);
  }
  fp.appendFilters(Ci.nsIFilePicker.filterAll);
  let filePickerResult = await new Promise(resolve => {
    fp.open(resolve);
  });

  if (
    filePickerResult != Ci.nsIFilePicker.returnOK &&
    filePickerResult != Ci.nsIFilePicker.returnReplace
  ) {
    return;
  }

  var content = "";
  switch (fp.filterIndex) {
    case 1:
      content = getPEMString(cert);
      for (let i = 1; i < chain.length; i++) {
        content += getPEMString(chain[i]);
      }
      break;
    case 2:
      content = Uint8Array.from(cert.getRawDER());
      break;
    case 3:
      content = getPKCS7Array([cert]);
      break;
    case 4:
      content = getPKCS7Array(chain);
      break;
    case 0:
    default:
      content = getPEMString(cert);
      break;
  }

  if (typeof content === "string") {
    content = new TextEncoder().encode(content);
  }

  try {
    await IOUtils.write(fp.file.path, content);
  } catch (ex) {
    let title = await document.l10n.formatValue("write-file-failure");
    alertPromptService(parent, title, ex.toString());
  }
}

const PRErrorCodeSuccess = 0;

const verifyUsages = new Map([
  ["verifyUsageTLSClient", Ci.nsIX509CertDB.verifyUsageTLSClient],
  ["verifyUsageTLSServer", Ci.nsIX509CertDB.verifyUsageTLSServer],
  ["verifyUsageTLSServerCA", Ci.nsIX509CertDB.verifyUsageTLSServerCA],
  ["verifyUsageEmailSigner", Ci.nsIX509CertDB.verifyUsageEmailSigner],
  ["verifyUsageEmailRecipient", Ci.nsIX509CertDB.verifyUsageEmailRecipient],
]);

export function asyncDetermineUsages(cert) {
  let promises = [];
  let now = Date.now() / 1000;
  let certdb = Cc["@mozilla.org/security/x509certdb;1"].getService(
    Ci.nsIX509CertDB
  );
  verifyUsages.keys().forEach(usageString => {
    promises.push(
      new Promise(resolve => {
        let usage = verifyUsages.get(usageString);
        certdb.asyncVerifyCertAtTime(
          cert,
          usage,
          0,
          null,
          now,
          [],
          (aPRErrorCode, aVerifiedChain) => {
            resolve({
              usageString,
              errorCode: aPRErrorCode,
              chain: aVerifiedChain,
            });
          }
        );
      })
    );
  });
  return Promise.all(promises);
}

export function getBestChain(results) {
  let usages = [
    Ci.nsIX509CertDB.verifyUsageTLSServer,
    Ci.nsIX509CertDB.verifyUsageTLSClient,
    Ci.nsIX509CertDB.verifyUsageEmailSigner,
    Ci.nsIX509CertDB.verifyUsageEmailRecipient,
    Ci.nsIX509CertDB.verifyUsageTLSServerCA,
  ];
  for (let usage of usages) {
    let chain = getChainForUsage(results, usage);
    if (chain) {
      return chain;
    }
  }
  return null;
}

function getChainForUsage(results, usage) {
  for (let result of results) {
    if (
      verifyUsages.get(result.usageString) == usage &&
      result.errorCode == PRErrorCodeSuccess
    ) {
      return result.chain;
    }
  }
  return null;
}

export async function checkCertHelper(uri, grabber) {
  let req = new XMLHttpRequest();
  req.open("GET", uri.prePath);
  req.onerror = grabber.bind(null, req);
  req.onload = grabber.bind(null, req);
  req.send(null);
}
