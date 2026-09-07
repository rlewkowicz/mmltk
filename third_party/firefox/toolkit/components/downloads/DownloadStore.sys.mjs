/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  Downloads: "resource://gre/modules/Downloads.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "gTextDecoder", function () {
  return new TextDecoder();
});

ChromeUtils.defineLazyGetter(lazy, "gTextEncoder", function () {
  return new TextEncoder();
});

export var DownloadStore = function (aList, aPath) {
  this.list = aList;
  this.path = aPath;
};

DownloadStore.prototype = {
  list: null,

  path: "",

  onsaveitem: () => true,

  load: function DS_load() {
    return (async () => {
      let bytes;
      try {
        bytes = await IOUtils.read(this.path);
      } catch (ex) {
        if (!(ex.name == "NotFoundError")) {
          throw ex;
        }
        return;
      }

      let storeData = JSON.parse(lazy.gTextDecoder.decode(bytes));

      for (let downloadData of storeData.list) {
        try {
          let download = await lazy.Downloads.createDownload(downloadData);

          try {
            if (!download.succeeded && !download.canceled && !download.error) {
              download.start().catch(() => {});
            } else {
              await download.refresh();
            }
          } finally {
            await this.list.add(download);
          }
        } catch (ex) {
          console.error(ex);
        }
      }

    })();
  },

  save: function DS_save() {
    return (async () => {
      let downloads = await this.list.getAll();

      let storeData = { list: [] };
      let atLeastOneDownload = false;
      for (let download of downloads) {
        try {
          if (!this.onsaveitem(download)) {
            continue;
          }

          let serializable = download.toSerializable();
          if (!serializable) {
            continue;
          }
          storeData.list.push(serializable);
          atLeastOneDownload = true;
        } catch (ex) {
          console.error(ex);
        }
      }

      if (atLeastOneDownload) {
        let bytes = lazy.gTextEncoder.encode(JSON.stringify(storeData));
        await IOUtils.write(this.path, bytes, {
          tmpPath: this.path + ".tmp",
        });
      } else {
        try {
          await IOUtils.remove(this.path);
        } catch (ex) {
          if (!(ex.name == "NotFoundError" || ex.name == "NotAllowedError")) {
            throw ex;
          }
        }
      }
    })();
  },
};
