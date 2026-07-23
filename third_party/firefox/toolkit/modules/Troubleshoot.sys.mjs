/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PlacesDBUtils: "resource://gre/modules/PlacesDBUtils.sys.mjs",
});

const PREFS_FOR_DISPLAY = [
  "accessibility.",
  "apz.",
  "browser.cache.",
  "browser.contentblocking.category",
  "browser.display.",
  "browser.download.always_ask_before_handling_new_types",
  "browser.download.enable_spam_prevention",
  "browser.download.folderList",
  "browser.download.improvements_to_download_panel",
  "browser.download.lastDir.savePerSite",
  "browser.download.manager.addToRecentDocs",
  "browser.download.manager.resumeOnWakeDelay",
  "browser.download.open_pdf_attachments_inline",
  "browser.download.preferred.",
  "browser.download.skipConfirmLaunchExecutable",
  "browser.download.start_downloads_in_tmp_dir",
  "browser.download.useDownloadDir",
  "browser.fixup.",
  "browser.history_expire_",
  "browser.link.open_newwindow",
  "browser.places.",
  "browser.privatebrowsing.",
  "browser.search.context.loadInBackground",
  "browser.search.lastEngineIgnored",
  "browser.search.lastSettingsCorruptTime",
  "browser.search.log",
  "browser.search.openintab",
  "browser.search.param",
  "browser.search.region",
  "browser.search.searchEnginesURL",
  "browser.search.suggest.enabled",
  "browser.search.update",
  "browser.sessionstore.",
  "browser.startup.homepage",
  "browser.startup.page",
  "browser.tabs.",
  "browser.theme.",
  "browser.toolbars.",
  "browser.urlbar.",
  "browser.zoom.",
  "doh-rollout.",
  "dom.",
  "extensions.backgroundServiceWorker.enabled",
  "extensions.checkCompatibility",
  "extensions.eventPages.enabled",
  "extensions.formautofill.",
  "extensions.lastAppVersion",
  "extensions.quarantinedDomains.enabled",
  "extensions.InstallTrigger.enabled",
  "fission.autostart",
  "font.",
  "general.autoScroll",
  "general.useragent.",
  "gfx.",
  "html5.",
  "identity.fxaccounts.enabled",
  "idle.",
  "image.",
  "javascript.",
  "keyword.",
  "layers.",
  "layout.css.",
  "layout.display-list.",
  "layout.frame_rate",
  "media.",
  "mousewheel.",
  "network.",
  "permissions.default.image",
  "places.",
  "plugin.",
  "plugins.",
  "privacy.",
  "security.",
  "services.sync.declinedEngines",
  "services.sync.lastPing",
  "services.sync.lastSync",
  "services.sync.numClients",
  "services.sync.engine.",
  "signon.",
  "storage.vacuum.last.",
  "svg.",
  "toolkit.startup.recent_crashes",
  "ui.osk.enabled",
  "ui.osk.detect_physical_keyboard",
  "ui.osk.require_tablet_mode",
  "ui.osk.debug.keyboardDisplayReason",
  "webgl.",
  "widget.dmabuf",
  "widget.use-xdg-desktop-portal",
  "widget.use-xdg-desktop-portal.file-picker",
  "widget.use-xdg-desktop-portal.mime-handler",
  "widget.gtk.overlay-scrollbars.enabled",
  "widget.wayland",
];

const PREF_REGEXES_NOT_TO_DISPLAY = [
  /^browser[.]fixup[.]domainwhitelist[.]/,
  /^dom[.]push[.]userAgentID/,
  /^media[.]webrtc[.]debug[.]aec_log_dir/,
  /^media[.]webrtc[.]debug[.]log_file/,
  /^print[.].*print_to_filename$/,
  /^network[.]proxy[.]/,
];

const PREFS_GETTERS = {};

PREFS_GETTERS[Ci.nsIPrefBranch.PREF_STRING] = (prefs, name) =>
  prefs.getStringPref(name);
PREFS_GETTERS[Ci.nsIPrefBranch.PREF_INT] = (prefs, name) =>
  prefs.getIntPref(name);
PREFS_GETTERS[Ci.nsIPrefBranch.PREF_BOOL] = (prefs, name) =>
  prefs.getBoolPref(name);

