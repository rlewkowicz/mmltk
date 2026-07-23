/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { clearTimeout, setTimeout } from "resource://gre/modules/Timer.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(
  lazy,
  {
    AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
    FileUtils: "resource://gre/modules/FileUtils.sys.mjs",
  },
  { global: "contextual" }
);

var likeSqlRegex = /\bLIKE\b\s(?![@:?])/i;

var carrayParamRegex = /(carray\((?:\?(\d+)|\?|:(\w+))\))|\?(?!\d)/gi;

var connectionCounters = new Map();

var wrappedConnections = new Set();

function isClosed() {
  if (
    typeof Object.getOwnPropertyDescriptor(lazy, "Barriers").get == "function"
  ) {
    return Services.startup.isInOrBeyondShutdownPhase(
      Ci.nsIAppStartup.SHUTDOWN_PHASE_XPCOMWILLSHUTDOWN
    );
  }
  return lazy.Barriers.shutdown.client.isClosed;
}

var Debugging = {
  failTestsOnAutoClose: true,
};

function isInvalidBoundLikeQuery(sql) {
  return likeSqlRegex.test(sql);
}

function logScriptError(message) {
  let consoleMessage = Cc["@mozilla.org/scripterror;1"].createInstance(
    Ci.nsIScriptError
  );
  let stack = new Error();
  consoleMessage.init(
    message,
    stack.fileName,
    stack.lineNumber,
    0,
    Ci.nsIScriptError.errorFlag,
    "component javascript"
  );
  Services.console.logMessage(consoleMessage);

  if (Debugging.failTestsOnAutoClose) {
    Promise.reject(new Error(message));
  }
}

function getIdentifierByFileName(fileName) {
  let number = connectionCounters.get(fileName) || 0;
  connectionCounters.set(fileName, number + 1);
  return fileName + "#" + number;
}

function convertStorageErrorResult(result) {
  switch (result) {
    case Ci.mozIStorageError.PERM:
    case Ci.mozIStorageError.AUTH:
    case Ci.mozIStorageError.CANTOPEN:
      return Cr.NS_ERROR_FILE_ACCESS_DENIED;
    case Ci.mozIStorageError.LOCKED:
      return Cr.NS_ERROR_FILE_IS_LOCKED;
    case Ci.mozIStorageError.READONLY:
      return Cr.NS_ERROR_FILE_READ_ONLY;
    case Ci.mozIStorageError.ABORT:
    case Ci.mozIStorageError.INTERRUPT:
      return Cr.NS_ERROR_ABORT;
    case Ci.mozIStorageError.TOOBIG:
    case Ci.mozIStorageError.FULL:
      return Cr.NS_ERROR_FILE_NO_DEVICE_SPACE;
    case Ci.mozIStorageError.NOMEM:
      return Cr.NS_ERROR_OUT_OF_MEMORY;
    case Ci.mozIStorageError.BUSY:
      return Cr.NS_ERROR_STORAGE_BUSY;
    case Ci.mozIStorageError.CONSTRAINT:
      return Cr.NS_ERROR_STORAGE_CONSTRAINT;
    case Ci.mozIStorageError.NOLFS:
    case Ci.mozIStorageError.IOERR:
      return Cr.NS_ERROR_STORAGE_IOERR;
    case Ci.mozIStorageError.SCHEMA:
    case Ci.mozIStorageError.MISMATCH:
    case Ci.mozIStorageError.MISUSE:
    case Ci.mozIStorageError.RANGE:
      return Cr.NS_ERROR_UNEXPECTED;
    case Ci.mozIStorageError.CORRUPT:
    case Ci.mozIStorageError.EMPTY:
    case Ci.mozIStorageError.FORMAT:
    case Ci.mozIStorageError.NOTADB:
      return Cr.NS_ERROR_FILE_CORRUPTED;
    default:
      return Cr.NS_ERROR_FAILURE;
  }
}
ChromeUtils.defineLazyGetter(lazy, "Barriers", () => {
  let Barriers = {
    shutdown: new lazy.AsyncShutdown.Barrier(
      "Sqlite.sys.mjs: wait until all clients have completed their task"
    ),

    connections: new lazy.AsyncShutdown.Barrier(
      "Sqlite.sys.mjs: wait until all connections are closed"
    ),
  };

  lazy.AsyncShutdown.profileBeforeChange.addBlocker(
    "Sqlite.sys.mjs shutdown blocker",
    async function () {
      await Barriers.shutdown.wait();
      await Barriers.connections.wait();

    },

    function status() {
      if (isClosed()) {
        return {
          description: "Waiting for connections to close",
          state: Barriers.connections.state,
        };
      }

      return {
        description: "Waiting for the barrier to be lifted",
        state: Barriers.shutdown.state,
      };
    }
  );

  return Barriers;
});

const VACUUM_CATEGORY = "vacuum-participant";
const VACUUM_CONTRACTID = "@sqlite.module.js/vacuum-participant;";
var registeredVacuumParticipants = new Map();

