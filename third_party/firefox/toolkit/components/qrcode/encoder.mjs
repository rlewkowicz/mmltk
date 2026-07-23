/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(
  lazy,
  {
    qrcode: "moz-src:///third_party/js/qrcode/qrcode.mjs",
    QRErrorCorrectionLevel: "moz-src:///third_party/js/qrcode/qrcode.mjs",
    QRRSBlock: "moz-src:///third_party/js/qrcode/qrcode.mjs",
  },
  { global: "current" }
);

function findMinimumVersion(message, errorCorrectionLevelChar) {
  const msgLength = message.length;
  const errorCorrectionLevel =
    lazy.QRErrorCorrectionLevel[errorCorrectionLevelChar];
  for (let version = 1; version <= 40; version++) {
    const rsBlocks = lazy.QRRSBlock.getRSBlocks(version, errorCorrectionLevel);
    let maxLength = rsBlocks.reduce((prev, block) => {
      return prev + block.dataCount;
    }, 0);
    maxLength -= 2;
    if (msgLength <= maxLength) {
      return version;
    }
  }
  throw new Error("Message too large");
}

function createEncoder(message, errorCorrectionLevelChar, version) {
  const levelChar = errorCorrectionLevelChar ?? "H";
  const qrVersion = version ?? findMinimumVersion(message, levelChar);
  const encoder = new lazy.qrcode(qrVersion, levelChar);
  encoder.addData(message);
  encoder.make();
  return encoder;
}

function encodeToDataURI(message, errorCorrectionLevelChar, version) {
  const encoder = createEncoder(message, errorCorrectionLevelChar, version);
  const dataURI = encoder.createDataURL();
  const dotCount = encoder.getModuleCount();
  const cellSize = 2;
  const margin = cellSize * 4;
  const size = dotCount * cellSize + margin * 2;
  return { src: dataURI, width: size, height: size };
}

function encodeToMatrix(message, errorCorrectionLevelChar, version) {
  const encoder = createEncoder(message, errorCorrectionLevelChar, version);
  const dotCount = encoder.getModuleCount();
  const matrix = [];
  for (let row = 0; row < dotCount; row++) {
    matrix[row] = [];
    for (let col = 0; col < dotCount; col++) {
      matrix[row][col] = encoder.isDark(row, col);
    }
  }
  return { matrix, dotCount };
}

export const QR = {
  encodeToDataURI,
  encodeToMatrix,
};