const PREFS_UNIMPORTANT_LOCKED = [
  "dom.postMessage.sharedArrayBuffer.bypassCOOP_COEP.insecure.enabled",
  "extensions.backgroundServiceWorker.enabled",
  "privacy.restrict3rdpartystorage.url_decorations",
  "security.storage.encryption.sqlite.enabled",
];

function getPref(name) {
  let type = Services.prefs.getPrefType(name);
  if (!(type in PREFS_GETTERS)) {
    throw new Error("Unknown preference type " + type + " for " + name);
  }
  return PREFS_GETTERS[type](Services.prefs, name);
}

function getPrefList(filter, allowlist = PREFS_FOR_DISPLAY) {
  return allowlist.reduce(function (prefs, branch) {
    Services.prefs.getChildList(branch).forEach(function (name) {
      if (
        filter(name) &&
        !PREF_REGEXES_NOT_TO_DISPLAY.some(re => re.test(name))
      ) {
        prefs[name] = getPref(name);
      }
    });
    return prefs;
  }, {});
}

export var Troubleshoot = {
  snapshot() {
    return new Promise(resolve => {
      let snapshot = {};
      let numPending = Object.keys(dataProviders).length;
      function providerDone(providerName, providerData) {
        snapshot[providerName] = providerData;
        if (--numPending == 0) {
          Services.tm.dispatchToMainThread(() => resolve(snapshot));
        }
      }
      for (let name in dataProviders) {
        try {
          dataProviders[name](providerDone.bind(null, name));
        } catch (err) {
          let msg = "Troubleshoot data provider failed: " + name + "\n" + err;
          console.error(msg);
          providerDone(name, msg);
        }
      }
    });
  },

  kMaxCrashAge: 3 * 24 * 60 * 60 * 1000, 
};

