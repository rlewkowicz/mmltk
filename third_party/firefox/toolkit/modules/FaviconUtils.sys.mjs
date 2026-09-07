/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const TYPE_ICO = "image/x-icon";
const TYPE_SVG = "image/svg+xml";

export { TYPE_ICO, TYPE_SVG };

export const SVG_DATA_URI_PREFIX = `data:${TYPE_SVG};base64,`;

export const TRUSTED_FAVICON_SCHEMES = Object.freeze([
  "chrome",
  "about",
  "resource",
]);

function getMozRemoteImageURL(url, options = {}) {
  if (options.size !== undefined) {
    options.height = options.width = options.size;
    delete options.size;
  }

  let params = new URLSearchParams({
    url,
    ...options,
  });
  return "moz-remote-image://?" + params;
}

export { getMozRemoteImageURL };

export function blobAsDataURL(blob) {
  return new Promise((resolve, reject) => {
    let reader = new FileReader();
    reader.addEventListener("load", () => resolve(reader.result));
    reader.addEventListener("error", reject);
    reader.readAsDataURL(blob);
  });
}

export let FaviconUtils = {
  SVG_DATA_URI_PREFIX,
  TRUSTED_FAVICON_SCHEMES,
  getMozRemoteImageURL,
};
