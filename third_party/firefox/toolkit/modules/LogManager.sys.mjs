/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


// eslint-disable-next-line mozilla/use-console-createInstance
import { Log } from "resource://gre/modules/Log.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  FileUtils: "resource://gre/modules/FileUtils.sys.mjs",
  NetUtil: "resource://gre/modules/NetUtil.sys.mjs",
});

const DEFAULT_MAX_ERROR_AGE = 20 * 24 * 60 * 60; 


var formatter;
var dumpAppender;
var consoleAppender;

var allBranches = new Set();

const STREAM_SEGMENT_SIZE = 4096;
const PR_UINT32_MAX = 0xffffffff;

class StorageStreamAppender extends Log.Appender {
  constructor(formatter, { onAppendTopic = "" } = {}) {
    super(formatter);
    this._name = "StorageStreamAppender";
    this._onAppendTopic = onAppendTopic;
    this._converterStream = null; 
    this._outputStream = null; 

    this._ss = null;
  }

  get outputStream() {
    if (!this._outputStream) {
      this._outputStream = this.newOutputStream();
      if (!this._outputStream) {
        return null;
      }

      if (!this._converterStream) {
        this._converterStream = Cc[
          "@mozilla.org/intl/converter-output-stream;1"
        ].createInstance(Ci.nsIConverterOutputStream);
      }
      this._converterStream.init(this._outputStream, "UTF-8");
    }
    return this._converterStream;
  }

  newOutputStream() {
    let ss = (this._ss = Cc["@mozilla.org/storagestream;1"].createInstance(
      Ci.nsIStorageStream
    ));
    ss.init(STREAM_SEGMENT_SIZE, PR_UINT32_MAX, null);
    return ss.getOutputStream(0);
  }

  getInputStream() {
    if (!this._ss) {
      return null;
    }
    return this._ss.newInputStream(0);
  }

  reset() {
    if (!this._outputStream) {
      return;
    }
    this.outputStream.close();
    this._outputStream = null;
    this._ss = null;
  }

  doAppend(formatted) {
    if (!formatted) {
      return;
    }
    let didAppend = false;
    try {
      this.outputStream.writeString(formatted + "\n");
      didAppend = true;
    } catch (ex) {
      if (ex.result == Cr.NS_BASE_STREAM_CLOSED) {
        this._outputStream = null;
      }
      try {
        this.outputStream.writeString(formatted + "\n");
        didAppend = true;
      } catch (ex) {
      }
    }
    if (didAppend && this._onAppendTopic) {
      Services.obs.notifyObservers(null, this._onAppendTopic, {});
    }
  }
}

class FlushableStorageAppender extends StorageStreamAppender {
  #overwriteFileOnFlush = true;
  #debugMessageCount = 0;
  #lastFlushTime = 0;

  constructor(
    formatter,
    { overwriteFileOnFlush = true, onAppendTopic = "" } = {}
  ) {
    super(formatter, { onAppendTopic });
    this.sawError = false;
    this.#overwriteFileOnFlush = overwriteFileOnFlush;
  }

  get lastFlushTime() {
    return this.#lastFlushTime;
  }

  get debugMessageCount() {
    return this.#debugMessageCount;
  }

  append(message) {
    if (message.level >= Log.Level.Error) {
      this.sawError = true;
    } else {
      this.#debugMessageCount++;
    }
    StorageStreamAppender.prototype.append.call(this, message);
  }

  reset() {
    super.reset();
    this.sawError = false;
    this.#debugMessageCount = 0;
  }

  async flushToFile(subdirArray, filename, log) {
    let inStream = this.getInputStream();
    this.reset();
    this.#lastFlushTime = Date.now();
    if (!inStream) {
      log.debug("Failed to flush log to a file - no input stream");
      return;
    }
    log.debug("Flushing file log");
    log.trace("Beginning stream copy to " + filename + ": " + Date.now());
    try {
      await this._copyStreamToFile(inStream, subdirArray, filename, log);
      log.trace("onCopyComplete", Date.now());
    } catch (ex) {
      log.error("Failed to copy log stream to file", ex);
    }
  }

