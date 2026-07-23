/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.*/

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ManifestFinder: "moz-src:///dom/manifest/ManifestFinder.sys.mjs",
  ManifestIcons: "moz-src:///dom/manifest/ManifestIcons.sys.mjs",
  ManifestObtainer: "moz-src:///dom/manifest/ManifestObtainer.sys.mjs",
});

export class ManifestMessagesChild extends JSWindowActorChild {
  receiveMessage(message) {
    switch (message.name) {
      case "DOM:WebManifest:hasManifestLink":
        return this.hasManifestLink();
      case "DOM:ManifestObtainer:Obtain":
        return this.obtainManifest(message.data);
      case "DOM:WebManifest:fetchIcon":
        return this.fetchIcon(message);
    }
    return undefined;
  }

  hasManifestLink() {
    const response = makeMsgResponse();
    response.result = lazy.ManifestFinder.contentHasManifestLink(
      this.contentWindow
    );
    response.success = true;
    return response;
  }

  async obtainManifest(options) {
    const { checkConformance } = options;
    const response = makeMsgResponse();
    try {
      response.result = await lazy.ManifestObtainer.contentObtainManifest(
        this.contentWindow,
        { checkConformance }
      );
      response.success = true;
    } catch (err) {
      response.result = serializeError(err);
    }
    return response;
  }

  async fetchIcon({ data: { manifest, iconSize, purposes } }) {
    const response = makeMsgResponse();
    try {
      response.result = await lazy.ManifestIcons.contentFetchIcon(
        this.contentWindow,
        manifest,
        iconSize,
        purposes
      );
      response.success = true;
    } catch (err) {
      response.result = serializeError(err);
    }
    return response;
  }
}

function serializeError(aError) {
  const clone = {
    fileName: aError.fileName,
    lineNumber: aError.lineNumber,
    columnNumber: aError.columnNumber,
    stack: aError.stack,
    message: aError.message,
    name: aError.name,
  };
  return clone;
}

function makeMsgResponse() {
  return {
    success: false,
    result: undefined,
  };
}
