/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  Policies: "resource:///modules/policies/Policies.sys.mjs",
  PolicySchemaValidator:
    "resource://gre/modules/policies/PolicySchemaValidator.sys.mjs",
  WindowsGPOParser: "resource://gre/modules/policies/WindowsGPOParser.sys.mjs",
  macOSPoliciesParser:
    "resource://gre/modules/policies/macOSPoliciesParser.sys.mjs",
  SitePolicyUtils: "resource://gre/modules/SitePolicyUtils.sys.mjs",
});

const POLICIES_FILENAME = "policies.json";

const PREF_PER_USER_DIR = "toolkit.policies.perUserDir";
const PREF_ALTERNATE_PATH = "browser.policies.alternatePath";
const PREF_ALTERNATE_GPO = "browser.policies.alternateGPO";

const MAGIC_TEST_ROOT_PREFIX = "<test-root>";
const PREF_TEST_ROOT = "mochitest.testRoot";

const PREF_LOGLEVEL = "browser.policies.loglevel";

const PREF_POLICIES_APPLIED = "browser.policies.applied";

ChromeUtils.defineLazyGetter(lazy, "log", () => {
  let { ConsoleAPI } = ChromeUtils.importESModule(
    "resource://gre/modules/Console.sys.mjs"
  );
  return new ConsoleAPI({
    prefix: "Enterprise Policies",
    maxLogLevel: "error",
    maxLogLevelPref: PREF_LOGLEVEL,
  });
});

function shouldIgnoreLocalPolicies() {
  return AppConstants.NIGHTLY_BUILD && false;
}

function isEmptyObject(obj) {
  if (typeof obj != "object" || Array.isArray(obj)) {
    return false;
  }
  for (let key of Object.keys(obj)) {
    if (!isEmptyObject(obj[key])) {
      return false;
    }
  }
  return true;
}

export function EnterprisePoliciesManager() {
  Services.obs.addObserver(this, "profile-after-change", true);
  Services.obs.addObserver(this, "final-ui-startup", true);
  Services.obs.addObserver(this, "sessionstore-windows-restored", true);
  Services.obs.addObserver(this, "EnterprisePolicies:Restart", true);
  Services.obs.addObserver(this, "distribution-customization-complete", true);
}

