/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


const EXCEPTION_CONSTRUCTORS = {
  EvalError(error) {
    let result = new EvalError(error.message, error.fileName, error.lineNumber);
    result.stack = error.stack;
    return result;
  },
  InternalError(error) {
    let result = new InternalError(
      error.message,
      error.fileName,
      error.lineNumber
    );
    result.stack = error.stack;
    return result;
  },
  RangeError(error) {
    let result = new RangeError(
      error.message,
      error.fileName,
      error.lineNumber
    );
    result.stack = error.stack;
    return result;
  },
  ReferenceError(error) {
    let result = new ReferenceError(
      error.message,
      error.fileName,
      error.lineNumber
    );
    result.stack = error.stack;
    return result;
  },
  SyntaxError(error) {
    let result = new SyntaxError(
      error.message,
      error.fileName,
      error.lineNumber
    );
    result.stack = error.stack;
    return result;
  },
  TypeError(error) {
    let result = new TypeError(error.message, error.fileName, error.lineNumber);
    result.stack = error.stack;
    return result;
  },
  URIError(error) {
    let result = new URIError(error.message, error.fileName, error.lineNumber);
    result.stack = error.stack;
    return result;
  },
  DOMException(error) {
    let result = new DOMException(error.message, error.name);
    return result;
  },
};

export var BasePromiseWorker = function (url, options = {}, functions = {}) {
  if (typeof url != "string") {
    throw new TypeError("Expecting a string");
  }
  this._url = url;
  this._options = options;
  this._functions = functions;

  this.ExceptionHandlers = Object.create(EXCEPTION_CONSTRUCTORS);

  this._deferredJobs = new Map();

  this._deferredJobId = 0;

  this.launchTimeStamp = null;

  this.workerTimeStamps = null;
};

BasePromiseWorker.prototype = {
  log() {
  },

  _generateDeferredJobId() {
    this._deferredJobId += 1;
    return "ThreadToWorker-" + this._deferredJobId;
  },

  get _worker() {
    if (this.__worker) {
      return this.__worker;
    }

    let worker = (this.__worker = new ChromeWorker(this._url, this._options));

    this.launchTimeStamp = Date.now();

    worker.onerror = error => {
      this.log(
        "Received uncaught error from worker",
        error.message,
        error.filename,
        error.lineno
      );

      error.preventDefault();

      if (this._deferredJobs.size > 0) {
        this._deferredJobs.forEach(job => {
          job.deferred.reject(error);
        });
        this._deferredJobs.clear();
      }
    };

    worker.onmessage = msg => {
      let data = msg.data;
      let messageId = data.id;

      this.log(`Received message ${messageId} from worker`);

      if ("timeStamps" in data) {
        this.workerTimeStamps = data.timeStamps;
      }

      if ("fun" in data) {
        if (data.fun in this._functions) {
          Promise.resolve(this._functions[data.fun](...data.args)).then(
            ok => {
              if (ok instanceof BasePromiseWorker.Meta) {
                if ("transfers" in ok.meta) {
                  worker.postMessage(
                    { ok: ok.data, id: messageId },
                    ok.meta.transfers
                  );
                } else {
                  worker.postMessage({ ok: ok.data, id: messageId });
                }
              } else {
                worker.postMessage({ id: messageId, ok });
              }
            },
            fail => {
              worker.postMessage({ id: messageId, fail });
            }
          );
        } else {
          worker.postMessage({
            id: messageId,
            fail: `function ${data.fun} not found`,
          });
        }
        return;
      }

      if (this._deferredJobs.has(messageId)) {
        let handler = this._deferredJobs.get(messageId);
        let deferred = handler.deferred;

        if ("ok" in data) {
          deferred.resolve(data);
        } else if ("fail" in data) {
          deferred.reject(new WorkerError(data.fail));
        }
        this._deferredJobs.delete(messageId);
        return;
      }

      throw new Error(
        `Unexpected message id ${messageId}, data: ${JSON.stringify(data)} `
      );
    };
    return worker;
  },

  post(fun, args, closure, transfers) {
    return async function postMessage() {
      if (args) {
        args = await Promise.resolve(Promise.all(args));
      }
      if (transfers) {
        transfers = await Promise.resolve(Promise.all(transfers));
      } else {
        transfers = [];
      }

      if (args) {
        args = args.map(arg => {
          if (arg instanceof BasePromiseWorker.Meta) {
            if (arg.meta && "transfers" in arg.meta) {
              transfers.push(...arg.meta.transfers);
            }
            return arg.data;
          }
          return arg;
        });
      }

      let id = this._generateDeferredJobId();
      let message = { fun, args, id };
      this.log("Posting message", JSON.stringify(message));
      try {
        this._worker.postMessage(message, ...[transfers]);
      } catch (ex) {
        if (typeof ex == "number") {
          this.log("Could not post message", message, "due to xpcom error", ex);
          throw new Components.Exception("Error in postMessage", ex);
        }

        this.log("Could not post message", message, "due to error", ex);
        throw ex;
      }

      let deferred = Promise.withResolvers();
      this._deferredJobs.set(id, { deferred, closure, id });

      let reply;
      try {
        this.log("Expecting reply");
        reply = await deferred.promise;
      } catch (error) {
        this.log("Got error", JSON.stringify(error));
        reply = error;

        if (error instanceof WorkerError) {
          throw this.ExceptionHandlers[error.data.exn](error.data);
        }

        if (ErrorEvent.isInstance(error)) {
          this.log(
            "Error serialized by DOM",
            error.message,
            error.filename,
            error.lineno
          );
          throw new Error(error.message, error.filename, error.lineno);
        }

        throw error;
      }

      let options = null;
      if (args) {
        options = args[args.length - 1];
      }

      if (
        !options ||
        typeof options !== "object" ||
        !("outExecutionDuration" in options)
      ) {
        return reply.ok;
      }
      if (!("durationMs" in reply)) {
        return reply.ok;
      }
      let durationMs = Math.max(0, reply.durationMs);
      if (typeof options.outExecutionDuration == "number") {
        options.outExecutionDuration += durationMs;
      } else {
        options.outExecutionDuration = durationMs;
      }
      return reply.ok;
    }.bind(this)();
  },

  terminate() {
    if (!this.__worker) {
      return;
    }

    try {
      this.__worker.terminate();
      delete this.__worker;
    } catch (ex) {
      this.log("Error whilst terminating ChromeWorker: " + ex.message);
    }

    if (this._deferredJobs.size) {
      let error = new Error("Internal error: worker terminated");
      this._deferredJobs.forEach(job => {
        job.deferred.reject(error);
      });
      this._deferredJobs.clear();
    }
  },
};

function WorkerError(data) {
  this.data = data;
}

BasePromiseWorker.Meta = function (data, meta) {
  this.data = data;
  this.meta = meta;
};
