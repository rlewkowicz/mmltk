/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

XPCOMUtils.defineLazyServiceGetters(lazy, {
  CertDB: ["@mozilla.org/security/x509certdb;1", Ci.nsIX509CertDB],
});

function arrayToString(a) {
  let s = "";
  for (let b of a) {
    s += String.fromCharCode(b);
  }
  return s;
}

function stringToArrayBuffer(str) {
  let bytes = new Uint8Array(str.length);
  for (let i = 0; i < str.length; i++) {
    bytes[i] = str.charCodeAt(i);
  }
  return bytes;
}

export var QWACs = {
  fromBase64URLEncoding(base64URLEncoded) {
    return atob(base64URLEncoded.replaceAll("-", "+").replaceAll("_", "/"));
  },

  toBase64URLEncoding(str) {
    return btoa(str)
      .replaceAll("+", "-")
      .replaceAll("/", "_")
      .replaceAll("=", "");
  },

  validateTLSCertificateBindingHeader(header) {
    const allowedHeaderKeys = new Set([
      "alg",
      "kid",
      "cty",
      "x5t#S256",
      "x5c",
      "iat",
      "exp",
      "sigD",
    ]);
    let headerKeys = new Set(Object.keys(header));
    if (!headerKeys.isSubsetOf(allowedHeaderKeys)) {
      console.error("header contains invalid parameter");
      return false;
    }

    if (!("alg" in header)) {
      console.error("header missing 'alg' field");
      return false;
    }
    let algorithm;
    switch (header.alg) {
      case "RS256":
        algorithm = { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" };
        break;
      case "PS256":
        algorithm = { name: "RSA-PSS", saltLength: 32, hash: "SHA-256" };
        break;
      case "ES256":
        algorithm = { name: "ECDSA", namedCurve: "P-256", hash: "SHA-256" };
        break;
      default:
        console.error("unsupported alg:", header.alg);
        return false;
    }


    if (!("cty" in header)) {
      console.error("header missing field 'cty'");
      return false;
    }
    if (header.cty != "TLS-Certificate-Binding-v1") {
      console.error("invalid value for cty:", header.cty);
      return false;
    }

    let x5tS256;
    if ("x5t#S256" in header) {
      x5tS256 = header["x5t#S256"];
    }

    if (!("x5c" in header)) {
      console.error("header missing field 'x5c'");
      return false;
    }
    let certificates = [];
    for (let base64 of header.x5c) {
      try {
        certificates.push(lazy.CertDB.constructX509FromBase64(base64));
      } catch (e) {
        console.error("couldn't decode certificate");
        return false;
      }
    }
    if (certificates.length < 1) {
      console.error("header must specify certificate chain");
      return false;
    }
    if (x5tS256) {
      let signingCertificateHashHex = certificates[0].sha256Fingerprint;
      let signingCertificateHashBytes = signingCertificateHashHex
        .split(":")
        .map(hexStr => parseInt(hexStr, 16));
      if (
        x5tS256 !=
        QWACs.toBase64URLEncoding(arrayToString(signingCertificateHashBytes))
      ) {
        console.error("x5t#S256 does not match signing certificate");
        return false;
      }
    }


    if ("exp" in header) {
      let expirationSeconds = parseInt(header.exp);
      if (isNaN(expirationSeconds)) {
        console.error("invalid expiration time");
        return false;
      }
      let expiration = new Date(expirationSeconds * 1000);
      if (expiration < new Date()) {
        console.error("header has expired");
        return false;
      }
    }

    if (!("sigD" in header)) {
      console.error("header missing field 'sigD'");
      return false;
    }
    let sigD = header.sigD;
    const allowedSigDKeys = new Set(["mId", "pars", "hashM", "hashV"]);
    let sigDKeys = new Set(Object.keys(sigD));
    if (!sigDKeys.isSubsetOf(allowedSigDKeys)) {
      console.error("sigD contains invalid parameter");
      return false;
    }
    if (!("mId" in sigD)) {
      console.error("header missing field 'sigD.mId'");
      return false;
    }
    if (sigD.mId != "http://uri.etsi.org/19182/ObjectIdByURIHash") {
      console.error("invalid value for sigD.mId:", sigD.mId);
      return false;
    }

    if (!("pars" in sigD)) {
      console.error("header missing field 'sigD.pars'");
      return false;
    }
    let pars = sigD.pars;

    if (!("hashM" in sigD)) {
      console.error("header missing field 'sigD.hashM'");
      return false;
    }
    let hashAlg;
    switch (sigD.hashM) {
      case "S256":
        hashAlg = "SHA-256";
        break;
      case "S384":
        hashAlg = "SHA-384";
        break;
      case "S512":
        hashAlg = "SHA-512";
        break;
      default:
        console.error("unsupported hashM:", sigD.hashM);
        return false;
    }

    if (!("hashV" in sigD)) {
      console.error("header missing field 'sigD.hashV'");
      return false;
    }
    let hashes = sigD.hashV;
    if (hashes.length != pars.length) {
      console.error("header sigD.pars/hashV mismatch");
      return false;
    }
    for (let hash of hashes) {
      if (typeof hash != "string") {
        console.error("invalid hash:", hash);
        return false;
      }
    }

    return { algorithm, certificates, hashAlg, hashes };
  },

  async verifyTLSCertificateBinding(
    tlsCertificateBinding,
    serverCertificate,
    hostname
  ) {
    let parts = tlsCertificateBinding.split(".");
    if (parts.length != 3) {
      console.error("invalid TLS certificate binding");
      return null;
    }
    if (parts[1] != "") {
      console.error("TLS certificate binding must have empty payload");
      return null;
    }
    let header;
    try {
      header = JSON.parse(QWACs.fromBase64URLEncoding(parts[0]));
    } catch (e) {
      console.error("header is not base64(JSON)");
      return null;
    }
    let params = QWACs.validateTLSCertificateBindingHeader(header);
    if (!params) {
      return null;
    }

    let signingCertificate = params.certificates[0];
    let chain = params.certificates.slice(1);
    if (
      !(await lazy.CertDB.asyncVerifyQWAC(
        Ci.nsIX509CertDB.TwoQWAC,
        signingCertificate,
        hostname,
        chain
      ))
    ) {
      console.error("signing certificate not 2-QWAC");
      return null;
    }

    let spki = signingCertificate.subjectPublicKeyInfo;
    let signingKey;
    try {
      signingKey = await crypto.subtle.importKey(
        "spki",
        new Uint8Array(spki),
        params.algorithm,
        true,
        ["verify"]
      );
    } catch (e) {
      console.error("invalid signing key (algorithm mismatch?)");
      return null;
    }

    let signature;
    try {
      signature = QWACs.fromBase64URLEncoding(parts[2]);
    } catch (e) {
      console.error("signature is not base64");
      return null;
    }

    let signatureValid;
    try {
      signatureValid = await crypto.subtle.verify(
        params.algorithm,
        signingKey,
        stringToArrayBuffer(signature),
        stringToArrayBuffer(parts[0] + ".")
      );
    } catch (e) {
      console.error("failed to verify signature");
      return null;
    }
    if (!signatureValid) {
      console.error("invalid signature");
      return null;
    }

    let serverCertificateHash = await crypto.subtle.digest(
      params.hashAlg,
      stringToArrayBuffer(
        QWACs.toBase64URLEncoding(arrayToString(serverCertificate.getRawDER()))
      )
    );
    if (
      !params.hashes.includes(
        QWACs.toBase64URLEncoding(
          arrayToString(new Uint8Array(serverCertificateHash))
        )
      )
    ) {
      console.error("TLS binding does not cover server certificate");
      return null;
    }
    return signingCertificate;
  },

  async determineQWACStatus(secInfo, uri, browsingContext) {
    if (
      !Services.prefs.getBoolPref("security.qwacs.enabled") ||
      !secInfo ||
      !secInfo.serverCert
    ) {
      return null;
    }

    let hostname;
    try {
      hostname = uri.host;
    } catch {
      return null;
    }

    let windowGlobal = browsingContext.currentWindowGlobal;
    let actor = windowGlobal.getActor("TLSCertificateBinding");
    let tlsCertificateBinding = null;
    try {
      tlsCertificateBinding = await actor.sendQuery(
        "TLSCertificateBinding::Get"
      );
    } catch {
      return null;
    }
    if (tlsCertificateBinding) {
      let twoQwac = await QWACs.verifyTLSCertificateBinding(
        tlsCertificateBinding,
        secInfo.serverCert,
        hostname
      );
      if (twoQwac) {
        return twoQwac;
      }
    }

    let is1qwac = await lazy.CertDB.asyncVerifyQWAC(
      Ci.nsIX509CertDB.OneQWAC,
      secInfo.serverCert,
      hostname,
      secInfo.handshakeCertificates.concat(secInfo.succeededCertChain)
    );
    if (is1qwac) {
      return secInfo.serverCert;
    }

    return null;
  },
};
