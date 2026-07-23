/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const topics = ["wake_notification", "sleep_notification"];

export class ScheduledTask {
  constructor(callback, epochMilliseconds) {
    this.epochMilliseconds = epochMilliseconds;
    this.armed = false;
    this.timer = null;
    this.callback = callback;
    this.promise = Promise.resolve();
  }

  async _callbackHandler() {
    try {
      await this.callback();
      this.resolve();
    } catch (err) {
      this.reject(err);
    } finally {
      this._disableTask();
    }
  }

  _createTimer() {
    const delay = this.epochMilliseconds - Date.now();
    if (delay >= 0) {
      const newTimer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
      newTimer.initWithCallback(
        () => {
          this._callbackHandler();
        },
        delay,
        Ci.nsITimer.TYPE_ONE_SHOT
      );
      return newTimer;
    }
    ChromeUtils.idleDispatch(() => {
      this._callbackHandler();
    });
    return null;
  }

  _destroyTimer() {
    if (this.timer) {
      this.timer.cancel();
      this.timer = null;
    }
  }

  observe(_subject, topic, _data) {
    switch (topic) {
      case "sleep_notification":
        if (this.armed && this.timer) {
          this._destroyTimer();
        }
        break;
      case "wake_notification":
        if (this.armed && !this.timer) {
          this.timer = this._createTimer();
        }
        break;
    }
  }

  _enableObservers() {
    topics.forEach(topic => {
      Services.obs.addObserver(this, topic);
    });
  }

  _disableObservers() {
    topics.forEach(topic => {
      Services.obs.removeObserver(this, topic);
    });
  }

  _disableTask() {
    if (this.armed) {
      this._destroyTimer();
      this._disableObservers();
    }
  }

  arm() {
    if (!this.armed) {
      const { promise, resolve, reject } = Promise.withResolvers();
      this.promise = promise;
      this.resolve = resolve;
      this.reject = reject;

      this._enableObservers();
      this.armed = true;
      this.timer = this._createTimer();
    }
    return this; 
  }

  disarm() {
    if (this.armed) {
      this.resolve();
      this._disableTask();
      this.armed = false;
    }
    return this; 
  }

  get isArmed() {
    return this.armed;
  }

  asPromise() {
    return this.promise;
  }
}
