/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


export var DownloadPaths = {
  sanitize(
    leafName,
    {
      compressWhitespaces = true,
      allowInvalidFilenames = false,
      allowDirectoryNames = false,
    } = {}
  ) {
    const mimeSvc = Cc["@mozilla.org/mime;1"].getService(Ci.nsIMIMEService);

    let flags = mimeSvc.VALIDATE_SANITIZE_ONLY | mimeSvc.VALIDATE_DONT_TRUNCATE;
    if (!compressWhitespaces) {
      flags |= mimeSvc.VALIDATE_DONT_COLLAPSE_WHITESPACE;
    }
    if (allowInvalidFilenames) {
      flags |= mimeSvc.VALIDATE_ALLOW_INVALID_FILENAMES;
    }
    if (allowDirectoryNames) {
      flags |= mimeSvc.VALIDATE_ALLOW_DIRECTORY_NAMES;
    }
    return mimeSvc.validateFileNameForSaving(leafName, "", flags);
  },

  createNiceUniqueFile(templateFile) {
    let curFile = templateFile.clone().QueryInterface(Ci.nsIFile);
    let [base, ext] = DownloadPaths.splitBaseNameAndExtension(curFile.leafName);
    for (let i = 1; i < 10000 && curFile.exists(); i++) {
      curFile.leafName = base + "(" + i + ")" + ext;
    }
    curFile.createUnique(Ci.nsIFile.NORMAL_FILE_TYPE, 0o644);
    return curFile;
  },

  splitBaseNameAndExtension(leafName) {
    let [, base, ext] =
      /(.*?)(\.[A-Z0-9]{1,3}\.(?:bz2|gz|lzma|xz|zst|Z)|\.[^.]*)?$/i.exec(
        leafName
      );
    return [base, ext || ""];
  },
};