var dataProviders = {
  application: async function application(done) {
    let data = {
      name: Services.appinfo.name,
      osVersion:
        Services.sysinfo.getProperty("name") +
        " " +
        Services.sysinfo.getProperty("version") +
        " " +
        Services.sysinfo.getProperty("build"),
      version: AppConstants.MOZ_APP_VERSION_DISPLAY,
      buildID: Services.appinfo.appBuildID,
      distributionID: Services.prefs
        .getDefaultBranch("")
        .getCharPref("distribution.id", ""),
      userAgent: Cc["@mozilla.org/network/protocol;1?name=http"].getService(
        Ci.nsIHttpProtocolHandler
      ).userAgent,
      safeMode: Services.appinfo.inSafeMode,
      memorySizeBytes: Services.sysinfo.getProperty("memsize"),
      diskAvailableBytes: Services.dirsvc.get("ProfD", Ci.nsIFile)
        .diskSpaceAvailable,
    };

    if (Services.sysinfo.getProperty("name") == "Windows_NT") {
      if ((await Services.sysinfo.processInfo).isWindowsSMode) {
        data.osVersion += " S";
      }
    }

    if (AppConstants.MOZ_UPDATER) {
      data.updateChannel = ChromeUtils.importESModule(
        "resource://gre/modules/UpdateUtils.sys.mjs"
      ).UpdateUtils.UpdateChannel;
    }

    // eslint-disable-next-line mozilla/use-default-preference-values
    try {
      data.vendor = Services.prefs.getCharPref("app.support.vendor");
    } catch (e) {}
    try {
      data.supportURL = Services.urlFormatter.formatURLPref(
        "app.support.baseURL"
      );
    } catch (e) {}

    data.osTheme = Services.sysinfo.getProperty("osThemeInfo");

    try {
      data.rosetta = Services.sysinfo.getProperty("rosettaStatus");
    } catch (e) {}

    try {
      data.pointingDevices = [];
      if (Services.sysinfo.getProperty("hasMouse")) {
        data.pointingDevices.push("pointing-device-mouse");
      }
      if (Services.sysinfo.getProperty("hasTouch")) {
        data.pointingDevices.push("pointing-device-touchscreen");
      }
      if (Services.sysinfo.getProperty("hasPen")) {
        data.pointingDevices.push("pointing-device-pen-digitizer");
      }
      if (!data.pointingDevices.length) {
        data.pointingDevices.push("pointing-device-none");
      }
    } catch (e) {}

    data.numTotalWindows = 0;
    data.numFissionWindows = 0;
    data.numRemoteWindows = 0;
    for (let { docShell } of Services.wm.getEnumerator(
      AppConstants.platform == "android"
        ? "navigator:geckoview"
        : "navigator:browser"
    )) {
      docShell.QueryInterface(Ci.nsILoadContext);
      data.numTotalWindows++;
      if (docShell.useRemoteSubframes) {
        data.numFissionWindows++;
      }
      if (docShell.useRemoteTabs) {
        data.numRemoteWindows++;
      }
    }

    try {
      data.launcherProcessState = Services.appinfo.launcherProcessState;
    } catch (e) {}

    data.fissionAutoStart = Services.appinfo.fissionAutostart;
    data.fissionDecisionStatus = Services.appinfo.fissionDecisionStatusString;

    data.remoteAutoStart = Services.appinfo.browserTabsRemoteAutostart;

    if (Services.policies) {
      data.policiesStatus = Services.policies.status;
    }

    const keyLocationServiceGoogle = Services.urlFormatter
      .formatURL("%GOOGLE_LOCATION_SERVICE_API_KEY%")
      .trim();
    data.keyLocationServiceGoogleFound =
      keyLocationServiceGoogle != "no-google-location-service-api-key" &&
      !!keyLocationServiceGoogle.length;

    const keyMozilla = Services.urlFormatter
      .formatURL("%MOZILLA_API_KEY%")
      .trim();
    data.keyMozillaFound =
      keyMozilla != "no-mozilla-api-key" && !!keyMozilla.length;

    done(data);
  },

  securitySoftware: function securitySoftware(done) {
    let data = {};

    const keys = [
      "registeredAntiVirus",
      "registeredAntiSpyware",
      "registeredFirewall",
    ];
    for (let key of keys) {
      let prop = "";
      try {
        prop = Services.sysinfo.getProperty(key);
      } catch (e) {}

      data[key] = prop;
    }

    done(data);
  },

  processes: async function processes(done) {
    let remoteTypes = {};
    const processInfo = await ChromeUtils.requestProcInfo();
    for (let i = 0; i < processInfo.children.length; i++) {
      let remoteType;
      try {
        remoteType = processInfo.children[i].type;
        remoteType = remoteType === "preallocated" ? "prealloc" : remoteType;
      } catch (e) {}

      if (remoteType === "utility") {
        continue;
      }

      if (!remoteType) {
        continue;
      }

      if (remoteTypes[remoteType]) {
        remoteTypes[remoteType]++;
      } else {
        remoteTypes[remoteType] = 1;
      }
    }

    for (let i = 0; i < processInfo.children.length; i++) {
      if (processInfo.children[i].type === "utility") {
        for (let utilityWithActor of processInfo.children[i].utilityActors.map(
          e => `utility_${e.actorName}`
        )) {
          if (remoteTypes[utilityWithActor]) {
            remoteTypes[utilityWithActor]++;
          } else {
            remoteTypes[utilityWithActor] = 1;
          }
        }
      }
    }

    try {
      let winUtils = Services.wm.getMostRecentWindow("").windowUtils;
      if (winUtils.gpuProcessPid != -1) {
        remoteTypes.gpu = 1;
      }
    } catch (e) {}

    if (Services.io.socketProcessLaunched) {
      remoteTypes.socket = 1;
    }

    let data = {
      remoteTypes,
      maxWebContentProcesses: Services.appinfo.maxWebProcessCount,
    };

    done(data);
  },

  async legacyUserStylesheets(done) {
    if (AppConstants.platform == "android") {
      done({ active: false, types: [] });
      return;
    }

    let active = Services.prefs.getBoolPref(
      "toolkit.legacyUserProfileCustomizations.stylesheets"
    );
    let types = [];
    for (let name of ["userChrome.css", "userContent.css"]) {
      let path = PathUtils.join(PathUtils.profileDir, "chrome", name);
      if (await IOUtils.exists(path)) {
        types.push(name);
      }
    }
    done({ active, types });
  },

  async environmentVariables(done) {
    if (AppConstants.platform == "android") {
      done({});
      return;
    }
    const { Subprocess } = ChromeUtils.importESModule(
      "resource://gre/modules/Subprocess.sys.mjs"
    );

    let environment = Subprocess.getEnvironment();
    let filteredEnvironment = {};
    let filteredEnvironmentKeys = ["xre_", "moz_", "gdk", "display"];
    let exactEnvironmentKeys = ["sslkeylogfile"];
    for (let key of Object.keys(environment)) {
      let lowerKey = key.toLowerCase();
      if (
        filteredEnvironmentKeys.some(k => lowerKey.startsWith(k)) ||
        exactEnvironmentKeys.includes(lowerKey)
      ) {
        filteredEnvironment[key] = environment[key];
      }
    }
    done(filteredEnvironment);
  },

  modifiedPreferences: function modifiedPreferences(done) {
    done(getPrefList(name => Services.prefs.prefHasUserValue(name)));
  },

  lockedPreferences: function lockedPreferences(done) {
    done(
      getPrefList(
        name =>
          !PREFS_UNIMPORTANT_LOCKED.includes(name) &&
          Services.prefs.prefIsLocked(name)
      )
    );
  },

  places: async function places(done) {
    const data = {};

    if (AppConstants.MOZ_PLACES) {
      data.prefs = await lazy.PlacesDBUtils.getEntitiesStatsAndCounts();

      data.lastMaintenanceDate =
        Services.prefs.getIntPref("places.database.lastMaintenance", 0) * 1000;
      data.lastVacuumDate =
        Services.prefs.getIntPref("storage.vacuum.last.places.sqlite", 0) *
        1000;

      try {
        const corruptFilePath = PathUtils.join(
          PathUtils.profileDir,
          "places.sqlite.corrupt"
        );
        const fileInfo = await IOUtils.stat(corruptFilePath);
        data.lastIntegrityCorruptionDate = fileInfo.lastModified;
      } catch (e) {
        data.lastIntegrityCorruptionDate = 0;
      }
    }

    done(data);
  },

  printingPreferences: function printingPreferences(done) {
    let filter = name => Services.prefs.prefHasUserValue(name);
    let prefs = getPrefList(filter, ["print."]);

    if (filter("print_printer")) {
      prefs.print_printer = getPref("print_printer");
    }

    done(prefs);
  },

  graphics: function graphics(done) {
    function statusMsgForFeature(feature) {
      let msg = { key: "" };
      try {
        var status = gfxInfo.getFeatureStatusStr(feature);
      } catch (e) {}
      switch (status) {
        case "BLOCKED_DEVICE":
        case "DISCOURAGED":
          msg = { key: "blocked-gfx-card" };
          break;
        case "BLOCKED_OS_VERSION":
          msg = { key: "blocked-os-version" };
          break;
        case "BLOCKED_DRIVER_VERSION":
          try {
            var driverVersion =
              gfxInfo.getFeatureSuggestedDriverVersionStr(feature);
          } catch (e) {}
          msg = driverVersion
            ? { key: "try-newer-driver", args: { driverVersion } }
            : { key: "blocked-driver" };
          break;
        case "BLOCKED_MISMATCHED_VERSION":
          msg = { key: "blocked-mismatched-version" };
          break;
      }
      return msg;
    }

    let data = {};

    try {
      var gfxInfo = Cc["@mozilla.org/gfx/info;1"].getService(Ci.nsIGfxInfo);
    } catch (e) {}

    data.desktopEnvironment = Services.appinfo.desktopEnvironment;
    data.numTotalWindows = 0;
    data.numAcceleratedWindows = 0;

    let devicePixelRatios = [];

    for (let win of Services.ww.getWindowEnumerator()) {
      let winUtils = win.windowUtils;
      try {
        if (
          winUtils.layerManagerType == "None" ||
          !winUtils.layerManagerRemote
        ) {
          continue;
        }
        devicePixelRatios.push(win.devicePixelRatio);

        data.numTotalWindows++;
        data.windowLayerManagerType = winUtils.layerManagerType;
        data.windowLayerManagerRemote = winUtils.layerManagerRemote;
      } catch (e) {
        continue;
      }
      if (data.windowLayerManagerType != "Basic") {
        data.numAcceleratedWindows++;
      }
    }
    data.graphicsDevicePixelRatios = devicePixelRatios;

    if (!data.windowLayerManagerType) {
      data.windowLayerManagerType = "Basic";
      data.windowLayerManagerRemote = false;
    }

    if (!data.numAcceleratedWindows && gfxInfo) {
      let win = AppConstants.platform == "win";
      let feature = win ? "DIRECT3D_9_LAYERS" : "OPENGL_LAYERS";
      data.numAcceleratedWindowsMessage = statusMsgForFeature(feature);
    }

    if (gfxInfo) {
      let gfxInfoProps = {
        adapterDescription: null,
        adapterVendorID: null,
        adapterDeviceID: null,
        adapterSubsysID: null,
        adapterRAM: null,
        adapterDriver: "adapterDrivers",
        adapterDriverVendor: "driverVendor",
        adapterDriverVersion: "driverVersion",
        adapterDriverDate: "driverDate",

        adapterDescription2: null,
        adapterVendorID2: null,
        adapterDeviceID2: null,
        adapterSubsysID2: null,
        adapterRAM2: null,
        adapterDriver2: "adapterDrivers2",
        adapterDriverVendor2: "driverVendor2",
        adapterDriverVersion2: "driverVersion2",
        adapterDriverDate2: "driverDate2",
        isGPU2Active: null,

        DWriteEnabled: "directWriteEnabled",
        DWriteVersion: "directWriteVersion",
        cleartypeParameters: "clearTypeParameters",
        TargetFrameRate: "targetFrameRate",
        windowProtocol: null,
        fontVisibilityDeterminationStr: "supportFontDetermination",
      };

      for (let prop in gfxInfoProps) {
        try {
          data[gfxInfoProps[prop] || prop] = gfxInfo[prop];
        } catch (e) {}
      }
    }

    let doc = new DOMParser().parseFromString("<html/>", "text/html");

    function GetWebGLInfo(data, keyPrefix, contextType) {
      data[keyPrefix + "Renderer"] = "-";
      data[keyPrefix + "Version"] = "-";
      data[keyPrefix + "DriverExtensions"] = "-";
      data[keyPrefix + "Extensions"] = "-";
      data[keyPrefix + "WSIInfo"] = "-";


      let canvas = doc.createElement("canvas");
      canvas.width = 1;
      canvas.height = 1;


      let creationError = null;

      canvas.addEventListener(
        "webglcontextcreationerror",

        function (e) {
          creationError = e.statusMessage;
        }
      );

      let gl = null;
      try {
        gl = canvas.getContext(contextType);
      } catch (e) {
        if (!creationError) {
          creationError = e.toString();
        }
      }
      if (!gl) {
        data[keyPrefix + "Renderer"] =
          creationError || "(no creation error info)";
        return;
      }


      data[keyPrefix + "Extensions"] = gl.getSupportedExtensions().join(" ");


      let ext = gl.getExtension("MOZ_debug");
      let vendor = ext.getParameter(gl.VENDOR);
      let renderer = ext.getParameter(gl.RENDERER);

      data[keyPrefix + "Renderer"] = vendor + " -- " + renderer;
      data[keyPrefix + "Version"] = ext.getParameter(gl.VERSION);
      data[keyPrefix + "DriverExtensions"] = ext.getParameter(ext.EXTENSIONS);
      data[keyPrefix + "WSIInfo"] = ext.getParameter(ext.WSI_INFO);


      let loseExt = gl.getExtension("WEBGL_lose_context");
      if (loseExt) {
        loseExt.loseContext();
      }
    }

    GetWebGLInfo(data, "webgl1", "webgl");
    GetWebGLInfo(data, "webgl2", "webgl2");

    if (gfxInfo) {
      let infoInfo = gfxInfo.getInfo();
      if (infoInfo) {
        data.info = infoInfo;
      }

      let failureIndices = {};

      let failures = gfxInfo.getFailures(failureIndices);
      if (failures.length) {
        data.failures = failures;
        if (failureIndices.value.length == failures.length) {
          data.indices = failureIndices.value;
        }
      }

      data.featureLog = gfxInfo.getFeatureLog();
      data.crashGuards = gfxInfo.getActiveCrashGuards();
    }

    function getNavigator() {
      for (let win of Services.ww.getWindowEnumerator()) {
        let winUtils = win.windowUtils;
        try {
          if (
            winUtils.layerManagerType == "None" ||
            !winUtils.layerManagerRemote
          ) {
            continue;
          }
          const nav = win.navigator;
          if (nav) {
            return nav;
          }
        } catch (e) {
          continue;
        }
      }
      throw new Error("No window had window.navigator.");
    }

    const navigator = getNavigator();

    async function GetWebgpuInfo(adapterOpts) {
      const ret = {};
      if (!navigator.gpu) {
        ret["navigator.gpu"] = null;
        return ret;
      }

      const requestAdapterkey = `navigator.gpu.requestAdapter(${JSON.stringify(
        adapterOpts
      )})`;

      let adapter;
      try {
        adapter = await navigator.gpu.requestAdapter(adapterOpts);
      } catch (e) {
        if (DOMException.isInstance(e) && e.name == "NotSupportedError") {
          return { [requestAdapterkey]: { not_supported: e.message } };
        }
        throw e;
      }

      if (!adapter) {
        ret[requestAdapterkey] = null;
        return ret;
      }
      const desc = (ret[requestAdapterkey] = {});

      desc.isFallbackAdapter = adapter.isFallbackAdapter;

      const adapterInfo = adapter.info;
      const adapterInfoObj = {};
      for (const k of Object.keys(Object.getPrototypeOf(adapterInfo)).sort()) {
        adapterInfoObj[k] = adapterInfo[k];
      }
      desc.info = adapterInfoObj;

      desc.features = Array.from(adapter.features).sort();

      desc.limits = {};
      const keys = Object.keys(Object.getPrototypeOf(adapter.limits)).sort(); 
      for (const k of keys) {
        desc.limits[k] = adapter.limits[k];
      }

      return ret;
    }

    (async () => {
      data.webgpuDefaultAdapter = await GetWebgpuInfo({});
      data.webgpuFallbackAdapter = await GetWebgpuInfo({
        forceFallbackAdapter: true,
      });

      done(data);
    })();
  },

  media: function media(done) {
    function convertDevices(devices) {
      if (!devices) {
        return undefined;
      }
      let infos = [];
      for (let i = 0; i < devices.length; ++i) {
        let device = devices.queryElementAt(i, Ci.nsIAudioDeviceInfo);
        infos.push({
          name: device.name,
          groupId: device.groupId,
          vendor: device.vendor,
          type: device.type,
          state: device.state,
          preferred: device.preferred,
          supportedFormat: device.supportedFormat,
          defaultFormat: device.defaultFormat,
          maxChannels: device.maxChannels,
          defaultRate: device.defaultRate,
          maxRate: device.maxRate,
          minRate: device.minRate,
          maxLatency: device.maxLatency,
          minLatency: device.minLatency,
        });
      }
      return infos;
    }

    let data = {};
    let winUtils = Services.wm.getMostRecentWindow("").windowUtils;
    data.currentAudioBackend = winUtils.currentAudioBackend;
    data.currentMaxAudioChannels = winUtils.currentMaxAudioChannels;
    data.currentPreferredSampleRate = winUtils.currentPreferredSampleRate;
    data.audioOutputDevices = convertDevices(
      winUtils
        .audioDevices(Ci.nsIDOMWindowUtils.AUDIO_OUTPUT)
        .QueryInterface(Ci.nsIArray)
    );
    data.audioInputDevices = convertDevices(
      winUtils
        .audioDevices(Ci.nsIDOMWindowUtils.AUDIO_INPUT)
        .QueryInterface(Ci.nsIArray)
    );

    data.codecSupportInfo = "Unknown";

    try {
      var gfxInfo = Cc["@mozilla.org/gfx/info;1"].getService(Ci.nsIGfxInfo);

      data.codecSupportInfo = gfxInfo.CodecSupportInfo;
    } catch (e) {}

    done(data);
  },

  accessibility: function accessibility(done) {
    let data = {};
    data.isActive = Services.appinfo.accessibilityEnabled;
    // eslint-disable-next-line mozilla/use-default-preference-values
    try {
      data.forceDisabled = Services.prefs.getIntPref(
        "accessibility.force_disabled"
      );
    } catch (e) {}
    data.instantiator = Services.appinfo.accessibilityInstantiator;
    done(data);
  },

  startupCache: function startupCache(done) {
    const startupInfo = Cc["@mozilla.org/startupcacheinfo;1"].getService(
      Ci.nsIStartupCacheInfo
    );
    done({
      DiskCachePath: startupInfo.DiskCachePath,
      IgnoreDiskCache: startupInfo.IgnoreDiskCache,
      FoundDiskCacheOnInit: startupInfo.FoundDiskCacheOnInit,
      WroteToDiskCache: startupInfo.WroteToDiskCache,
    });
  },

  libraryVersions: function libraryVersions(done) {
    let data = {};
    let verInfo = Cc["@mozilla.org/security/nssversion;1"].getService(
      Ci.nsINSSVersion
    );
    for (let prop in verInfo) {
      let match = /^([^_]+)_((Min)?Version)$/.exec(prop);
      if (match) {
        let verProp = match[2][0].toLowerCase() + match[2].substr(1);
        data[match[1]] = data[match[1]] || {};
        data[match[1]][verProp] = verInfo[prop];
      }
    }
    done(data);
  },

  userJS: function userJS(done) {
    let userJSFile = Services.dirsvc.get("PrefD", Ci.nsIFile);
    userJSFile.append("user.js");
    done({
      exists: userJSFile.exists() && userJSFile.fileSize > 0,
    });
  },

  intl: function intl(done) {
    const osPrefs = Cc["@mozilla.org/intl/ospreferences;1"].getService(
      Ci.mozIOSPreferences
    );
    done({
      localeService: {
        requested: Services.locale.requestedLocales,
        available: Services.locale.availableLocales,
        supported: Services.locale.appLocalesAsBCP47,
        regionalPrefs: Services.locale.regionalPrefsLocales,
        defaultLocale: Services.locale.defaultLocale,
      },
      osPrefs: {
        systemLocales: osPrefs.systemLocales,
        regionalPrefsLocales: osPrefs.regionalPrefsLocales,
      },
    });
  },

  async normandy(done) {
    if (!AppConstants.MOZ_NORMANDY) {
      done();
      return;
    }

    const { PreferenceExperiments: NormandyPreferenceStudies } =
      ChromeUtils.importESModule(
        "resource://normandy/lib/PreferenceExperiments.sys.mjs"
      );
    const { AddonStudies: NormandyAddonStudies } = ChromeUtils.importESModule(
      "resource://normandy/lib/AddonStudies.sys.mjs"
    );
    const { PreferenceRollouts: NormandyPreferenceRollouts } =
      ChromeUtils.importESModule(
        "resource://normandy/lib/PreferenceRollouts.sys.mjs"
      );
    const { ExperimentAPI } = ChromeUtils.importESModule(
      "resource://nimbus/ExperimentAPI.sys.mjs"
    );

    const [
      addonStudies,
      prefRollouts,
      prefStudies,
      nimbusExperiments,
      nimbusRollouts,
    ] = await Promise.all(
      [
        NormandyAddonStudies.getAllActive(),
        NormandyPreferenceRollouts.getAllActive(),
        NormandyPreferenceStudies.getAllActive(),
        ExperimentAPI.manager.store
          .ready()
          .then(() => ExperimentAPI.manager.store.getAllActiveExperiments()),
        ExperimentAPI.manager.store
          .ready()
          .then(() => ExperimentAPI.manager.store.getAllActiveRollouts()),
      ].map(promise =>
        promise
          .catch(error => {
            console.error(error);
            return [];
          })
          .then(items => items.sort((a, b) => a.slug.localeCompare(b.slug)))
      )
    );

    done({
      addonStudies,
      prefRollouts,
      prefStudies,
      nimbusExperiments,
      nimbusRollouts,
    });
  },

  async remoteSettings(done) {
    const { RemoteSettings } = ChromeUtils.importESModule(
      "resource://services-settings/remote-settings.sys.mjs"
    );

    let inspected;
    try {
      inspected = await RemoteSettings.inspect({ localOnly: true });
    } catch (error) {
      console.error(error);
      done({ isSynchronizationBroken: true, history: { "settings-sync": [] } });
      return;
    }

    inspected.lastCheck = inspected.lastCheck
      ? new Date(inspected.lastCheck * 1000).toISOString()
      : "";
    for (let h of Object.values(inspected.history)) {
      h.splice(10, Infinity);
    }

    done(inspected);
  },
};
