/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export var ManifestIcons = {
  async browserFetchIcon(aBrowser, manifest, iconSize, purposes = ["any"]) {
    const msgKey = "DOM:WebManifest:fetchIcon";

    const actor =
      aBrowser.browsingContext.currentWindowGlobal.getActor("ManifestMessages");
    const reply = await actor.sendQuery(msgKey, {
      manifest,
      iconSize,
      purposes,
    });
    if (!reply.success) {
      throw reply.result;
    }
    return reply.result;
  },

  async contentFetchIcon(aWindow, manifest, iconSize, purposes) {
    return getIcon(aWindow, toIconArray(manifest.icons, purposes), iconSize);
  },
};

function parseIconSize(size) {
  if (size === "any" || size === "") {
    return Number.MAX_SAFE_INTEGER;
  }
  return parseInt(size, 10);
}

function toIconArray(icons, purposes) {
  const purposesSet = new Set(purposes);
  const iconBySize = [];
  icons.forEach(icon => {
    if (new Set(icon.purpose).isDisjointFrom(purposesSet)) {
      return;
    }

    const sizes = icon.sizes ?? [""];
    sizes.forEach(size => {
      iconBySize.push({ src: icon.src, size: parseIconSize(size) });
    });
  });
  return iconBySize.sort((a, b) => a.size - b.size);
}

async function getIcon(aWindow, icons, expectedSize) {
  if (!icons.length) {
    throw new Error("Could not find valid icon");
  }
  let index = icons.findIndex(icon => icon.size >= expectedSize);
  if (index === -1) {
    index = icons.length - 1;
  }

  return fetchIcon(aWindow, icons[index].src).catch(() => {
    icons = icons.filter(x => x.src !== icons[index].src);
    return getIcon(aWindow, icons, expectedSize);
  });
}

async function fetchIcon(aWindow, src) {
  const iconURL = new aWindow.URL(src, aWindow.location);
  if (iconURL.protocol === "data:") {
    return iconURL.href;
  }

  const request = new aWindow.Request(iconURL, { mode: "cors" });
  request.overrideContentPolicyType(Ci.nsIContentPolicy.TYPE_IMAGE);
  const response = await aWindow.fetch(request);
  const blob = await response.blob();
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onloadend = () => resolve(reader.result);
    reader.onerror = reject;
    reader.readAsDataURL(blob);
  });
}
