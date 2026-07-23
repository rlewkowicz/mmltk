/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export var FileUtils = {
  MODE_RDONLY: 0x01,
  MODE_WRONLY: 0x02,
  MODE_RDWR: 0x04,
  MODE_CREATE: 0x08,
  MODE_APPEND: 0x10,
  MODE_TRUNCATE: 0x20,

  PERMS_FILE: 0o644,
  PERMS_DIRECTORY: 0o755,

  getDir: function FileUtils_getDir(key, pathArray) {
    var dir = Services.dirsvc.get(key, Ci.nsIFile);
    for (var i = 0; i < pathArray.length; ++i) {
      dir.append(pathArray[i]);
    }
    return dir;
  },

  openFileOutputStream: function FileUtils_openFileOutputStream(
    file,
    modeFlags
  ) {
    var fos = Cc["@mozilla.org/network/file-output-stream;1"].createInstance(
      Ci.nsIFileOutputStream
    );
    return this._initFileOutputStream(fos, file, modeFlags);
  },

  openAtomicFileOutputStream: function FileUtils_openAtomicFileOutputStream(
    file,
    modeFlags
  ) {
    var fos = Cc[
      "@mozilla.org/network/atomic-file-output-stream;1"
    ].createInstance(Ci.nsIFileOutputStream);
    return this._initFileOutputStream(fos, file, modeFlags);
  },

  openSafeFileOutputStream: function FileUtils_openSafeFileOutputStream(
    file,
    modeFlags
  ) {
    var fos = Cc[
      "@mozilla.org/network/safe-file-output-stream;1"
    ].createInstance(Ci.nsIFileOutputStream);
    return this._initFileOutputStream(fos, file, modeFlags);
  },

  _initFileOutputStream: function FileUtils__initFileOutputStream(
    fos,
    file,
    modeFlags
  ) {
    if (modeFlags === undefined) {
      modeFlags = this.MODE_WRONLY | this.MODE_CREATE | this.MODE_TRUNCATE;
    }
    fos.init(file, modeFlags, this.PERMS_FILE, fos.DEFER_OPEN);
    return fos;
  },

  closeAtomicFileOutputStream: function FileUtils_closeAtomicFileOutputStream(
    stream
  ) {
    if (stream instanceof Ci.nsISafeOutputStream) {
      try {
        stream.finish();
        return;
      } catch (e) {}
    }
    stream.close();
  },

  closeSafeFileOutputStream: function FileUtils_closeSafeFileOutputStream(
    stream
  ) {
    if (stream instanceof Ci.nsISafeOutputStream) {
      try {
        stream.finish();
        return;
      } catch (e) {}
    }
    stream.close();
  },

  File: Components.Constructor(
    "@mozilla.org/file/local;1",
    Ci.nsIFile,
    "initWithPath"
  ),
};
