/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { Integration } from "resource://gre/modules/Integration.sys.mjs";

import {
  Download,
  DownloadError,
} from "resource://gre/modules/DownloadCore.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  DownloadCombinedList: "resource://gre/modules/DownloadList.sys.mjs",
  DownloadList: "resource://gre/modules/DownloadList.sys.mjs",
  DownloadSummary: "resource://gre/modules/DownloadList.sys.mjs",
});

Integration.downloads.defineESModuleGetter(
  lazy,
  "DownloadIntegration",
  "resource://gre/modules/DownloadIntegration.sys.mjs"
);

export const Downloads = {
  get PUBLIC() {
    return "{Downloads.PUBLIC}";
  },
  get PRIVATE() {
    return "{Downloads.PRIVATE}";
  },
  get ALL() {
    return "{Downloads.ALL}";
  },

  async createDownload(properties) {
    return Download.fromSerializable(properties);
  },

  async fetch(source, target, options) {
    const download = await this.createDownload({ source, target });

    if (options?.isPrivate) {
      download.source.isPrivate = options.isPrivate;
    }
    return download.start();
  },

  async getList(type) {
    if (!this._promiseListsInitialized) {
      this._promiseListsInitialized = (async () => {
        let publicList = new lazy.DownloadList();
        let privateList = new lazy.DownloadList();
        let combinedList = new lazy.DownloadCombinedList(
          publicList,
          privateList
        );

        try {
          await lazy.DownloadIntegration.addListObservers(publicList, false);
          await lazy.DownloadIntegration.addListObservers(privateList, true);
          await lazy.DownloadIntegration.initializePublicDownloadList(
            publicList
          );
        } catch (err) {
          console.error(err);
        }

        let publicSummary = await this.getSummary(Downloads.PUBLIC);
        let privateSummary = await this.getSummary(Downloads.PRIVATE);
        let combinedSummary = await this.getSummary(Downloads.ALL);

        await publicSummary.bindToList(publicList);
        await privateSummary.bindToList(privateList);
        await combinedSummary.bindToList(combinedList);

        this._lists[Downloads.PUBLIC] = publicList;
        this._lists[Downloads.PRIVATE] = privateList;
        this._lists[Downloads.ALL] = combinedList;
      })();
    }

    await this._promiseListsInitialized;

    return this._lists[type];
  },

  _promiseListsInitialized: null,

  _lists: {},

  async getSummary(type) {
    if (
      type != Downloads.PUBLIC &&
      type != Downloads.PRIVATE &&
      type != Downloads.ALL
    ) {
      throw new Error("Invalid type argument.");
    }

    if (!(type in this._summaries)) {
      this._summaries[type] = new lazy.DownloadSummary();
    }

    return this._summaries[type];
  },

  _summaries: {},

  getSystemDownloadsDirectory() {
    return lazy.DownloadIntegration.getSystemDownloadsDirectory();
  },

  getPreferredDownloadsDirectory() {
    return lazy.DownloadIntegration.getPreferredDownloadsDirectory();
  },

  getPreferredScreenshotsDirectory() {
    return lazy.DownloadIntegration.getPreferredScreenshotsDirectory();
  },

  getTemporaryDownloadsDirectory() {
    return lazy.DownloadIntegration.getTemporaryDownloadsDirectory();
  },

  Error: DownloadError,
};
