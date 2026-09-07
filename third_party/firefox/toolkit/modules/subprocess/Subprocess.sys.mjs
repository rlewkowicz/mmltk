/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { SubprocessConstants } from "resource://gre/modules/subprocess/subprocess_common.sys.mjs";

const lazy = {};

if (AppConstants.platform == "win") {
  ChromeUtils.defineESModuleGetters(lazy, {
    SubprocessImpl: "resource://gre/modules/subprocess/subprocess_win.sys.mjs",
  });
} else {
  ChromeUtils.defineESModuleGetters(lazy, {
    SubprocessImpl: "resource://gre/modules/subprocess/subprocess_unix.sys.mjs",
  });
}

function encodeEnvVar(name, value) {
  if (typeof name === "string" && typeof value === "string") {
    return `${name}=${value}`;
  }

  let encoder = new TextEncoder();
  function encode(val) {
    return typeof val === "string" ? encoder.encode(val) : val;
  }

  return Uint8Array.of(...encode(name), ...encode("="), ...encode(value), 0);
}

function platformSupportsDisclaimedSpawn() {
  return AppConstants.platform === "macosx";
}

export var Subprocess = {
  call(options) {
    options = Object.assign({}, options);

    options.stderr = options.stderr || "ignore";
    options.workdir = options.workdir || null;
    options.disclaim = options.disclaim || false;

    let environment = {};
    if (!options.environment || options.environmentAppend) {
      environment = this.getEnvironment();
    }

    if (options.environment) {
      Object.assign(environment, options.environment);
    }

    options.environment = Object.entries(environment)
      .map(([key, val]) => (val !== null ? encodeEnvVar(key, val) : null))
      .filter(s => s);

    options.arguments = Array.from(options.arguments || []);

    if (options.disclaim && !platformSupportsDisclaimedSpawn()) {
      options.disclaim = false;
    }

    return Promise.resolve(
      lazy.SubprocessImpl.isExecutableFile(options.command)
    ).then(isExecutable => {
      if (!isExecutable) {
        let error = new Error(
          `File at path "${options.command}" does not exist, or is not executable`
        );
        error.errorCode = SubprocessConstants.ERROR_BAD_EXECUTABLE;
        throw error;
      }

      options.arguments.unshift(options.command);

      return lazy.SubprocessImpl.call(options);
    });
  },

  getEnvironment() {
    let environment = Object.create(null);
    for (let [k, v] of lazy.SubprocessImpl.getEnvironment()) {
      environment[k] = v;
    }
    return environment;
  },

  pathSearch(command, environment = this.getEnvironment()) {
    let path = lazy.SubprocessImpl.pathSearch(command, environment);
    return Promise.resolve(path);
  },

  connectRunning(fds) {
    return lazy.SubprocessImpl.connectRunning(fds);
  },
};

Object.assign(Subprocess, SubprocessConstants);
Object.freeze(Subprocess);

export function getSubprocessImplForTest() {
  return lazy.SubprocessImpl;
}
