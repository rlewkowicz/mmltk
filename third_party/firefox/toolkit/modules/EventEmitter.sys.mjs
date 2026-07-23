/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "loggingEnabled",
  "toolkit.dump.emit",
  false
);

function describeNthCaller(n) {
  let caller = Components.stack;
  while (n >= 0) {
    --n;
    caller = caller.caller;
  }

  let func = caller.name;
  let file = caller.filename;
  if (file.includes(" -> ")) {
    file = caller.filename.split(/ -> /)[1];
  }
  let path = file + ":" + caller.lineNumber;

  return func + "() -> " + path;
}

export class EventEmitter {
  static decorate(objectToDecorate) {
    let emitter = new EventEmitter();
    objectToDecorate.on = emitter.on.bind(emitter);
    objectToDecorate.off = emitter.off.bind(emitter);
    objectToDecorate.once = emitter.once.bind(emitter);
    objectToDecorate.emit = emitter.emit.bind(emitter);
  }

  on(event, listener) {
    if (!this._eventEmitterListeners) {
      this._eventEmitterListeners = new Map();
    }
    if (!this._eventEmitterListeners.has(event)) {
      this._eventEmitterListeners.set(event, []);
    }
    this._eventEmitterListeners.get(event).push(listener);
  }

  once(event, listener) {
    return new Promise(resolve => {
      let handler = (_, first, ...rest) => {
        this.off(event, handler);
        if (listener) {
          listener(event, first, ...rest);
        }
        resolve(first);
      };

      handler._originalListener = listener;
      this.on(event, handler);
    });
  }

  off(event, listener) {
    if (!this._eventEmitterListeners) {
      return;
    }
    let listeners = this._eventEmitterListeners.get(event);
    if (listeners) {
      this._eventEmitterListeners.set(
        event,
        listeners.filter(l => {
          return l !== listener && l._originalListener !== listener;
        })
      );
    }
  }

  emit(event, ...args) {
    this.#logEvent(event, args);

    if (
      !this._eventEmitterListeners ||
      !this._eventEmitterListeners.has(event)
    ) {
      return;
    }

    let originalListeners = this._eventEmitterListeners.get(event);
    for (let listener of this._eventEmitterListeners.get(event)) {
      if (!this._eventEmitterListeners) {
        break;
      }

      if (
        originalListeners === this._eventEmitterListeners.get(event) ||
        this._eventEmitterListeners.get(event).some(l => l === listener)
      ) {
        try {
          listener(event, ...args);
        } catch (ex) {
          console.error(ex);
        }
      }
    }
  }

  #logEvent(event, args) {
    if (!lazy.loggingEnabled) {
      return;
    }

    let description = describeNthCaller(2);

    let argOut = "(";
    if (args.length === 1) {
      argOut += event;
    }

    let out = "EMITTING: ";

    try {
      for (let i = 1; i < args.length; i++) {
        if (i === 1) {
          argOut = "(" + event + ", ";
        } else {
          argOut += ", ";
        }

        let arg = args[i];
        argOut += arg;

        if (arg && arg.nodeName) {
          argOut += " (" + arg.nodeName;
          if (arg.id) {
            argOut += "#" + arg.id;
          }
          if (arg.className) {
            argOut += "." + arg.className;
          }
          argOut += ")";
        }
      }
    } catch (e) {
    }

    argOut += ")";
    out += "emit" + argOut + " from " + description + "\n";

    dump(out);
  }
}