function registerVacuumParticipant(connectionData) {
  let contractId = VACUUM_CONTRACTID + connectionData._identifier;
  let factory = {
    createInstance(iid) {
      return connectionData.QueryInterface(iid);
    },
    QueryInterface: ChromeUtils.generateQI(["nsIFactory"]),
  };
  let cid = Services.uuid.generateUUID();
  Components.manager
    .QueryInterface(Ci.nsIComponentRegistrar)
    .registerFactory(cid, contractId, contractId, factory);
  Services.catMan.addCategoryEntry(
    VACUUM_CATEGORY,
    contractId,
    contractId,
    false,
    false
  );
  registeredVacuumParticipants.set(contractId, { cid, factory });
}

function unregisterVacuumParticipant(connectionData) {
  let contractId = VACUUM_CONTRACTID + connectionData._identifier;
  let component = registeredVacuumParticipants.get(contractId);
  if (component) {
    Components.manager
      .QueryInterface(Ci.nsIComponentRegistrar)
      .unregisterFactory(component.cid, component.factory);
    Services.catMan.deleteCategoryEntry(VACUUM_CATEGORY, contractId, false);
  }
}

function createLoggerWithPrefix(prefix) {
  return console.createInstance({
    prefix: `SQLite JSM (${prefix})`,
    maxLogLevelPref: "toolkit.sqlitejsm.loglevel",
  });
}

function ConnectionData(connection, identifier, options = {}) {
  this._logger = createLoggerWithPrefix(`Connection ${identifier}`);
  this._logger.debug("Opened");

  this._dbConn = connection;

  this._identifier = identifier;

  this._open = true;

  this._cachedStatements = new Map();
  this._anonymousStatements = new Map();
  this._anonymousCounter = 0;

  this._pendingStatements = new Map();

  this._statementCounter = 0;

  this._operationsCounter = 0;

  if ("defaultTransactionType" in options) {
    this.defaultTransactionType = options.defaultTransactionType;
  } else {
    this.defaultTransactionType = convertStorageTransactionType(
      this._dbConn.defaultTransactionType
    );
  }
  this._initiatedTransaction = false;
  this._transactionQueue = Promise.resolve();

  this._idleShrinkMS = options.shrinkMemoryOnConnectionIdleMS;
  if (this._idleShrinkMS) {
    this._idleShrinkTimer = Cc["@mozilla.org/timer;1"].createInstance(
      Ci.nsITimer
    );
  }

  this._deferredClose = Promise.withResolvers();
  this._closeRequested = false;

  this._barrier = new lazy.AsyncShutdown.Barrier(
    `${this._identifier}: waiting for clients`
  );

  lazy.Barriers.connections.client.addBlocker(
    this._identifier + ": waiting for shutdown",
    this._deferredClose.promise,
    () => ({
      identifier: this._identifier,
      isCloseRequested: this._closeRequested,
      hasDbConn: !!this._dbConn,
      initiatedTransaction: this._initiatedTransaction,
      pendingStatements: this._pendingStatements.size,
      statementCounter: this._statementCounter,
    })
  );

  this._useIncrementalVacuum = !!options.incrementalVacuum;
  if (this._useIncrementalVacuum) {
    this._logger.debug("Set auto_vacuum INCREMENTAL");
    this.execute("PRAGMA auto_vacuum = 2").catch(ex => {
      this._logger.error("Setting auto_vacuum to INCREMENTAL failed.");
      console.error(ex);
    });
  }

  this._expectedPageSize = options.pageSize ?? 0;
  if (this._expectedPageSize) {
    this._logger.debug("Set page_size to " + this._expectedPageSize);
    this.execute("PRAGMA page_size = " + this._expectedPageSize).catch(ex => {
      this._logger.error(
        `Setting page_size to ${this._expectedPageSize} failed.`
      );
      console.error(ex);
    });
  }

  this._vacuumOnIdle = options.vacuumOnIdle;
  if (this._vacuumOnIdle) {
    this._logger.debug("Register as vacuum participant");
    this.QueryInterface = ChromeUtils.generateQI([
      Ci.mozIStorageVacuumParticipant,
    ]);
    registerVacuumParticipant(this);
  }
}

ConnectionData.byId = new Map();

const connectionFinalizer = new FinalizationRegistry(identifier => {
  let connectionData = ConnectionData.byId.get(identifier);
  if (connectionData === undefined) {
    return;
  }
  ConnectionData.byId.delete(identifier);
  connectionData.close();
});

