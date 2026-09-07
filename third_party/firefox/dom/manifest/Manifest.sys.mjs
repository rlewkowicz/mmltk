/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { ManifestObtainer } from "moz-src:///dom/manifest/ManifestObtainer.sys.mjs";

import { ManifestIcons } from "moz-src:///dom/manifest/ManifestIcons.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  JSONFile: "resource://gre/modules/JSONFile.sys.mjs",
});

function generateHash(aString, hashAlg) {
  const cryptoHash = Cc["@mozilla.org/security/hash;1"].createInstance(
    Ci.nsICryptoHash
  );
  cryptoHash.init(hashAlg);
  const stringStream = Cc[
    "@mozilla.org/io/string-input-stream;1"
  ].createInstance(Ci.nsIStringInputStream);
  stringStream.setByteStringData(aString);
  cryptoHash.updateFromStream(stringStream, -1);
  return cryptoHash.finish(true).replace(/\//g, "-");
}

function stripQuery(uri) {
  return uri.mutate().setQuery("").setRef("").finalize().spec;
}

const MANIFESTS_DIR = PathUtils.join(PathUtils.profileDir, "manifests");

const MANIFESTS_FILE = "manifest-scopes.json";


class Manifest {
  constructor(browser, manifestUrl) {
    this._manifestUrl = manifestUrl;
    const filename =
      generateHash(manifestUrl, Ci.nsICryptoHash.SHA256) + ".json";
    this._path = PathUtils.join(MANIFESTS_DIR, filename);
    this.browser = browser;
  }

  async removeMD5BasedFilename() {
    const filenameMD5 =
      generateHash(this._manifestUrl, Ci.nsICryptoHash.MD5) + ".json";
    const MD5Path = PathUtils.join(MANIFESTS_DIR, filenameMD5);
    try {
      await IOUtils.copy(MD5Path, this._path, { noOverwrite: true });
    } catch (error) {
    }

    try {
      await IOUtils.remove(MD5Path);
    } catch {
    }
  }

  get browser() {
    return this._browser;
  }

  set browser(aBrowser) {
    this._browser = aBrowser;
  }

  async initialize() {
    await this.removeMD5BasedFilename();
    this._store = new lazy.JSONFile({ path: this._path, saveDelayMs: 100 });
    await this._store.load();
  }

  async prefetch(browser) {
    const manifestData = await ManifestObtainer.browserObtainManifest(browser);
    const icon = await ManifestIcons.browserFetchIcon(
      browser,
      manifestData,
      192
    );
    const data = {
      installed: false,
      manifest: manifestData,
      cached_icon: icon,
    };
    return data;
  }

  async install() {
    const manifestData = await ManifestObtainer.browserObtainManifest(
      this._browser
    );
    this._store.data = {
      installed: true,
      manifest: manifestData,
    };
    Manifests.manifestInstalled(this);
    this._store.saveSoon();
  }

  async icon(expectedSize) {
    if ("cached_icon" in this._store.data) {
      return this._store.data.cached_icon;
    }
    const icon = await ManifestIcons.browserFetchIcon(
      this._browser,
      this._store.data.manifest,
      expectedSize
    );
    this._store.data.cached_icon = icon;
    this._store.saveSoon();
    return icon;
  }

  get scope() {
    const scope =
      this._store.data.manifest.scope || this._store.data.manifest.start_url;
    return stripQuery(Services.io.newURI(scope));
  }

  get name() {
    return (
      this._store.data.manifest.short_name ||
      this._store.data.manifest.name ||
      this._store.data.manifest.short_url
    );
  }

  get url() {
    return this._manifestUrl;
  }

  get installed() {
    return (this._store.data && this._store.data.installed) || false;
  }

  get start_url() {
    return this._store.data.manifest.start_url;
  }

  get path() {
    return this._path;
  }
}

export var Manifests = {
  async _initialize() {
    if (this._readyPromise) {
      return this._readyPromise;
    }

    this._readyPromise = (async () => {
      await IOUtils.makeDirectory(MANIFESTS_DIR, { ignoreExisting: true });

      this._path = PathUtils.join(PathUtils.profileDir, MANIFESTS_FILE);
      this._store = new lazy.JSONFile({ path: this._path });
      await this._store.load();

      if (!this._store.data.hasOwnProperty("scopes")) {
        this._store.data.scopes = new Map();
      }
    })();

    this.manifestObjs = new Map();
    return this._readyPromise;
  },

  manifestInstalled(manifest) {
    this._store.data.scopes[manifest.scope] = manifest.url;
    this._store.saveSoon();
  },

  findManifestUrl(url) {
    for (let scope in this._store.data.scopes) {
      if (url.startsWith(scope)) {
        return this._store.data.scopes[scope];
      }
    }
    return null;
  },

  async getManifest(browser, manifestUrl) {
    if (!this._readyPromise) {
      await this._initialize();
    }

    if (!manifestUrl) {
      const url = stripQuery(browser.currentURI);
      manifestUrl = this.findManifestUrl(url);
    }

    if (manifestUrl === null) {
      return null;
    }

    if (this.manifestObjs.has(manifestUrl)) {
      const manifest = this.manifestObjs.get(manifestUrl);
      if (manifest.browser !== browser) {
        manifest.browser = browser;
      }
      return manifest;
    }

    const manifest = new Manifest(browser, manifestUrl);
    this.manifestObjs.set(manifestUrl, manifest);
    await manifest.initialize();
    return manifest;
  },
};