EnterprisePoliciesManager.prototype = {
  QueryInterface: ChromeUtils.generateQI([
    "nsIObserver",
    "nsISupportsWeakReference",
    "nsIEnterprisePolicies",
  ]),

  _initialize() {
    if (Services.prefs.getBoolPref(PREF_POLICIES_APPLIED, false)) {
      if ("_cleanup" in lazy.Policies) {
        let policyImpl = lazy.Policies._cleanup;

        for (let timing of Object.keys(this._callbacks)) {
          let policyCallback = policyImpl[timing];
          if (policyCallback) {
            this._schedulePolicyCallback(
              timing,
              policyCallback.bind(
                policyImpl,
                this 
              )
            );
          }
        }
      }
      Services.prefs.clearUserPref(PREF_POLICIES_APPLIED);
    }

    let provider = this._buildProvider();

    if (provider.failed) {
      this.status = Ci.nsIEnterprisePolicies.FAILED;
      return;
    }

    if (!provider.hasPolicies) {
      this.status = Ci.nsIEnterprisePolicies.INACTIVE;
      return;
    }

    if (
      Object.keys(provider.policies).length === 1 &&
      provider.policies.Certificates &&
      Object.keys(provider.policies.Certificates).length === 1 &&
      (provider.policies.Certificates.ImportEnterpriseRoots === true ||
        provider.policies.Certificates.ImportEnterpriseRoots === 1)
    ) {
      this.status = Ci.nsIEnterprisePolicies.INACTIVE;
      return;
    }

    this.status = Ci.nsIEnterprisePolicies.ACTIVE;

    Services.prefs
      .getDefaultBranch("")
      .setBoolPref("dom.webserial.enabled", false);

    this._parsedPolicies = {};
    this._activatePolicies(provider.policies);

    Services.prefs.setBoolPref(PREF_POLICIES_APPLIED, true);
  },

  _reportEnterpriseTelemetry() {
  },

  _buildProvider() {
    const provider = new CombinedProvider();

    lazy.log.debug("Adding JSON provider.");
    provider.push(new JSONPoliciesProvider());

    if (AppConstants.MOZ_SYSTEM_POLICIES) {
      if (AppConstants.platform == "win") {
        lazy.log.debug("Adding Windows GPO platform provider.");
        provider.push(new WindowsGPOPoliciesProvider());
      } else if (AppConstants.platform == "macosx") {
        lazy.log.debug("Adding macOS platform provider.");
        provider.push(new macOSPoliciesProvider());
      }
    }

    provider.mergePolicies();
    return provider;
  },

  _activatePolicies(unparsedPolicies) {
    let { schema } = ChromeUtils.importESModule(
      "resource:///modules/policies/schema.sys.mjs"
    );

    for (let policyName of Object.keys(unparsedPolicies)) {
      let policySchema = schema.properties[policyName];
      let policyParameters = unparsedPolicies[policyName];

      if (!policySchema) {
        lazy.log.error(`Unknown policy: ${policyName}`);
        continue;
      }

      let {
        valid: parametersAreValid,
        parsedValue: parsedParameters,
        error: validationError,
      } = lazy.PolicySchemaValidator.validate(policyParameters, policySchema, {
        allowAdditionalProperties: true,
      });

      if (!parametersAreValid) {
        lazy.log.error(
          `Invalid parameters specified for ${policyName}: ${validationError.message}`
        );
        continue;
      }

      let policyImpl = lazy.Policies[policyName];

      if (!policyImpl) {
        lazy.log.info(`${policyName} has been deprecated.`);
        continue;
      }

      if (policyImpl.validate && !policyImpl.validate(parsedParameters)) {
        lazy.log.error(
          `Parameters for ${policyName} did not validate successfully.`
        );
        continue;
      }

      this._parsedPolicies[policyName] = parsedParameters;

      for (let timing of Object.keys(this._callbacks)) {
        let policyCallback = policyImpl[timing];
        if (policyCallback) {
          this._schedulePolicyCallback(
            timing,
            policyCallback.bind(
              policyImpl,
              this ,
              parsedParameters
            )
          );
        }
      }
    }
  },

  _callbacks: {
    onBeforeAddons: [],

    onProfileAfterChange: [],

    onBeforeUIStartup: [],

    onAllWindowsRestored: [],
  },

  _schedulePolicyCallback(timing, callback) {
    this._callbacks[timing].push(callback);
  },

  _runPoliciesCallbacks(timing) {
    let callbacks = this._callbacks[timing];
    while (callbacks.length) {
      let callback = callbacks.shift();
      try {
        callback();
      } catch (ex) {
        lazy.log.error("Error running ", callback, `for ${timing}:`, ex);
      }
    }
  },

  async _restart() {
    DisallowedFeatures = {};
    SitePolicies = [];

    Services.ppmm.sharedData.delete("EnterprisePolicies:Status");
    Services.ppmm.sharedData.delete("EnterprisePolicies:DisallowedFeatures");
    Services.ppmm.sharedData.delete("EnterprisePolicies:SitePolicies");

    this._status = Ci.nsIEnterprisePolicies.UNINITIALIZED;
    this._parsedPolicies = undefined;
    for (let timing of Object.keys(this._callbacks)) {
      this._callbacks[timing] = [];
    }

    let notifyTopicOnIdle = topic =>
      new Promise(resolve => {
        ChromeUtils.idleDispatch(() => {
          this.observe(null, topic, "");
          resolve();
        });
      });
    await notifyTopicOnIdle("policies-startup");
    await notifyTopicOnIdle("profile-after-change");
    await notifyTopicOnIdle("final-ui-startup");
    await notifyTopicOnIdle("sessionstore-windows-restored");
    await notifyTopicOnIdle("distribution-customization-complete");
  },

  observe: function BG_observe(subject, topic) {
    switch (topic) {
      case "policies-startup":
        this._initialize();

        this._runPoliciesCallbacks("onBeforeAddons");
        break;

      case "profile-after-change":
        this._runPoliciesCallbacks("onProfileAfterChange");
        break;

      case "final-ui-startup":
        this._runPoliciesCallbacks("onBeforeUIStartup");
        break;

      case "sessionstore-windows-restored":
        this._runPoliciesCallbacks("onAllWindowsRestored");
        break;

      case "EnterprisePolicies:Restart":
        this._restart().then(null, console.error);
        break;

      case "distribution-customization-complete":
        this._reportEnterpriseTelemetry();

        Services.obs.notifyObservers(
          null,
          "EnterprisePolicies:AllPoliciesApplied"
        );

        break;
    }
  },

  disallowFeature(feature, neededOnContentProcess = false) {
    DisallowedFeatures[feature] = neededOnContentProcess;

    if (neededOnContentProcess) {
      Services.ppmm.sharedData.set(
        "EnterprisePolicies:DisallowedFeatures",
        new Set(
          Object.keys(DisallowedFeatures).filter(key => DisallowedFeatures[key])
        )
      );
    }
  },

  updateSitePolicies(policies) {
    SitePolicies = policies;

    let clonable = policies.map(policy => ({
      match: policy.match.patterns.map(p => p.pattern),
      exceptions: policy.exceptions.patterns.map(p => p.pattern),
      features: policy.features,
    }));

    Services.ppmm.sharedData.set("EnterprisePolicies:SitePolicies", clonable);
  },


  _status: Ci.nsIEnterprisePolicies.UNINITIALIZED,

  set status(val) {
    this._status = val;
    if (val != Ci.nsIEnterprisePolicies.INACTIVE) {
      Services.ppmm.sharedData.set("EnterprisePolicies:Status", val);
    }
  },

  get status() {
    return this._status;
  },

  isAllowed(feature) {
    return !(feature in DisallowedFeatures);
  },

  isAllowedForURI(feature, uri) {
    return lazy.SitePolicyUtils.isAllowedForURI(
      this,
      SitePolicies,
      feature,
      uri
    );
  },

  getActivePolicies() {
    return this._parsedPolicies;
  },

  setSupportMenu(supportMenu) {
    SupportMenu = supportMenu;
  },

  getSupportMenu() {
    return SupportMenu;
  },

  isExemptExecutableExtension(url, extension) {
    let urlObject = URL.parse(url);
    if (!urlObject) {
      return false;
    }
    let { hostname } = urlObject;
    let exemptArray =
      this.getActivePolicies()
        ?.ExemptDomainFileTypePairsFromFileTypeDownloadWarnings;
    if (!hostname || !extension || !exemptArray) {
      return false;
    }
    extension = extension.toLowerCase();
    let domains = exemptArray
      .filter(item => item.file_extension.toLowerCase() == extension)
      .map(item => item.domains)
      .flat();
    for (let domain of domains) {
      if (Services.eTLD.hasRootDomain(hostname, domain)) {
        return true;
      }
    }
    return false;
  },

  get isEnterprise() {
    let policiesLength = Object.keys(this._parsedPolicies || {}).length;

    let isEnterprise =
      AppConstants.IS_ESR ||
      policiesLength > 0;

    return isEnterprise;
  },
};

