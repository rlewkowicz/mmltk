/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { QR } from "moz-src:///toolkit/components/qrcode/encoder.mjs";
import { PromiseWorker } from "resource://gre/modules/workers/PromiseWorker.mjs";

const FINDER_SIZE = 7;
const CELL_SIZE = 20;
const MARGIN_CELLS = 4;
const DOT_RADIUS_FACTOR = 0.4;
const FINDER_OUTER_CORNER_RADIUS_FACTOR = 1.2;
const FINDER_INNER_CORNER_RADIUS_FACTOR = 0.6;
const MIN_LOGO_MODULE_SPAN = 6;
const MAX_LOGO_MODULE_SPAN = 8;

class QRCodeWorkerImpl {
  constructor() {
    this.#connectToPromiseWorker();
  }

  #getMargin() {
    return MARGIN_CELLS * CELL_SIZE;
  }

  #getCanvasSize(dotCount, margin = this.#getMargin()) {
    return dotCount * CELL_SIZE + margin * 2;
  }

  #getFinderPatternOrigins(dotCount) {
    return [
      [0, 0],
      [0, dotCount - FINDER_SIZE],
      [dotCount - FINDER_SIZE, 0],
    ];
  }

  #forEachVisibleDarkModule(matrix, placement, margin, callback) {
    const dotCount = matrix.length;
    const isInFinderPatternCorners = (row, col) =>
      (row < FINDER_SIZE && col < FINDER_SIZE) ||
      (row < FINDER_SIZE && col >= dotCount - FINDER_SIZE) ||
      (row >= dotCount - FINDER_SIZE && col < FINDER_SIZE);

    for (let row = 0; row < dotCount; row++) {
      for (let col = 0; col < dotCount; col++) {
        if (isInFinderPatternCorners(row, col) || !matrix[row][col]) {
          continue;
        }
        const dotX = margin + (col + 0.5) * CELL_SIZE;
        const dotY = margin + (row + 0.5) * CELL_SIZE;
        const offsetX = dotX - placement.centerX;
        const offsetY = dotY - placement.centerY;
        if (
          placement.showLogo &&
          Math.hypot(offsetX, offsetY) <
            placement.clearRadius + CELL_SIZE * DOT_RADIUS_FACTOR
        ) {
          continue;
        }
        callback(dotX, dotY);
      }
    }
  }

  #drawFinderPattern(ctx, x, y) {
    const outerSize = FINDER_SIZE * CELL_SIZE;
    const ringSize = (FINDER_SIZE - 2) * CELL_SIZE;
    const centerSize = (FINDER_SIZE - 4) * CELL_SIZE;
    const outerR = CELL_SIZE * FINDER_OUTER_CORNER_RADIUS_FACTOR;
    const innerR = CELL_SIZE * FINDER_INNER_CORNER_RADIUS_FACTOR;

    ctx.fillStyle = "black";
    ctx.beginPath();
    ctx.roundRect(x, y, outerSize, outerSize, outerR);
    ctx.fill();

    ctx.fillStyle = "white";
    ctx.beginPath();
    ctx.roundRect(x + CELL_SIZE, y + CELL_SIZE, ringSize, ringSize, innerR);
    ctx.fill();

    ctx.fillStyle = "black";
    ctx.beginPath();
    ctx.roundRect(
      x + 2 * CELL_SIZE,
      y + 2 * CELL_SIZE,
      centerSize,
      centerSize,
      innerR
    );
    ctx.fill();
  }

  #drawQRBodyToCanvas(ctx, matrix, placement, margin = this.#getMargin()) {
    const dotCount = matrix.length;
    const canvasSize = this.#getCanvasSize(dotCount, margin);

    ctx.fillStyle = "white";
    ctx.fillRect(0, 0, canvasSize, canvasSize);

    ctx.fillStyle = "black";
    this.#forEachVisibleDarkModule(matrix, placement, margin, (dotX, dotY) => {
      ctx.beginPath();
      ctx.arc(dotX, dotY, CELL_SIZE * DOT_RADIUS_FACTOR, 0, Math.PI * 2);
      ctx.fill();
    });

    for (const [startRow, startCol] of this.#getFinderPatternOrigins(
      dotCount
    )) {
      const x = margin + startCol * CELL_SIZE;
      const y = margin + startRow * CELL_SIZE;
      this.#drawFinderPattern(ctx, x, y);
    }
  }

  #getPreferredLogoSize(canvasSize) {
    const desiredLogoSize = Math.round(canvasSize * 0.18);
    return Math.min(desiredLogoSize, MAX_LOGO_MODULE_SPAN * CELL_SIZE);
  }

  generateQRMatrix(url) {
    if (!QR || !QR.encodeToMatrix) {
      throw new Error("QRCode library not available in worker");
    }
    const { matrix, dotCount } = QR.encodeToMatrix(url, "M");
    return { matrix, dotCount };
  }

  getLogoPlacement(dotCount, margin = this.#getMargin()) {
    const canvasSize = this.#getCanvasSize(dotCount, margin);
    const preferredLogoSize = this.#getPreferredLogoSize(canvasSize);
    const minimumLogoSize = MIN_LOGO_MODULE_SPAN * CELL_SIZE;
    const logoSize = Math.max(preferredLogoSize, minimumLogoSize);

    return {
      centerX: canvasSize / 2,
      centerY: canvasSize / 2,
      clearRadius: logoSize / 2,
      logoSize,
      showLogo: true,
    };
  }

  async generateFullQRCode(url, showLogo = true) {
    let matrix, dotCount, ecLevel;
    for (const level of ["M", "L"]) {
      try {
        ({ matrix, dotCount } = QR.encodeToMatrix(url, level));
        ecLevel = level;
        break;
      } catch (e) {
        if (level === "L") {
          throw e;
        }
      }
    }
    const margin = this.#getMargin();
    const placement = this.getLogoPlacement(dotCount, margin);
    if (ecLevel !== "M" || !showLogo) {
      placement.showLogo = false;
    }
    const size = this.#getCanvasSize(dotCount, margin);

    const canvas = new OffscreenCanvas(size, size);
    const ctx = canvas.getContext("2d");

    this.#drawQRBodyToCanvas(ctx, matrix, placement, margin);

    if (placement.showLogo) {
      try {
        const response = await fetch(
          "chrome://branding/content/about-logo@2x.png"
        );
        if (!response.ok) {
          throw new Error(`Logo fetch failed: ${response.status}`);
        }
        const blob = await response.blob();
        const logoSize = Math.round(placement.logoSize);
        const logoBitmap = await globalThis.createImageBitmap(blob, {
          resizeWidth: logoSize,
          resizeHeight: logoSize,
          resizeQuality: "high",
        });
        ctx.imageSmoothingEnabled = true;
        ctx.imageSmoothingQuality = "high";
        ctx.drawImage(
          logoBitmap,
          placement.centerX - placement.logoSize / 2,
          placement.centerY - placement.logoSize / 2,
          placement.logoSize,
          placement.logoSize
        );
        logoBitmap.close();
      } catch (e) {
        console.warn("Failed to load Firefox logo for QR code:", e);
      }
    }

    const pngBlob = await canvas.convertToBlob({ type: "image/png" });
    const arrayBuffer = await pngBlob.arrayBuffer();
    const base64 = new Uint8Array(arrayBuffer).toBase64();
    return `data:image/png;base64,${base64}`;
  }

  #connectToPromiseWorker() {
    const worker = new PromiseWorker.AbstractWorker();

    worker.dispatch = (method, args = []) => {
      if (!this[method]) {
        throw new Error("Method does not exist: " + method);
      }
      return this[method](...args);
    };

    worker.close = () => self.close();

    worker.postMessage = (message, ...transfers) => {
      self.postMessage(message, ...transfers);
    };

    self.addEventListener("message", msg => worker.handleMessage(msg));
    self.addEventListener("unhandledrejection", function (error) {
      throw error.reason;
    });
  }
}

new QRCodeWorkerImpl();