ConnectionData.prototype = Object.freeze({
  get expectedDatabasePageSize() {
    return this._expectedPageSize;
  },

  get useIncrementalVacuum() {
    return this._useIncrementalVacuum;
  },

  get databaseConnection() {
    if (this._vacuumOnIdle) {
      return this._dbConn;
    }
    return null;
  },

  onBeginVacuum() {
    let granted = !this.transactionInProgress;
    this._logger.debug("Begin Vacuum - " + granted ? "granted" : "denied");
    return granted;
  },

  onEndVacuum(succeeded) {
    this._logger.debug("End Vacuum - " + succeeded ? "success" : "failure");
  },

  executeBeforeShutdown(parent, name, task) {
    if (!name) {
      throw new TypeError("Expected a human-readable name as first argument");
    }
    if (typeof task != "function") {
      throw new TypeError("Expected a function as second argument");
    }
    if (this._closeRequested) {
      throw new Error(
        `${this._identifier}: cannot execute operation ${name}, the connection is already closing`
      );
    }

    let status = {
      command: "<not started>",

      isPending: false,
    };

    let loggedDb = Object.create(parent, {
      execute: {
        value: async (sql, ...rest) => {
          status.isPending = true;
          status.command = sql;
          try {
            return await this.execute(sql, ...rest);
          } finally {
            status.isPending = false;
          }
        },
      },
      close: {
        value: async () => {
          status.isPending = true;
          status.command = "<close>";
          try {
            return await this.close();
          } finally {
            status.isPending = false;
          }
        },
      },
      executeCached: {
        value: async (sql, ...rest) => {
          status.isPending = true;
          status.command = "cached: " + sql;
          try {
            return await this.executeCached(sql, ...rest);
          } finally {
            status.isPending = false;
          }
        },
      },
    });

    let promiseResult = task(loggedDb);
    if (
      !promiseResult ||
      typeof promiseResult != "object" ||
      !("then" in promiseResult)
    ) {
      throw new TypeError("Expected a Promise");
    }
    let key = `${this._identifier}: ${name} (${this._getOperationId()})`;
    let promiseComplete = promiseResult.catch(() => {});
    this._barrier.client.addBlocker(key, promiseComplete, {
      fetchState: () => status,
    });

    return (async () => {
      try {
        return await promiseResult;
      } finally {
        this._barrier.client.removeBlocker(key, promiseComplete);
      }
    })();
  },
  close() {
    this._closeRequested = true;

    if (!this._dbConn) {
      return this._deferredClose.promise;
    }

    this._logger.debug("Request to close connection.");
    this._clearIdleShrinkTimer();

    if (this._vacuumOnIdle) {
      this._logger.debug("Unregister as vacuum participant");
      unregisterVacuumParticipant(this);
    }

    return this._barrier.wait().then(() => {
      if (!this._dbConn) {
        return undefined;
      }
      return this._finalize();
    });
  },

  clone(readOnly = false) {
    this.ensureOpen();

    this._logger.debug("Request to clone connection.");

    let options = {
      connection: this._dbConn,
      readOnly,
    };
    if (this._idleShrinkMS) {
      options.shrinkMemoryOnConnectionIdleMS = this._idleShrinkMS;
    }

    return cloneStorageConnection(options);
  },
  _getOperationId() {
    return this._operationsCounter++;
  },
  _finalize() {
    this._logger.debug("Finalizing connection.");
    for (let [,  statement] of this._pendingStatements) {
      statement.cancel();
    }
    this._pendingStatements.clear();

    this._statementCounter = 0;

    for (let [,  statement] of this._anonymousStatements) {
      statement.finalize();
    }
    this._anonymousStatements.clear();

    for (let [,  statement] of this._cachedStatements) {
      statement.finalize();
    }
    this._cachedStatements.clear();

    this._open = false;

    let markAsClosed = () => {
      this._logger.debug("Closed");
      lazy.Barriers.connections.client.removeBlocker(
        this._deferredClose.promise
      );
      this._deferredClose.resolve();
    };
    if (wrappedConnections.has(this._identifier)) {
      wrappedConnections.delete(this._identifier);
      this._dbConn = null;
      markAsClosed();
    } else {
      this._logger.debug("Calling asyncClose().");
      try {
        this._dbConn.asyncClose(markAsClosed);
      } catch (ex) {
        markAsClosed();
      } finally {
        this._dbConn = null;
      }
    }
    return this._deferredClose.promise;
  },

  executeCached(sql, params = null, onRow = null) {
    this.ensureOpen();

    if (!sql) {
      throw new Error("sql argument is empty.");
    }

    let statement = this._cachedStatements.get(sql);
    if (!statement) {
      statement = this._dbConn.createAsyncStatement(sql);
      this._cachedStatements.set(sql, statement);
    }

    this._clearIdleShrinkTimer();

    return new Promise((resolve, reject) => {
      try {
        this._executeStatement(sql, statement, params, onRow).then(
          result => {
            this._startIdleShrinkTimer();
            resolve(result);
          },
          error => {
            this._startIdleShrinkTimer();
            reject(error);
          }
        );
      } catch (ex) {
        this._startIdleShrinkTimer();
        throw ex;
      }
    });
  },

  execute(sql, params = null, onRow = null) {
    if (typeof sql != "string") {
      throw new Error("Must define SQL to execute as a string: " + sql);
    }

    this.ensureOpen();

    let statement = this._dbConn.createAsyncStatement(sql);
    let index = this._anonymousCounter++;

    this._anonymousStatements.set(index, statement);
    this._clearIdleShrinkTimer();

    let onFinished = () => {
      this._anonymousStatements.delete(index);
      statement.finalize();
      this._startIdleShrinkTimer();
    };

    return new Promise((resolve, reject) => {
      try {
        this._executeStatement(sql, statement, params, onRow).then(
          rows => {
            onFinished();
            resolve(rows);
          },
          error => {
            onFinished();
            reject(error);
          }
        );
      } catch (ex) {
        onFinished();
        throw ex;
      }
    });
  },

  get transactionInProgress() {
    return this._open && this._dbConn.transactionInProgress;
  },

  executeTransaction(func, type) {
    let caller = new Error().stack
      .split("\n", 3)
      .pop()
      .match(/^([^@]*@).*\/([^\/:]+)[:0-9]*$/);
    caller = caller[1] + caller[2];
    this._logger.debug(`Transaction (type ${type}) requested by: ${caller}`);

    if (type == OpenedConnection.prototype.TRANSACTION_DEFAULT) {
      type = this.defaultTransactionType;
    } else if (!OpenedConnection.TRANSACTION_TYPES.includes(type)) {
      throw new Error("Unknown transaction type: " + type);
    }
    this.ensureOpen();

    let promise = this._transactionQueue.then(() => {
      if (this._closeRequested) {
        throw new Error("Transaction canceled due to a closed connection.");
      }
      let timeoutId;
      let timeoutPromise = new Promise((_, reject) => {
        timeoutId = setTimeout(() => {
          let e = new Error(
            "Transaction timeout, most likely caused by unresolved pending work."
          );
          e.becauseTimedOut = true;
          reject(e);
        }, Sqlite.TRANSACTIONS_TIMEOUT_MS);
      });

      let transactionPromise = (async () => {
        if (this._initiatedTransaction) {
          this._logger.error(
            "Unexpected transaction in progress when trying to start a new one."
          );
        }
        try {
          try {
            await this.execute("BEGIN " + type + " TRANSACTION");
            this._logger.debug(`Begin transaction`);
            this._initiatedTransaction = true;
          } catch (ex) {
            if (wrappedConnections.has(this._identifier)) {
              this._logger.warn(
                "A new transaction could not be started cause the wrapped connection had one in progress",
                ex
              );
            } else {
              this._logger.warn(
                "A transaction was already in progress, likely a nested transaction",
                ex
              );
              throw ex;
            }
          }

          let result;
          try {
            result = await Promise.race([func(), timeoutPromise]);
          } catch (ex) {
            if (this._closeRequested) {
              this._logger.warn(
                "Connection closed while performing a transaction",
                ex
              );
            } else {
              if (ex.becauseTimedOut) {
                let caller_module = caller.split(":", 1)[0];
                this._logger.error(
                  `The transaction requested by ${caller} timed out. Rolling back`,
                  ex
                );
              } else {
                this._logger.error(
                  `Error during transaction requested by ${caller}. Rolling back`,
                  ex
                );
              }
              if (this._initiatedTransaction) {
                try {
                  await this.execute("ROLLBACK TRANSACTION");
                  this._initiatedTransaction = false;
                  this._logger.debug(`Roll back transaction`);
                } catch (inner) {
                  this._logger.error("Could not roll back transaction", inner);
                }
              }
            }
            throw ex;
          }

          if (this._closeRequested) {
            this._logger.warn(
              "Connection closed before committing the transaction."
            );
            throw new Error(
              "Connection closed before committing the transaction."
            );
          }

          if (this._initiatedTransaction) {
            try {
              await this.execute("COMMIT TRANSACTION");
              this._logger.debug(`Commit transaction`);
            } catch (ex) {
              this._logger.warn("Error committing transaction", ex);
              throw ex;
            }
          }

          return result;
        } finally {
          this._initiatedTransaction = false;
          clearTimeout(timeoutId);
        }
      })();

      return Promise.race([transactionPromise, timeoutPromise]);
    });
    this._transactionQueue = promise.catch(ex => {
      this._logger.error(ex);
    });

    this._barrier.client.addBlocker(
      `Transaction (${this._getOperationId()})`,
      this._transactionQueue
    );
    return promise;
  },

  shrinkMemory() {
    this._logger.debug("Shrinking memory usage.");
    return this.execute("PRAGMA shrink_memory").finally(() => {
      this._clearIdleShrinkTimer();
    });
  },

  discardCachedStatements() {
    let count = 0;
    for (let [,  statement] of this._cachedStatements) {
      ++count;
      statement.finalize();
    }
    this._cachedStatements.clear();
    this._logger.debug("Discarded " + count + " cached statements.");
    return count;
  },

  interrupt() {
    this._logger.debug("Trying to interrupt.");
    this.ensureOpen();
    this._dbConn.interrupt();
  },

  _parseCarrayParams(sql) {
    if (!sql.includes("carray(") && !sql.includes("CARRAY(")) {
      return null;
    }
    const carrayParams = new Set();
    let largestIdx = 0;
    for (const m of sql.matchAll(carrayParamRegex)) {
      if (m[1]) {
        if (m[2]) {
          const n = +m[2];
          largestIdx = Math.max(largestIdx, n);
          carrayParams.add(n - 1);
        } else if (m[3]) {
          carrayParams.add(m[3]);
        } else {
          carrayParams.add(largestIdx);
          largestIdx++;
        }
      } else {
        largestIdx++;
      }
    }
    return carrayParams;
  },

  _bindParam(obj, key, val, carrayParams) {
    const bindMethod = typeof key == "number" ? "Index" : "Name";
    if (Array.isArray(val)) {
      if (!carrayParams || !carrayParams.has(key)) {
        throw new Error("Array parameters require carray()");
      }
      if (!val.length) {
        throw new Error("Array must not be empty");
      }
      let finalType, lastSeenType;
      for (const v of val) {
        const type = typeof v;
        if (type != "string" && type != "number") {
          throw new Error(`Unsupported array element type: ${type}`);
        }
        if (lastSeenType && type != lastSeenType) {
          throw new Error("All array elements must be of the same type");
        }
        if (type == "number" && !isFinite(v)) {
          throw new Error("Array elements must be finite numbers");
        }
        lastSeenType = type;
        if (type == "string") {
          finalType = "Strings";
        } else if (finalType != "Doubles") {
          finalType = Number.isInteger(v) ? "Integers" : "Doubles";
        }
      }
      obj[`bindArrayOf${finalType}By${bindMethod}`](key, val);
      return;
    }

    if (carrayParams?.has(key)) {
      throw new Error("carray() parameters must be bound to an array");
    }

    const isBlob =
      val &&
      typeof val == "object" &&
      ["Uint8Array", "Uint8ClampedArray"].includes(val.constructor.name);
    const args = [key, val];
    if (isBlob) {
      args.push(val.length);
    }
    obj[`bind${isBlob ? "Blob" : ""}By${bindMethod}`](...args);
  },

  _bindParameters(statement, params, sql) {
    if (!params) {
      return;
    }

    const carrayParams = this._parseCarrayParams(sql);

    if (Array.isArray(params)) {
      if (
        params.length &&
        typeof params[0] == "object" &&
        params[0] !== null &&
        !Array.isArray(params[0])
      ) {
        let paramsArray = statement.newBindingParamsArray();
        for (let p of params) {
          let bindings = paramsArray.newBindingParams();
          for (let [key, value] of Object.entries(p)) {
            this._bindParam(bindings, key, value, carrayParams);
          }
          paramsArray.addParams(bindings);
        }

        statement.bindParameters(paramsArray);
        return;
      }

      for (let i = 0; i < params.length; i++) {
        this._bindParam(statement, i, params[i], carrayParams);
      }
      return;
    }

    if (params && typeof params == "object") {
      for (let k in params) {
        this._bindParam(statement, k, params[k], carrayParams);
      }
      return;
    }

    throw new Error(
      "Invalid type for bound parameters. Expected Array or " +
        "object. Got: " +
        params
    );
  },

  _executeStatement(sql, statement, params, onRow) {
    if (statement.state != statement.MOZ_STORAGE_STATEMENT_READY) {
      throw new Error("Statement is not ready for execution.");
    }

    if (onRow && typeof onRow != "function") {
      throw new Error("onRow must be a function. Got: " + onRow);
    }

    this._bindParameters(statement, params, sql);

    let index = this._statementCounter++;

    let deferred = Promise.withResolvers();
    let userCancelled = false;
    let errors = [];
    let rows = [];
    let handledRow = false;

    if (this._logger.shouldLog("Trace")) {
      let msg = "Stmt #" + index + " " + sql;

      if (params) {
        msg += " - " + JSON.stringify(params);
      }
      this._logger.trace(msg);
    } else {
      this._logger.debug("Stmt #" + index + " starting");
    }

    let self = this;
    let pending = statement.executeAsync({
      handleResult(resultSet) {
        for (
          let row = resultSet.getNextRow();
          row && !userCancelled;
          row = resultSet.getNextRow()
        ) {
          if (!onRow) {
            rows.push(row);
            continue;
          }

          handledRow = true;

          try {
            onRow(row, () => {
              userCancelled = true;
              pending.cancel();
            });
          } catch (e) {
            self._logger.warn("Exception when calling onRow callback", e);
          }
        }
      },

      handleError(error) {
        self._logger.warn(
          "Error when executing SQL (" + error.result + "): " + error.message
        );
        errors.push(error);
      },

      handleCompletion(reason) {
        self._logger.debug("Stmt #" + index + " finished.");
        self._pendingStatements.delete(index);

        switch (reason) {
          case Ci.mozIStorageStatementCallback.REASON_FINISHED:
          case Ci.mozIStorageStatementCallback.REASON_CANCELED: {
            let result = onRow ? handledRow : rows;
            deferred.resolve(result);
            break;
          }
          case Ci.mozIStorageStatementCallback.REASON_ERROR: {
            let error = new Error(
              "Error(s) encountered during statement execution: " +
                errors.map(e => e.message).join(", ")
            );
            error.errors = errors;

            if (errors.some(e => e.result == Ci.mozIStorageError.CORRUPT)) {
              error.result = Cr.NS_ERROR_FILE_CORRUPTED;
            } else {
              error.result = convertStorageErrorResult(errors[0]?.result);
            }

            deferred.reject(error);
            break;
          }
          default:
            deferred.reject(
              new Error("Unknown completion reason code: " + reason)
            );
            break;
        }
      },
    });

    this._pendingStatements.set(index, pending);
    return deferred.promise;
  },

  ensureOpen() {
    if (!this._open) {
      throw new Error("Connection is not open.");
    }
  },

  _clearIdleShrinkTimer() {
    if (!this._idleShrinkTimer) {
      return;
    }

    this._idleShrinkTimer.cancel();
  },

  _startIdleShrinkTimer() {
    if (!this._idleShrinkTimer) {
      return;
    }

    this._idleShrinkTimer.initWithCallback(
      this.shrinkMemory.bind(this),
      this._idleShrinkMS,
      this._idleShrinkTimer.TYPE_ONE_SHOT
    );
  },

  async backupToFile(destFilePath, pagesPerStep = 0, stepDelayMs = 0) {
    if (!this._dbConn) {
      return Promise.reject(
        new Error("No opened database connection to create a backup from.")
      );
    }
    let destFile = await IOUtils.getFile(destFilePath);
    return new Promise((resolve, reject) => {
      this._dbConn.backupToFileAsync(
        destFile,
        result => {
          if (Components.isSuccessCode(result)) {
            resolve();
          } else {
            reject(result);
          }
        },
        pagesPerStep,
        stepDelayMs
      );
    });
  },

  attachDatabase(path, name) {
    let deferred = Promise.withResolvers();
    let index = this._statementCounter++;
    let self = this;
    let errors = [];

    let pending = this._dbConn.attachDatabase(path, name, {
      handleResult(_result) {},
      handleError(error) {
        self._logger.warn(
          "Error when attaching database (" +
            error.result +
            "): " +
            error.message
        );
        errors.push(error);
      },
      handleCompletion(reason) {
        self._logger.debug("attachDatabase #" + index + " finished.");
        self._pendingStatements.delete(index);

        switch (reason) {
          case Ci.mozIStorageStatementCallback.REASON_FINISHED:
          case Ci.mozIStorageStatementCallback.REASON_CANCELED:
            deferred.resolve(reason);
            break;
          case Ci.mozIStorageStatementCallback.REASON_ERROR: {
            let error = new Error(
              "Error(s) encountered during ATTACH DATABASE: " +
                errors.map(e => e.message).join(", ")
            );
            error.errors = errors;
            error.result = convertStorageErrorResult(errors[0]?.result);
            deferred.reject(error);
            break;
          }
          default:
            deferred.reject(
              new Error("Unknown completion reason code: " + reason)
            );
            break;
        }
      },
    });

    this._pendingStatements.set(index, pending);
    return deferred.promise;
  },
});