let DisallowedFeatures = {};
let SitePolicies = [];
let SupportMenu = null;

class PoliciesProvider {
  constructor() {
    this._policies = null;
    this._failed = false;
  }

  get policies() {
    return this._policies;
  }

  get hasPolicies() {
    return this._policies !== null && !isEmptyObject(this._policies);
  }

  get failed() {
    return this._failed;
  }
}


class JSONPoliciesProvider extends PoliciesProvider {
  constructor() {
    super();
    this._readData();
  }

  _getLocalConfigurationFile() {
    if (shouldIgnoreLocalPolicies()) {
      return null;
    }

    if (AppConstants.platform == "linux" && AppConstants.MOZ_SYSTEM_POLICIES) {
      let systemConfigFile = Services.dirsvc.get("SysConfD", Ci.nsIFile);
      systemConfigFile.append("policies");
      systemConfigFile.append(POLICIES_FILENAME);
      if (systemConfigFile.exists()) {
        return systemConfigFile;
      }
    }

    try {
      let configFile;
      let perUserPath = Services.prefs.getBoolPref(PREF_PER_USER_DIR, false);
      if (perUserPath) {
        configFile = Services.dirsvc.get("XREUserRunTimeDir", Ci.nsIFile);
      } else {
        configFile = Services.dirsvc.get("XREAppDist", Ci.nsIFile);
      }
      configFile.append(POLICIES_FILENAME);
      return configFile;
    } catch (ex) {
      return null;
    }
  }

