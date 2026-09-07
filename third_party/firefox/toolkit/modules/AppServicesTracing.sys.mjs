/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
  // eslint-disable-next-line mozilla/use-console-createInstance
  Log: "resource://gre/modules/Log.sys.mjs",
});

import {
  registerEventSink,
  unregisterEventSink,
  EventSink,
  EventSinkSpecification,
  EventTarget,
  TracingLevel,
} from "moz-src:///toolkit/components/uniffi-bindgen-gecko-js/components/generated/RustTracing.sys.mjs";

ChromeUtils.defineLazyGetter(lazy, "console", () =>
  console.createInstance({
    prefix: "AppServices",
    maxLogLevelPref: "toolkit.rust-components.logging.internal-level",
  })
);

class CallbackList {
  items = [];

  maxLevel() {
    return Math.max(...this.items.map(i => i.level));
  }

  add(level, callback) {
    const index = this.items.findIndex(item => item.callback === callback);
    if (index == -1) {
      this.items.push({ level, callback });
    } else {
      this.items[index].level = level;
    }
  }

  remove(callback) {
    const index = this.items.find(i => i.callback === callback);
    if (index === undefined) {
      lazy.console.trace(
        "ignoring attempt to remove an event handler that's not registered"
      );
    }
    this.items.splice(index, 1);
  }

  processEvent(event) {
    for (const item of this.items) {
      if (item.level >= event.level) {
        try {
          item.callback(event);
        } catch (e) {
          lazy.console.error("tracing callback failed", item.callback, e);
        }
      }
    }
  }
}

class TracingEventHandler extends EventSink {
  constructor() {
    super();
    this.targetCallbackLists = new Map();
    this.minLevelCallbackList = new CallbackList();
    this.eventSinkId = null;

    if (lazy.AsyncShutdown.profileBeforeChange.isClosed) {
      this.#close();
    } else {
      lazy.AsyncShutdown.profileBeforeChange.addBlocker(
        "TracingEventHandler: deregister callbacks",
        () => this.#close()
      );
    }
  }

  register(target, level, callback) {
    if (this.targetCallbackLists === null) {
      lazy.console.trace(
        "ignoring attempt to register event handler after shutdown has commenced"
      );
      return;
    }
    const callbackList = this._getTargetList(target);
    callbackList.add(level, callback);
  }

  deregister(target, callback) {
    if (this.targetCallbackLists === null) {
      lazy.console.trace(
        "ignoring attempt to register event handler after shutdown has commenced"
      );
      return;
    }
    const callbackList = this._getTargetList(target);
    callbackList.remove(callback);
  }

  registerMinLevelEventSink(level, callback) {
    if (this.minLevelCallbackList === null) {
      lazy.console.trace(
        "ignoring attempt to register min-level event handler after shutdown has commenced"
      );
      return;
    }

    this.minLevelCallbackList.add(level, callback);
  }

  unregisterMinLevelEventSink(callback) {
    if (this.minLevelCallbackList === null) {
      lazy.console.trace(
        "ignoring attempt to unregister min-level event handler after shutdown has commenced"
      );
      return;
    }

    this.minLevelCallbackList.remove(callback);
  }

  _getTargetList(target) {
    if (!this.targetCallbackLists.has(target)) {
      this.targetCallbackLists.set(target, new CallbackList());
    }
    return this.targetCallbackLists.get(target);
  }

  onEvent(event) {
    let target = targetRoot(event.target);
    let targetList = this.targetCallbackLists.get(target);
    if (targetList) {
      targetList.processEvent(event);
    }
    this.minLevelCallbackList.processEvent(event);
  }

  updateRustTracingRegistration() {
    let minLevel = this.minLevelCallbackList.maxLevel();
    if (minLevel == -Infinity) {
      minLevel = null;
    }
    const spec = new EventSinkSpecification({
      targets: [
        ...this.targetCallbackLists.entries().map(([target, callbackList]) => {
          let level = callbackList.maxLevel();
          if (level == -Infinity) {
            level = TracingLevel.DEBUG;
          }
          return new EventTarget({ target, level });
        }),
      ],
      minLevel,
    });
    this.#unregisterWithRustTracing();
    if (spec.minLevel !== null || spec.targets.length) {
      lazy.console.trace("calling registerEventSink", spec);
      this.eventSinkId = registerEventSink(spec, this);
    } else {
      lazy.console.trace(
        "skipping registerEventSink, since there are callbacks"
      );
    }
  }

  #unregisterWithRustTracing() {
    if (this.eventSinkId !== null) {
      lazy.console.trace("calling unregisterEventSink", this.eventSinkId);
      unregisterEventSink(this.eventSinkId);
      this.eventSinkId = null;
    }
  }

  #close() {
    this.#unregisterWithRustTracing();
    this.targetCallbackLists = null;
    this.minLevelCallbackList = null;
  }
}

let tracingEventHandler = new TracingEventHandler();

function targetRoot(target) {
  let colonIndex = target.indexOf(":");
  if (colonIndex > 0) {
    return target.slice(0, colonIndex);
  }
  return target;
}


let targetToLogNames = new Map();