function openConnection(options) {
  let logger = createLoggerWithPrefix("ConnectionOpener");

  if (!options.path) {
    throw new Error("path not specified in connection options.");
  }

  if (isClosed()) {
    throw new Error(
      "Sqlite.sys.mjs has been shutdown. Cannot open connection to: " +
        options.path
    );
  }

  let path = options.path;
  let file;
  try {
    file = lazy.FileUtils.File(path);
  } catch (ex) {
    if (ex.result == Cr.NS_ERROR_FILE_UNRECOGNIZED_PATH) {
      path = PathUtils.joinRelative(
        Services.dirsvc.get("ProfD", Ci.nsIFile).path,
        options.path
      );
      file = lazy.FileUtils.File(path);
    } else {
      throw ex;
    }
  }

  let sharedMemoryCache =
    "sharedMemoryCache" in options ? options.sharedMemoryCache : true;

  let openedOptions = {};

  if ("shrinkMemoryOnConnectionIdleMS" in options) {
    if (!Number.isInteger(options.shrinkMemoryOnConnectionIdleMS)) {
      throw new Error(
        "shrinkMemoryOnConnectionIdleMS must be an integer. " +
          "Got: " +
          options.shrinkMemoryOnConnectionIdleMS
      );
    }

    openedOptions.shrinkMemoryOnConnectionIdleMS =
      options.shrinkMemoryOnConnectionIdleMS;
  }

  if ("defaultTransactionType" in options) {
    let defaultTransactionType = options.defaultTransactionType;
    if (!OpenedConnection.TRANSACTION_TYPES.includes(defaultTransactionType)) {
      throw new Error(
        "Unknown default transaction type: " + defaultTransactionType
      );
    }

    openedOptions.defaultTransactionType = defaultTransactionType;
  }

  if ("vacuumOnIdle" in options) {
    if (typeof options.vacuumOnIdle != "boolean") {
      throw new Error("Invalid vacuumOnIdle: " + options.vacuumOnIdle);
    }
    openedOptions.vacuumOnIdle = options.vacuumOnIdle;
  }

  if ("incrementalVacuum" in options) {
    if (typeof options.incrementalVacuum != "boolean") {
      throw new Error(
        "Invalid incrementalVacuum: " + options.incrementalVacuum
      );
    }
    openedOptions.incrementalVacuum = options.incrementalVacuum;
  }

  if ("pageSize" in options) {
    if (
      ![512, 1024, 2048, 4096, 8192, 16384, 32768, 65536].includes(
        options.pageSize
      )
    ) {
      throw new Error("Invalid pageSize: " + options.pageSize);
    }
    openedOptions.pageSize = options.pageSize;
  }

  let identifier = getIdentifierByFileName(PathUtils.filename(path));

  logger.debug("Opening database: " + path + " (" + identifier + ")");

  return new Promise((resolve, reject) => {
    let dbOpenOptions = Ci.mozIStorageService.OPEN_DEFAULT;
    if (sharedMemoryCache) {
      dbOpenOptions |= Ci.mozIStorageService.OPEN_SHARED;
    }
    if (options.readOnly) {
      dbOpenOptions |= Ci.mozIStorageService.OPEN_READONLY;
    }
    if (options.ignoreLockingMode) {
      dbOpenOptions |= Ci.mozIStorageService.OPEN_IGNORE_LOCKING_MODE;
      dbOpenOptions |= Ci.mozIStorageService.OPEN_READONLY;
    }
    if (options.openNotExclusive) {
      dbOpenOptions |= Ci.mozIStorageService.OPEN_NOT_EXCLUSIVE;
    }

    let dbConnectionOptions = Ci.mozIStorageService.CONNECTION_DEFAULT;

    Services.storage.openAsyncDatabase(
      file,
      dbOpenOptions,
      dbConnectionOptions,
      async (status, connection) => {
        if (!connection) {
          logger.error(`Could not open connection to ${path}: ${status}`);
          let error = new Components.Exception(
            `Could not open connection to ${path}: ${status}`,
            status
          );
          reject(error);
          return;
        }

        logger.debug("Connection opened");
        connection.QueryInterface(Ci.mozIStorageAsyncConnection);

        if (options.testDelayedOpenPromise) {
          await options.testDelayedOpenPromise;
        }

        if (options.extensions) {
          for (let extension of options.extensions) {
            try {
              await new Promise((resolve2, reject2) =>
                connection.loadExtension(extension, rv => {
                  if (Components.isSuccessCode(rv)) {
                    resolve2();
                  } else {
                    reject2(rv);
                  }
                })
              );
            } catch (ex) {
              logger.error(`Could not load extension '${extension}'`, ex);
              connection.asyncClose();
              reject(
                new Error(`Could not load extension '${extension}'`, {
                  cause: ex,
                })
              );
              return;
            }
          }
        }

        if (isClosed()) {
          connection.asyncClose();
          reject(
            new Error(
              "Sqlite.sys.mjs has been shutdown. Cannot open connection to: " +
                options.path
            )
          );
          return;
        }

        try {
          resolve(new OpenedConnection(connection, identifier, openedOptions));
        } catch (ex) {
          logger.error("Could not open database", ex);
          connection.asyncClose();
          reject(ex);
        }
      }
    );
  });
}