  _getConfigurationFile() {
    let configFile = this._getLocalConfigurationFile();

    let alternatePath = Services.prefs.getStringPref(PREF_ALTERNATE_PATH, "");

    if (
      alternatePath &&
      (false || AppConstants.NIGHTLY_BUILD) &&
      (!configFile || !configFile.exists())
    ) {
      if (alternatePath.startsWith(MAGIC_TEST_ROOT_PREFIX)) {
        let testRoot = Services.prefs.getStringPref(PREF_TEST_ROOT);
        let relativePath = alternatePath.substring(
          MAGIC_TEST_ROOT_PREFIX.length
        );
        if (AppConstants.platform == "win") {
          relativePath = relativePath.replace(/\//g, "\\");
        }
        alternatePath = testRoot + relativePath;
      }

      configFile = Cc["@mozilla.org/file/local;1"].createInstance(Ci.nsIFile);
      configFile.initWithPath(alternatePath);
    }

    return configFile;
  }

  _readData() {
    let configFile = this._getConfigurationFile();
    if (!configFile) {
      return;
    }
    try {
      let data = Cu.readUTF8File(configFile);
      if (data) {
        lazy.log.debug(`policies.json path = ${configFile.path}`);
        lazy.log.debug(`policies.json content = ${data}`);
        this._policies = JSON.parse(data).policies;

        if (!this._policies) {
          lazy.log.error("Policies file doesn't contain a 'policies' object");
          this._policies = null;
          this._failed = true;
        }
      }
    } catch (ex) {
      if (
        ex instanceof Components.Exception &&
        ex.result == Cr.NS_ERROR_FILE_NOT_FOUND
      ) {
      } else if (ex instanceof SyntaxError) {
        lazy.log.error(`Error parsing JSON file: ${ex}`);
        this._failed = true;
      } else {
        lazy.log.error(`Error reading JSON file: ${ex}`);
        this._failed = true;
      }
    }
  }
}

class WindowsGPOPoliciesProvider extends PoliciesProvider {
  constructor() {
    super();

    let wrk = Cc["@mozilla.org/windows-registry-key;1"].createInstance(
      Ci.nsIWindowsRegKey
    );

    this._readData(wrk, wrk.ROOT_KEY_CURRENT_USER);
    if (!false) {
      this._readData(wrk, wrk.ROOT_KEY_LOCAL_MACHINE);
    }
  }

  _readData(wrk, root) {
    try {
      let regLocation = "SOFTWARE\\Policies";
      if (false) {
        let altLocation = Services.prefs.getStringPref(PREF_ALTERNATE_GPO, "");
        if (altLocation) {
          regLocation = altLocation;
        } else if (shouldIgnoreLocalPolicies()) {
          return;
        }
      }
      wrk.open(root, regLocation, wrk.ACCESS_READ);
      if (wrk.hasChild("Mozilla\\" + Services.appinfo.name)) {
        lazy.log.debug(
          `root = ${
            root == wrk.ROOT_KEY_CURRENT_USER
              ? "HKEY_CURRENT_USER"
              : "HKEY_LOCAL_MACHINE"
          }`
        );
        this._policies = lazy.WindowsGPOParser.readPolicies(
          wrk,
          this._policies
        );
      }
      wrk.close();
    } catch (e) {
      lazy.log.error("Unable to access registry - ", e);
    }
  }
}

class macOSPoliciesProvider extends PoliciesProvider {
  constructor() {
    super();
    let prefReader = Cc["@mozilla.org/mac-preferences-reader;1"].createInstance(
      Ci.nsIMacPreferencesReader
    );
    if (!prefReader.policiesEnabled()) {
      return;
    }
    this._policies = lazy.macOSPoliciesParser.readPolicies(prefReader);
  }
}

export class CombinedProvider extends PoliciesProvider {
  constructor() {
    super();
    this._providers = [];
  }

  push(provider) {
    this._providers.push(provider);
  }

  mergePolicies() {
    this._policies = Object.assign({}, ...this._providers.map(p => p.policies));
  }

  get failed() {
    return this._providers.some(p => p.failed) && !this.hasPolicies;
  }
}
