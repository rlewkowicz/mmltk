/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const MAX_USER_CONTEXT_ID = -1 >>> 0;
const LAST_CONTAINERS_JSON_VERSION = 6;
const SAVE_DELAY_MS = 1500;
const CONTEXTUAL_IDENTITY_ENABLED_PREF = "privacy.userContext.enabled";

export const CONTAINER_COLORS = [
  {
    name: "gray",
    code: "#7c7c7d",
    codeNova: "#949297",
    l10nId: "user-context-color-gray",
  },
  {
    name: "yellow",
    code: "#ffcb00",
    codeNova: "#db820e",
    l10nId: "user-context-color-yellow",
  },
  {
    name: "orange",
    code: "#ff9f00",
    codeNova: "#f4682c",
    l10nId: "user-context-color-orange",
  },
  {
    name: "red",
    code: "#ff613d",
    codeNova: "#ed566e",
    l10nId: "user-context-color-red",
  },
  {
    name: "pink",
    code: "#ff4bda",
    codeNova: "#db54bf",
    l10nId: "user-context-color-pink",
  },
  {
    name: "purple",
    code: "#af51f5",
    codeNova: "#b864ee",
    l10nId: "user-context-color-purple",
  },
  {
    name: "violet",
    code: "#764edd",
    codeNova: "#9871ff",
    l10nId: "user-context-color-violet",
  },
  {
    name: "blue",
    code: "#37adff",
    codeNova: "#5a87fd",
    l10nId: "user-context-color-blue",
  },
  {
    name: "cyan",
    code: "#00c79a",
    codeNova: "#10a4ca",
    l10nId: "user-context-color-cyan",
  },
  {
    name: "green",
    code: "#51cd00",
    codeNova: "#11ae84",
    l10nId: "user-context-color-green",
  },
];

export const CONTAINER_COLOR_ALIASES = {
  __proto__: null,
  turquoise: "cyan",
  toolbar: "gray",
};

const CONTAINER_ICONS = [
  { name: "fingerprint", l10nId: "user-context-icon-fingerprint" },
  { name: "briefcase", l10nId: "user-context-icon-briefcase" },
  { name: "dollar", l10nId: "user-context-icon-dollar" },
  { name: "cart", l10nId: "user-context-icon-cart" },
  { name: "vacation", l10nId: "user-context-icon-vacation" },
  { name: "gift", l10nId: "user-context-icon-gift" },
  { name: "food", l10nId: "user-context-icon-food" },
  { name: "fruit", l10nId: "user-context-icon-fruit" },
  { name: "pet", l10nId: "user-context-icon-pet" },
  { name: "tree", l10nId: "user-context-icon-tree" },
  { name: "chill", l10nId: "user-context-icon-chill" },
  { name: "circle", l10nId: "user-context-icon-circle" },
  { name: "fence", l10nId: "user-context-icon-fence" },
];

const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "l10n", function () {
  return new Localization(["toolkit/global/contextual-identity.ftl"], true);
});

ChromeUtils.defineLazyGetter(lazy, "gTextDecoder", function () {
  return new TextDecoder();
});

ChromeUtils.defineLazyGetter(lazy, "gTextEncoder", function () {
  return new TextEncoder();
});

ChromeUtils.defineESModuleGetters(lazy, {
  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
  DeferredTask: "resource://gre/modules/DeferredTask.sys.mjs",
  FileUtils: "resource://gre/modules/FileUtils.sys.mjs",
  NetUtil: "resource://gre/modules/NetUtil.sys.mjs",
});

function _TabRemovalObserver(resolver, remoteTabIds) {
  this._resolver = resolver;
  this._remoteTabIds = remoteTabIds;
  Services.obs.addObserver(this, "ipc:browser-destroyed");
}

_TabRemovalObserver.prototype = {
  _resolver: null,
  _remoteTabIds: null,

  QueryInterface: ChromeUtils.generateQI(["nsIObserver"]),

  observe(subject) {
    let remoteTab = subject.QueryInterface(Ci.nsIRemoteTab);
    if (this._remoteTabIds.has(remoteTab.tabId)) {
      this._remoteTabIds.delete(remoteTab.tabId);
      if (this._remoteTabIds.size == 0) {
        Services.obs.removeObserver(this, "ipc:browser-destroyed");
        this._resolver();
      }
    }
  },
};