function cloneStorageConnection(options) {
  let logger = createLoggerWithPrefix("ConnectionCloner");

  let source = options && options.connection;
  if (!source) {
    throw new TypeError("connection not specified in clone options.");
  }
  if (!(source instanceof Ci.mozIStorageAsyncConnection)) {
    throw new TypeError("Connection must be a valid Storage connection.");
  }

  if (isClosed()) {
    throw new Error(
      "Sqlite.sys.mjs has been shutdown. Cannot clone connection to: " +
        source.databaseFile.path
    );
  }

  let openedOptions = {};

  if ("shrinkMemoryOnConnectionIdleMS" in options) {
    if (!Number.isInteger(options.shrinkMemoryOnConnectionIdleMS)) {
      throw new TypeError(
        "shrinkMemoryOnConnectionIdleMS must be an integer. " +
          "Got: " +
          options.shrinkMemoryOnConnectionIdleMS
      );
    }
    openedOptions.shrinkMemoryOnConnectionIdleMS =
      options.shrinkMemoryOnConnectionIdleMS;
  }

  let path = source.databaseFile.path;
  let identifier = getIdentifierByFileName(PathUtils.filename(path));

  logger.debug("Cloning database: " + path + " (" + identifier + ")");

  return new Promise((resolve, reject) => {
    source.asyncClone(!!options.readOnly, (status, connection) => {
      if (!connection) {
        logger.error("Could not clone connection: " + status);
        reject(new Error("Could not clone connection: " + status));
        return;
      }
      logger.debug("Connection cloned");

      if (isClosed()) {
        connection.QueryInterface(Ci.mozIStorageAsyncConnection).asyncClose();
        reject(
          new Error(
            "Sqlite.sys.mjs has been shutdown. Cannot open connection to: " +
              options.path
          )
        );
        return;
      }

      try {
        let conn = connection.QueryInterface(Ci.mozIStorageAsyncConnection);
        resolve(new OpenedConnection(conn, identifier, openedOptions));
      } catch (ex) {
        logger.error("Could not clone database", ex);
        connection.asyncClose();
        reject(ex);
      }
    });
  });
}

