/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const Timer = Components.Constructor(
  "@mozilla.org/timer;1",
  "nsITimer",
  "initWithCallback"
);

export class DeferredTask {
  constructor(taskFn, delayMs, idleTimeoutMs) {
    this.#taskFn = taskFn;
    this.#delayMs = delayMs;
    this._idleTimeoutMs = idleTimeoutMs;

    let win = Cu.getGlobalForObject(taskFn);
    if (Window.isInstance(win)) {
      this.#windowGlobalWeakRef = Cu.getWeakReference(win);
    }
  }

  #taskFn;

  #delayMs;

  _idleTimeoutMs = undefined;

  #windowGlobalWeakRef;

  get #windowGlobal() {
    return this.#windowGlobalWeakRef?.get();
  }

  get isArmed() {
    return this.#armed;
  }
  #armed = false;

  get isRunning() {
    return !!this._runningPromise;
  }

  _runningPromise = undefined;

  #timer = null;

  #startTimer() {
    let callback, timer;
    if (this._idleTimeoutMs === 0) {
      callback = () => this.#timerCallback();
    } else {
      callback = () => {
        this._startIdleDispatch(() => {
          if (this.#timer === timer) {
            this.#timerCallback();
          }
        }, this._idleTimeoutMs);
      };
    }
    if (this.#windowGlobalWeakRef) {
      timer = this.#windowGlobal.setTimeout(callback, this.#delayMs);
    } else {
      timer = new Timer(callback, this.#delayMs, Ci.nsITimer.TYPE_ONE_SHOT);
    }
    this.#timer = timer;
  }

  _startIdleDispatch(callback, timeout) {
    if (this.#windowGlobalWeakRef) {
      this.#windowGlobal.requestIdleCallback(callback, { timeout });
    } else {
      ChromeUtils.idleDispatch(callback, { timeout });
    }
  }

  arm() {
    if (this.#finalized) {
      throw new Error("Unable to arm timer, the object has been finalized.");
    }

    this.#armed = true;

    if (!this._runningPromise && !this.#timer) {
      this.#startTimer();
    }
  }

  disarm() {
    this.#armed = false;
    if (this.#timer) {
      if (this.#windowGlobalWeakRef) {
        this.#windowGlobal.clearTimeout(this.#timer);
      } else {
        this.#timer.cancel();
      }
      this.#timer = null;
    }
  }

  finalize() {
    if (this.#finalized) {
      throw new Error("The object has been already finalized.");
    }
    this.#finalized = true;

    if (this.#timer) {
      this.disarm();
      this.#timerCallback();
    }

    if (this._runningPromise) {
      this._runningPromise.then(() => this.#releaseTaskCallback());
      return this._runningPromise;
    }
    this.#releaseTaskCallback();
    return Promise.resolve();
  }
  #finalized = false;

  get isFinalized() {
    return this.#finalized;
  }

  #timerCallback() {
    let runningDeferred = Promise.withResolvers();

    this.#timer = null;
    this.#armed = false;
    this._runningPromise = runningDeferred.promise;

    runningDeferred.resolve(
      (async () => {
        await this.#runTask();

        if (this.#armed) {
          if (!this.#finalized) {
            this.#startTimer();
          } else {
            this.#armed = false;
            await this.#runTask();
          }
        }

        this._runningPromise = null;
      })().catch(console.error)
    );
  }

  async #runTask() {
    try {
      await this.#taskFn();
    } catch (ex) {
      console.error(ex);
    }
  }

  #releaseTaskCallback() {
    this.#taskFn = null;
  }
}