function _ContextualIdentityService(path) {
  this.init(path);
}

_ContextualIdentityService.prototype = {
  LAST_CONTAINERS_JSON_VERSION,

  _userIdentities: [
    {
      icon: "fingerprint",
      color: "blue",
      l10nId: "user-context-personal",
    },
    {
      icon: "briefcase",
      color: "orange",
      l10nId: "user-context-work",
    },
    {
      icon: "dollar",
      color: "green",
      l10nId: "user-context-banking",
    },
    {
      icon: "cart",
      color: "pink",
      l10nId: "user-context-shopping",
    },
  ],
  _systemIdentities: [
    {
      public: false,
      icon: "",
      color: "",
      name: "userContextIdInternal.thumbnail",
      accessKey: "",
    },
    {
      userContextId: MAX_USER_CONTEXT_ID,
      public: false,
      icon: "",
      color: "",
      name: "userContextIdInternal.webextStorageLocal",
      accessKey: "",
    },
  ],

  _defaultIdentities: [],

  _identities: null,
  _openedIdentities: new Set(),
  _lastUserContextId: 0,

  _path: null,
  _dataReady: false,

  _saver: null,

  init(path) {
    this._path = path;

    Services.prefs.addObserver(CONTEXTUAL_IDENTITY_ENABLED_PREF, this);

    this._defaultIdentities = [];
    let userContextId = 1;
    for (let identity of this._userIdentities) {
      identity.public = true;
      identity.userContextId = userContextId;
      userContextId++;
      this._defaultIdentities.push(identity);
    }
    for (let identity of this._systemIdentities) {
      if (!("userContextId" in identity)) {
        identity.userContextId = userContextId;
        userContextId++;
      }
      this._defaultIdentities.push(identity);
    }
  },

  async observe(aSubject, aTopic) {
    if (aTopic === "nsPref:changed") {
      const contextualIdentitiesEnabled = Services.prefs.getBoolPref(
        CONTEXTUAL_IDENTITY_ENABLED_PREF
      );
      if (!contextualIdentitiesEnabled) {
        await this.closeContainerTabs();
        this.notifyAllContainersCleared();
        this.resetDefault();
      }
    }
  },

  async load() {
    await IOUtils.read(this._path).then(
      bytes => {
        if (this._dataReady) {
          return;
        }

        try {
          this.parseData(bytes);
        } catch (error) {
          this.loadError(error);
        }
      },
      error => {
        this.loadError(error);
      }
    );

    Services.obs.notifyObservers(
      null,
      "contextual-identity-service-load-finished"
    );
  },

  resetDefault() {
    this._identities = [];

    this._lastUserContextId = this._defaultIdentities
      .filter(identity => identity.userContextId < MAX_USER_CONTEXT_ID)
      .map(identity => identity.userContextId)
      .sort((a, b) => a >= b)
      .pop();

    for (let identity of this._defaultIdentities) {
      this._identities.push(Object.assign({}, identity));
    }
    this._openedIdentities = new Set();

    this._dataReady = true;

    this.deleteContainerData();

    this.saveSoon();
  },

  loadError(error) {
    if (error != null && error.name != "NotFoundError") {
      console.error(error);
    }

    if (this._dataReady) {
      return;
    }

    this.resetDefault();
  },

  saveSoon() {
    if (!this._saver) {
      this._saverCallback = () => this._saver.finalize();

      this._saver = new lazy.DeferredTask(() => this.save(), SAVE_DELAY_MS);
      lazy.AsyncShutdown.profileBeforeChange.addBlocker(
        "ContextualIdentityService: writing data",
        this._saverCallback
      );
    } else {
      this._saver.disarm();
    }

    this._saver.arm();
  },

  save() {
    lazy.AsyncShutdown.profileBeforeChange.removeBlocker(this._saverCallback);

    this._saver = null;
    this._saverCallback = null;

    let object = {
      version: LAST_CONTAINERS_JSON_VERSION,
      lastUserContextId: this._lastUserContextId,
      identities: this._identities,
    };

    let bytes = lazy.gTextEncoder.encode(JSON.stringify(object));
    return IOUtils.write(this._path, bytes, {
      tmpPath: this._path + ".tmp",
    });
  },

  create(name, icon, color) {
    this.ensureDataReady();

    let userContextId = ++this._lastUserContextId;

    if (userContextId >= MAX_USER_CONTEXT_ID) {
      throw new Error(
        `Unable to create a new userContext with id '${userContextId}'`
      );
    }

    if (!name.trim()) {
      throw new Error(
        "Contextual identity names cannot contain only whitespace."
      );
    }

    let identity = {
      userContextId,
      public: true,
      icon,
      color,
      name,
    };

    this._identities.push(identity);
    this.saveSoon();
    Services.obs.notifyObservers(
      this.getIdentityObserverOutput(identity),
      "contextual-identity-created"
    );


    return Cu.cloneInto(identity, {});
  },

  update(userContextId, name, icon, color) {
    this.ensureDataReady();

    let identity = this._identities.find(
      i => i.userContextId == userContextId && i.public
    );

    if (!name.trim()) {
      throw new Error(
        "Contextual identity names cannot contain only whitespace."
      );
    }

    if (identity && name) {
      identity.name = name;
      identity.color = color;
      identity.icon = icon;
      delete identity.l10nId;

      this.saveSoon();
      Services.obs.notifyObservers(
        this.getIdentityObserverOutput(identity),
        "contextual-identity-updated"
      );
    }


    return !!identity;
  },

  move(userContextIds, position) {
    this.ensureDataReady();

    if (position < -1) {
      return false;
    }
    let movedIdentities = this._identities.filter(
      identity =>
        identity.public && userContextIds.includes(identity.userContextId)
    );

    if (movedIdentities.length) {
      if (position === -1) {
        position = this._identities.length;
      }
      position = this._identities.reduce(
        (dest, identity, idx) =>
          dest + (!identity.public && dest >= idx ? 1 : 0),
        position
      );
      this._identities = this._identities.filter(
        identity =>
          !identity.public || !userContextIds.includes(identity.userContextId)
      );
      this._identities.splice(position, 0, ...movedIdentities);
      this.saveSoon();
    }

    return !!movedIdentities.length;
  },

  remove(userContextId) {
    this.ensureDataReady();

    let index = this._identities.findIndex(
      i => i.userContextId == userContextId && i.public
    );
    if (index == -1) {
      return false;
    }

    Services.clearData.deleteDataFromOriginAttributesPattern({ userContextId });

    let deletedOutput = this.getIdentityObserverOutput(
      this.getPublicIdentityFromId(userContextId)
    );
    this._identities.splice(index, 1);
    this._openedIdentities.delete(userContextId);
    this.saveSoon();
    Services.obs.notifyObservers(deletedOutput, "contextual-identity-deleted");


    return true;
  },

  getIdentityObserverOutput(identity) {
    let wrappedJSObject = {
      name: this.getUserContextLabel(identity.userContextId),
      icon: identity.icon,
      color: identity.color,
      userContextId: identity.userContextId,
    };

    return { wrappedJSObject };
  },

  parseData(bytes) {
    let data = JSON.parse(lazy.gTextDecoder.decode(bytes));
    if (data.version == 1) {
      this.resetDefault();
      return;
    }

    let saveNeeded = false;

    if (data.version == 2) {
      data = this.migrate2to3(data);
      saveNeeded = true;
    }

    if (data.version == 3) {
      data = this.migrate3to4(data);
      saveNeeded = true;
    }

    if (data.version == 4) {
      data = this.migrate4to5(data);
      saveNeeded = true;
    }

    if (data.version == 5) {
      data = this.migrate5to6(data);
      saveNeeded = true;
    }

    if (data.version != LAST_CONTAINERS_JSON_VERSION) {
      dump(
        "ERROR - ContextualIdentityService - Unknown version found in " +
          this._path +
          "\n"
      );
      this.loadError(null);
      return;
    }

    this._identities = data.identities;
    this._lastUserContextId = data.lastUserContextId;

    if (saveNeeded) {
      this.saveSoon();
    }

    this._dataReady = true;
  },

  ensureDataReady() {
    if (this._dataReady) {
      return;
    }

    try {
      let file = new lazy.FileUtils.File(this._path);
      if (!file.exists()) {
        this.resetDefault();
        return;
      }
      let inputStream = Cc[
        "@mozilla.org/network/file-input-stream;1"
      ].createInstance(Ci.nsIFileInputStream);
      inputStream.init(
        file,
        lazy.FileUtils.MODE_RDONLY,
        lazy.FileUtils.PERMS_FILE,
        0
      );
      try {
        let bytes = lazy.NetUtil.readInputStream(
          inputStream,
          inputStream.available()
        );
        this.parseData(bytes);
      } finally {
        inputStream.close();
      }
    } catch (error) {
      this.loadError(error);
    }
  },

  getPublicUserContextIds() {
    return this._identities
      .filter(identity => identity.public)
      .map(identity => identity.userContextId);
  },

  getPrivateUserContextIds() {
    return this._identities
      .filter(identity => !identity.public)
      .map(identity => identity.userContextId);
  },

  getPublicIdentities() {
    this.ensureDataReady();
    return Cu.cloneInto(
      this._identities.filter(info => info.public),
      {}
    );
  },

  getPrivateIdentity(name) {
    this.ensureDataReady();
    return Cu.cloneInto(
      this._identities.find(info => !info.public && info.name == name),
      {}
    );
  },

  getDefaultPrivateIdentity(name) {
    return Cu.cloneInto(
      this._defaultIdentities.find(info => !info.public && info.name == name),
      {}
    );
  },

  getPublicIdentityFromId(userContextId) {
    this.ensureDataReady();
    return Cu.cloneInto(
      this._identities.find(
        info => info.userContextId == userContextId && info.public
      ),
      {}
    );
  },

  formatContextLabel(l10nId) {
    const [msg] = lazy.l10n.formatMessagesSync([l10nId]);
    for (let attr of msg.attributes) {
      if (attr.name === "label") {
        return attr.value;
      }
    }
    return "";
  },

  getUserContextLabel(userContextId) {
    let identity = this.getPublicIdentityFromId(userContextId);

    if (identity?.name) {
      return identity.name;
    }

    if (identity?.l10nId) {
      return this.formatContextLabel(identity.l10nId);
    }

    return "";
  },

  get containerColors() {
    return CONTAINER_COLORS.map(color => color.name);
  },

  get containerIcons() {
    return CONTAINER_ICONS.map(icon => icon.name);
  },

  resolveContainerColor(color) {
    return CONTAINER_COLOR_ALIASES[color] ?? color;
  },

  getContainerColorCode(color) {
    let entry = CONTAINER_COLORS.find(e => e.name === color);
    if (!entry) {
      return null;
    }
    return Services.prefs.getBoolPref("browser.nova.enabled", false)
      ? entry.codeNova
      : entry.code;
  },

  getContainerIconURL(icon) {
    if (!CONTAINER_ICONS.some(entry => entry.name === icon)) {
      return null;
    }
    return `resource://usercontext-content/${icon}.svg`;
  },

  getContainerColorL10nId(color) {
    return CONTAINER_COLORS.find(entry => entry.name === color)?.l10nId ?? null;
  },

  getContainerIconL10nId(icon) {
    return CONTAINER_ICONS.find(entry => entry.name === icon)?.l10nId ?? null;
  },

  getContainerColorLabel(color) {
    let l10nId = this.getContainerColorL10nId(color);
    return l10nId ? this.formatContextLabel(l10nId) : "";
  },

  getContainerIconLabel(icon) {
    let l10nId = this.getContainerIconL10nId(icon);
    return l10nId ? this.formatContextLabel(l10nId) : "";
  },

  setTabStyle(tab) {
    if (!tab.hasAttribute("usercontextid")) {
      return;
    }

    let userContextId = tab.getAttribute("usercontextid");
    let identity = this.getPublicIdentityFromId(userContextId);

    let prefix = "identity-color-";
    for (let className of tab.classList) {
      if (className.startsWith(prefix)) {
        tab.classList.remove(className);
      }
    }
    if (identity && identity.color) {
      tab.classList.add(prefix + identity.color);
    }
  },

  countContainerTabs(userContextId = 0) {
    let count = 0;
    this._forEachContainerTab(function () {
      ++count;
    }, userContextId);
    return count;
  },

  closeContainerTabs(userContextId = 0, removeTabOptions = {}) {
    return new Promise(resolve => {
      let remoteTabIds = new Set();
      this._forEachContainerTab((tab, tabbrowser) => {
        let frameLoader = tab.linkedBrowser.frameLoader;

        if (frameLoader?.remoteTab) {
          remoteTabIds.add(frameLoader.remoteTab.tabId);
        }

        tabbrowser.removeTab(tab, removeTabOptions);
      }, userContextId);

      if (remoteTabIds.size == 0) {
        resolve();
        return;
      }

      new _TabRemovalObserver(resolve, remoteTabIds);
    });
  },

  notifyAllContainersCleared() {
    for (let identity of this._identities) {
      if (!identity.public) {
        continue;
      }
      Services.clearData.deleteDataFromOriginAttributesPattern({
        userContextId: identity.userContextId,
      });
    }
  },

  _forEachContainerTab(callback, userContextId = 0) {
    for (let win of Services.wm.getEnumerator("navigator:browser")) {
      if (win.closed || !win.gBrowser) {
        continue;
      }

      let tabbrowser = win.gBrowser;
      for (let i = tabbrowser.tabs.length - 1; i >= 0; --i) {
        let tab = tabbrowser.tabs[i];
        if (
          tab.hasAttribute("usercontextid") &&
          (!userContextId ||
            parseInt(tab.getAttribute("usercontextid"), 10) == userContextId)
        ) {
          callback(tab, tabbrowser);
        }
      }
    }
  },

  createNewInstanceForTesting(path) {
    return new _ContextualIdentityService(path);
  },

  deleteContainerData() {
    let minUserContextId = 1;

    const keepDataContextIds = this.getPrivateUserContextIds();

    let cookiesUserContextIds = new Set();

    for (let cookie of Services.cookies.cookies) {
      if (
        cookie.originAttributes.userContextId >= minUserContextId &&
        !keepDataContextIds.includes(cookie.originAttributes.userContextId)
      ) {
        cookiesUserContextIds.add(cookie.originAttributes.userContextId);
      }
    }

    for (let userContextId of cookiesUserContextIds) {
      Services.clearData.deleteDataFromOriginAttributesPattern({
        userContextId,
      });
    }
  },

  migrate2to3(data) {
    data.version = 3;

    return data;
  },

  migrate3to4(data) {
    const webextStorageLocalIdentity = this.getDefaultPrivateIdentity(
      "userContextIdInternal.webextStorageLocal"
    );

    data.identities.push(webextStorageLocalIdentity);

    data.version = 4;

    return data;
  },

  migrate4to5(data) {

    for (let identity of data.identities) {
      switch (identity.l10nID) {
        case "userContextPersonal.label":
          identity.l10nId = "user-context-personal";
          break;
        case "userContextWork.label":
          identity.l10nId = "user-context-work";
          break;
        case "userContextBanking.label":
          identity.l10nId = "user-context-banking";
          break;
        case "userContextShopping.label":
          identity.l10nId = "user-context-shopping";
          break;
      }
      delete identity.l10nID;
      delete identity.accessKey;
    }

    data.version = 5;

    return data;
  },

  migrate5to6(data) {
    for (let identity of data.identities) {
      if (identity.color) {
        identity.color = this.resolveContainerColor(identity.color);
      }
    }

    data.version = 6;

    return data;
  },
};

let path = PathUtils.join(
  Services.dirsvc.get("ProfD", Ci.nsIFile).path,
  "containers.json"
);
export var ContextualIdentityService = new _ContextualIdentityService(path);