function wrapStorageConnection(options) {
  let logger = createLoggerWithPrefix("ConnectionWrapper");

  let connection = options && options.connection;
  if (!connection || !(connection instanceof Ci.mozIStorageAsyncConnection)) {
    throw new TypeError("connection not specified or invalid.");
  }

  if (isClosed()) {
    throw new Error(
      "Sqlite.sys.mjs has been shutdown. Cannot wrap connection to: " +
        connection.databaseFile.path
    );
  }

  let identifier = getIdentifierByFileName(connection.databaseFile.leafName);

  logger.debug("Wrapping database: " + identifier);
  return new Promise(resolve => {
    try {
      let conn = connection.QueryInterface(Ci.mozIStorageAsyncConnection);
      let wrapper = new OpenedConnection(conn, identifier);
      wrappedConnections.add(identifier);
      resolve(wrapper);
    } catch (ex) {
      logger.error("Could not wrap database", ex);
      throw ex;
    }
  });
}

export function OpenedConnection(connection, identifier, options = {}) {
  this._connectionData = new ConnectionData(connection, identifier, options);

  ConnectionData.byId.set(
    this._connectionData._identifier,
    this._connectionData
  );

  connectionFinalizer.register(
    this,
    this._connectionData._identifier,
    this
  );
}

OpenedConnection.TRANSACTION_TYPES = ["DEFERRED", "IMMEDIATE", "EXCLUSIVE"];