  async _copyStreamToFile(inputStream, subdirArray, outputFileName, log) {
    let outputDirectory = PathUtils.join(PathUtils.profileDir, ...subdirArray);
    await IOUtils.makeDirectory(outputDirectory);
    let fullOutputFileName = PathUtils.join(outputDirectory, outputFileName);

    let outputStream = Cc[
      "@mozilla.org/network/file-output-stream;1"
    ].createInstance(Ci.nsIFileOutputStream);

    let ioFlags = -1;
    if (!this.#overwriteFileOnFlush) {
      ioFlags = 0x02; 
      ioFlags |= 0x08; 
      ioFlags |= 0x10; 
    }

    outputStream.init(
      new lazy.FileUtils.File(fullOutputFileName),
      ioFlags,
      -1,
      Ci.nsIFileOutputStream.DEFER_OPEN
    );

    await new Promise(resolve =>
      lazy.NetUtil.asyncCopy(inputStream, outputStream, () => resolve())
    );

    outputStream.close();
    log.trace("finished copy to", fullOutputFileName);
  }
}

export class LogManager {
  constructor(options = {}) {
    this._prefObservers = [];
    this.#init(options);
  }

  static StorageStreamAppender = StorageStreamAppender;

  _cleaningUpFileLogs = false;

  #init({
    prefRoot,
    logNames,
    logFilePrefix,
    logFileSubDirectoryEntries,
    testTopicPrefix,
    fileAppenderChangeTopic,
    overwriteFileOnFlush,
  } = {}) {
    this._prefs = Services.prefs.getBranch(prefRoot);
    this._prefsBranch = prefRoot;

    this.logFilePrefix = logFilePrefix;
    this._testTopicPrefix = testTopicPrefix;
    this._fileAppenderChangeTopic = fileAppenderChangeTopic;
    this._overwriteFileOnFlush = overwriteFileOnFlush;

    this.logFileSubDirectoryEntries = Object.freeze(logFileSubDirectoryEntries);

    if (!formatter) {
      formatter = new Log.BasicFormatter();
      consoleAppender = new Log.ConsoleAppender(formatter);
      dumpAppender = new Log.DumpAppender(formatter);
    }

    allBranches.add(this._prefsBranch);
    let setupAppender = (
      appender,
      prefName,
      defaultLevel,
      findSmallest = false
    ) => {
      let observer = newVal => {
        let level = Log.Level[newVal] || defaultLevel;
        if (findSmallest) {
          for (let branch of allBranches) {
            let lookPrefBranch = Services.prefs.getBranch(branch);
            let lookVal =
              Log.Level[lookPrefBranch.getStringPref(prefName, null)];
            if (lookVal && lookVal < level) {
              level = lookVal;
            }
          }
        }
        appender.level = level;
      };
      this._prefs.addObserver(prefName, observer);
      this._prefObservers.push([prefName, observer]);
      observer(this._prefs.getStringPref(prefName, null));
      return observer;
    };

    this._observeConsolePref = setupAppender(
      consoleAppender,
      "log.appender.console",
      Log.Level.Fatal,
      true
    );
    this._observeDumpPref = setupAppender(
      dumpAppender,
      "log.appender.dump",
      Log.Level.Error,
      true
    );

    let fapp = (this._fileAppender = new FlushableStorageAppender(formatter, {
      overwriteFileOnFlush: this._overwriteFileOnFlush,
      onAppendTopic: this._fileAppenderChangeTopic,
    }));
    this._observeStreamPref = setupAppender(
      fapp,
      "log.appender.file.level",
      Log.Level.Debug
    );

    for (let logName of logNames) {
      let log = Log.repository.getLogger(logName);
      for (let appender of [fapp, dumpAppender, consoleAppender]) {
        log.addAppender(appender);
      }
    }
    this._log = Log.repository.getLogger(logNames[0] + ".LogManager");
  }

  finalize() {
    for (let [prefName, observer] of this._prefObservers) {
      this._prefs.removeObserver(prefName, observer);
    }
    this._prefObservers = [];
    try {
      allBranches.delete(this._prefsBranch);
    } catch (e) {}
    this._prefs = null;
  }

  get sawError() {
    return this._fileAppender.sawError;
  }

  SUCCESS_LOG_WRITTEN = "success-log-written";
  ERROR_LOG_WRITTEN = "error-log-written";

  getLogFilename(reasonPrefix = "success", timestamp = Date.now()) {
    return (
      [reasonPrefix, this.logFilePrefix, timestamp]
        .filter(val => !!val)
        .join("-") + ".txt"
    );
  }

  async resetFileLog() {
    try {
      let flushToFile;
      let reasonPrefix;
      let reason;
      if (this._fileAppender.sawError) {
        reason = this.ERROR_LOG_WRITTEN;
        flushToFile = this._prefs.getBoolPref(
          "log.appender.file.logOnError",
          true
        );
        reasonPrefix = "error";
      } else if (this._fileAppender.debugMessageCount) {
        reason = this.SUCCESS_LOG_WRITTEN;
        flushToFile = this._prefs.getBoolPref(
          "log.appender.file.logOnSuccess",
          false
        );
        reasonPrefix = "success";
      }

      if (!flushToFile || !reasonPrefix) {
        this._fileAppender.reset();
        return null;
      }

      let filename = this.getLogFilename(reasonPrefix);
      await this._fileAppender.flushToFile(
        this.logFileSubDirectoryEntries,
        filename,
        this._log
      );
      if (!this._cleaningUpFileLogs) {
        this._log.trace("Running cleanup.");
        try {
          await this.cleanupLogs();
        } catch (err) {
          this._log.error("Failed to cleanup logs", err);
        }
      }
      return reason;
    } catch (ex) {
      this._log.error("Failed to resetFileLog", ex);
      return null;
    }
  }

  cleanupLogs() {
    let maxAge = this._prefs.getIntPref(
      "log.appender.file.maxErrorAge",
      DEFAULT_MAX_ERROR_AGE
    );
    let threshold = Date.now() - 1000 * maxAge;
    this._log.debug("Log cleanup threshold time: " + threshold);

    let shouldDelete = fileInfo => {
      return fileInfo.lastModified < threshold;
    };
    return this._deleteLogFiles(shouldDelete);
  }

  removeAllLogs() {
    return this._deleteLogFiles(() => true);
  }

  async _deleteLogFiles(cbShouldDelete) {
    this._cleaningUpFileLogs = true;
    let logDir = lazy.FileUtils.getDir(
      "ProfD",
      this.logFileSubDirectoryEntries
    );
    for (const path of await IOUtils.getChildren(logDir.path)) {
      const name = PathUtils.filename(path);

      if (!name.startsWith("error-") && !name.startsWith("success-")) {
        continue;
      }

      try {
        const info = await IOUtils.stat(path);
        if (!cbShouldDelete(info)) {
          continue;
        }

        this._log.trace(` > Cleanup removing ${name} (${info.lastModified})`);
        await IOUtils.remove(path);
        this._log.trace(`Deleted ${name}`);
      } catch (ex) {
        this._log.debug(
          `Encountered error trying to clean up old log file ${name}`,
          ex
        );
      }
    }
    this._cleaningUpFileLogs = false;
    this._log.debug("Done deleting files.");
    if (this._testTopicPrefix) {
      Services.obs.notifyObservers(
        null,
        `${this._testTopicPrefix}cleanup-logs`
      );
      ("cleanup-logs");
    }
  }
}