function loggerEventHandler(event) {
  let target = targetRoot(event.target);
  for (const name of targetToLogNames.get(target) || []) {
    let log = lazy.Log.repository.getLogger(name);
    let log_level;
    if (event.level == TracingLevel.DEBUG) {
      log_level = lazy.Log.Level.Debug;
    } else if (event.level == TracingLevel.TRACE) {
      log_level = lazy.Log.Level.Trace;
    } else if (event.level == TracingLevel.INFO) {
      log_level = lazy.Log.Level.Info;
    } else if (event.level == TracingLevel.WARN) {
      log_level = lazy.Log.Level.Warn;
    } else {
      log_level = lazy.Log.Level.Error;
    }
    log.log(log_level, event.message);
  }
}

export function setupLoggerForTarget(target, log) {
  if (typeof log == "string") {
    log = lazy.Log.repository.getLogger(log);
  }
  let log_level = log.level;
  let tracing_level;
  if (log_level == lazy.Log.ERROR) {
    tracing_level = TracingLevel.ERROR;
  } else if (log_level == lazy.Log.WARN) {
    tracing_level = TracingLevel.WARN;
  } else if (log_level == lazy.Log.INFO) {
    tracing_level = TracingLevel.INFO;
  } else if (log_level == lazy.Log.DEBUG) {
    tracing_level = TracingLevel.DEBUG;
  } else {
    tracing_level = TracingLevel.TRACE;
  }
  let logTargets = targetToLogNames.getOrInsert(target, []);
  logTargets.push(log.name);
  tracingEventHandler.register(target, tracing_level, loggerEventHandler);
  tracingEventHandler.updateRustTracingRegistration();
}

class LogForwarder extends EventSink {
  static PREF_CRATES_TO_FORWARD = "toolkit.rust-components.logging.crates";

  registeredTargets = new Set();
  registeredMinLevelSink = false;

  init() {
    Services.prefs.addObserver(LogForwarder.PREF_CRATES_TO_FORWARD, this);
    this.callback = this.onLog.bind(this);
    this.console = console.createInstance({});
    this.setupForwarding();
  }

  setupForwarding() {
    const prefValue = Services.prefs.getStringPref(
      LogForwarder.PREF_CRATES_TO_FORWARD,
      ""
    );
    const prefValueParsed = this.parsePrefValue(prefValue);

    if (prefValueParsed.minLevel != -Infinity) {
      tracingEventHandler.registerMinLevelEventSink(
        prefValueParsed.minLevel,
        this.callback
      );
      this.registeredMinLevelSink = true;
    } else if (this.registeredMinLevelSink) {
      tracingEventHandler.unregisterMinLevelEventSink(this.callback);
      this.registeredMinLevelSink = false;
    }

    const oldRegisteredTargets = this.registeredTargets;
    this.registeredTargets = new Set();
    for (const [target, level] of prefValueParsed.targets) {
      oldRegisteredTargets.delete(target);
      this.registeredTargets.add(target);
      tracingEventHandler.register(target, level, this.callback);
    }
    for (const oldTarget of oldRegisteredTargets) {
      tracingEventHandler.deregister(oldTarget, this.callback);
    }
    tracingEventHandler.updateRustTracingRegistration();
  }

  parsePrefValue(prefValue) {
    const parsed = {
      minLevel: -Infinity,
      targets: [],
    };

    if (prefValue == "") {
      parsed.minLevel = TracingLevel.ERROR;
      return parsed;
    }

    for (let spec of prefValue.split(",")) {
      spec = spec.trim();
      if (spec == "") {
        continue;
      }
      const minLevel = this.parseLevel(spec);
      if (minLevel !== undefined) {
        parsed.minLevel = Math.max(parsed.minLevel, minLevel);
      } else {
        parsed.targets.push(this.parseCrateSpec(spec));
      }
    }
    return parsed;
  }

  parseCrateSpec(spec) {
    var [target, level] = spec.split(":");
    if (level === undefined) {
      return [target, TracingLevel.DEBUG];
    }
    return [target, this.parseLevel(level) ?? TracingLevel.DEBUG];
  }

  parseLevel(levelString) {
    levelString = levelString.toLowerCase();
    if (levelString == "error") {
      return TracingLevel.ERROR;
    } else if (levelString == "warn" || levelString == "warning") {
      return TracingLevel.WARN;
    } else if (levelString == "info") {
      return TracingLevel.INFO;
    } else if (levelString == "debug") {
      return TracingLevel.DEBUG;
    } else if (levelString == "trace") {
      return TracingLevel.TRACE;
    }
    return undefined;
  }

  onLog(event) {
    const msg = `${event.target}: ${event.message}`;
    let log_level = event.level;
    if (log_level == TracingLevel.ERROR) {
      this.console.error(msg);
    } else if (log_level == TracingLevel.WARN) {
      this.console.warn(msg);
    } else if (log_level == TracingLevel.INFO) {
      this.console.info(msg);
    } else if (log_level == TracingLevel.DEBUG) {
      this.console.debug(msg);
    } else {
      this.console.trace(msg);
    }
  }

  observe(_subj, topic, data) {
    switch (topic) {
      case "nsPref:changed":
        if (data === LogForwarder.PREF_CRATES_TO_FORWARD) {
          this.setupForwarding();
        }
        break;
    }
  }
}

export const RustLogForwarder = new LogForwarder();