function convertStorageTransactionType(type) {
  if (!(type in OpenedConnection.TRANSACTION_TYPES)) {
    throw new Error("Unknown storage transaction type: " + type);
  }
  return OpenedConnection.TRANSACTION_TYPES[type];
}

OpenedConnection.prototype = {
  TRANSACTION_DEFAULT: "DEFAULT",
  TRANSACTION_DEFERRED: "DEFERRED",
  TRANSACTION_IMMEDIATE: "IMMEDIATE",
  TRANSACTION_EXCLUSIVE: "EXCLUSIVE",

  get unsafeRawConnection() {
    return this._connectionData._dbConn;
  },

  get variableLimit() {
    return this.unsafeRawConnection.variableLimit;
  },

  set variableLimit(newLimit) {
    this.unsafeRawConnection.variableLimit = newLimit;
  },

  getSchemaVersion(schemaName = "main") {
    return this.execute(`PRAGMA ${schemaName}.user_version`).then(result =>
      result[0].getInt32(0)
    );
  },

  setSchemaVersion(value, schemaName = "main") {
    if (!Number.isInteger(value)) {
      throw new TypeError("Schema version must be an integer. Got " + value);
    }
    this._connectionData.ensureOpen();
    return this.execute(`PRAGMA ${schemaName}.user_version = ${value}`);
  },

  close() {
    if (ConnectionData.byId.has(this._connectionData._identifier)) {
      ConnectionData.byId.delete(this._connectionData._identifier);
      connectionFinalizer.unregister(this);
    }
    return this._connectionData.close();
  },

  clone(readOnly = false) {
    return this._connectionData.clone(readOnly);
  },

  executeBeforeShutdown(name, task) {
    return this._connectionData.executeBeforeShutdown(this, name, task);
  },

  executeCached(sql, params = null, onRow = null) {
    if (isInvalidBoundLikeQuery(sql)) {
      throw new Error("Please enter a LIKE clause with bindings");
    }
    return this._connectionData.executeCached(sql, params, onRow);
  },

  execute(sql, params = null, onRow = null) {
    if (isInvalidBoundLikeQuery(sql)) {
      throw new Error("Please enter a LIKE clause with bindings");
    }
    return this._connectionData.execute(sql, params, onRow);
  },

  get defaultTransactionType() {
    return this._connectionData.defaultTransactionType;
  },

  get transactionInProgress() {
    return this._connectionData.transactionInProgress;
  },

  executeTransaction(func, type = this.TRANSACTION_DEFAULT) {
    return this._connectionData.executeTransaction(() => func(this), type);
  },

  tableExists(name) {
    return this.execute(
      "SELECT name FROM (SELECT * FROM sqlite_master UNION ALL " +
        "SELECT * FROM sqlite_temp_master) " +
        "WHERE type = 'table' AND name=?",
      [name]
    ).then(function onResult(rows) {
      return Promise.resolve(!!rows.length);
    });
  },

  indexExists(name) {
    return this.execute(
      "SELECT name FROM (SELECT * FROM sqlite_master UNION ALL " +
        "SELECT * FROM sqlite_temp_master) " +
        "WHERE type = 'index' AND name=?",
      [name]
    ).then(function onResult(rows) {
      return Promise.resolve(!!rows.length);
    });
  },

  shrinkMemory() {
    return this._connectionData.shrinkMemory();
  },

  discardCachedStatements() {
    return this._connectionData.discardCachedStatements();
  },

  interrupt() {
    this._connectionData.interrupt();
  },

  backup(destFilePath, pagesPerStep = 0, stepDelayMs = 0) {
    return this._connectionData.backupToFile(
      destFilePath,
      pagesPerStep,
      stepDelayMs
    );
  },

  attachDatabase(path, name) {
    return this._connectionData.attachDatabase(path, name);
  },
};
OpenedConnection.prototype = Object.freeze(OpenedConnection.prototype);

export var Sqlite = {
  TRANSACTIONS_TIMEOUT_MS: 300000, 

  openConnection,
  cloneStorageConnection,
  wrapStorageConnection,
  get shutdown() {
    return lazy.Barriers.shutdown.client;
  },
  failTestsOnAutoClose(enabled) {
    Debugging.failTestsOnAutoClose = enabled;
  },
};
