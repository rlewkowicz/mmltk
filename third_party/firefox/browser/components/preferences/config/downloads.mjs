/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


import { Preferences } from "chrome://global/content/preferences/Preferences.mjs";
import { SettingGroupManager } from "chrome://browser/content/preferences/config/SettingGroupManager.mjs";


var { AppConstants } = ChromeUtils.importESModule(
  "resource://gre/modules/AppConstants.sys.mjs"
);
var { getMozRemoteImageURL } = ChromeUtils.importESModule(
  "moz-src:///toolkit/modules/FaviconUtils.sys.mjs"
);

const gHandlerService = Cc[
  "@mozilla.org/uriloader/handler-service;1"
].getService(Ci.nsIHandlerService);

const gMIMEService = Cc["@mozilla.org/mime;1"].getService(Ci.nsIMIMEService);

const lazy = {};

var Integration = ChromeUtils.importESModule(
  "resource://gre/modules/Integration.sys.mjs"
).Integration;
Integration.downloads.defineESModuleGetter(
  lazy,
  "DownloadIntegration",
  "resource://gre/modules/DownloadIntegration.sys.mjs"
);

let gGIOService = null;

if (Cc["@mozilla.org/gio-service;1"]) {
  gGIOService = Cc["@mozilla.org/gio-service;1"].getService(Ci.nsIGIOService);
}

var { FileUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/FileUtils.sys.mjs"
);

var Downloads = ChromeUtils.importESModule(
  "resource://gre/modules/Downloads.sys.mjs"
).Downloads;

Preferences.addAll([
  { id: "browser.download.useDownloadDir", type: "bool", inverted: true },
  { id: "browser.download.enableDeletePrivate", type: "bool" },
  { id: "browser.download.deletePrivate", type: "bool" },
  { id: "browser.download.always_ask_before_handling_new_types", type: "bool" },
  { id: "browser.download.folderList", type: "int" },
  { id: "browser.download.dir", type: "file" },
  { id: "pref.downloads.disable_button.edit_actions", type: "bool" },
]);


let DownloadsHelpers = new (class DownloadsHelpers {
  folder;
  folderPath;
  folderHostPath;
  displayName;
  downloadsDir;
  desktopDir;
  downloadsFolderLocalizedName;
  desktopFolderLocalizedName;

  setupDownloadsHelpersFields = async () => {
    this.downloadsDir = await this._getDownloadsFolder("Downloads");
    this.desktopDir = await this._getDownloadsFolder("Desktop");
    [this.downloadsFolderLocalizedName, this.desktopFolderLocalizedName] =
      await document.l10n.formatValues([
        { id: "downloads-folder-name" },
        { id: "desktop-folder-name" },
      ]);
  };

  async _getDownloadsFolder(aFolder) {
    switch (aFolder) {
      case "Desktop":
        return Services.dirsvc.get("Desk", Ci.nsIFile);
      case "Downloads": {
        let downloadsDir = await Downloads.getSystemDownloadsDirectory();
        return new FileUtils.File(downloadsDir);
      }
    }
    throw new Error(
      "ASSERTION FAILED: folder type should be 'Desktop' or 'Downloads'"
    );
  }

  _getSystemDownloadFolderDetails(folderIndex) {
    let currentDirPref = Preferences.get("browser.download.dir");

    let file;
    let firefoxLocalizedName;
    if (folderIndex == 2 && currentDirPref.value) {
      file = currentDirPref.value;
      if (file.equals(this.downloadsDir)) {
        folderIndex = 1;
      } else if (file.equals(this.desktopDir)) {
        folderIndex = 0;
      }
    }
    switch (folderIndex) {
      case 2: 
        break;

      case 1: {
        file = this.downloadsDir;
        firefoxLocalizedName = this.downloadsFolderLocalizedName;
        break;
      }

      case 0:
      // fall through
      default: {
        file = this.desktopDir;
        firefoxLocalizedName = this.desktopFolderLocalizedName;
      }
    }

    if (file) {
      let displayName = file.path;

      if (AppConstants.platform == "linux") {
        if (this.folderHostPath && displayName == this.folderPath) {
          displayName = this.folderHostPath;
          if (displayName == this.downloadsDir.path) {
            firefoxLocalizedName = this.downloadsFolderLocalizedName;
          } else if (displayName == this.desktopDir.path) {
            firefoxLocalizedName = this.desktopFolderLocalizedName;
          }
        } else if (displayName != this.folderPath) {
          this.folderHostPath = null;
          try {
            file.hostPath().then(folderHostPath => {
              this.folderHostPath = folderHostPath;
              Preferences.getSetting("downloadFolder")?.onChange();
            });
          } catch (error) {
          }
        }
      }

      if (firefoxLocalizedName) {
        let folderDisplayName, leafName;
        try {
          folderDisplayName = file.displayName;
        } catch (ex) {
        }
        try {
          leafName = file.leafName;
        } catch (ex) {
        }

        if (folderDisplayName && folderDisplayName != leafName) {
          return { file, folderDisplayName };
        }

        if (
          AppConstants.platform == "macosx" ||
          leafName == firefoxLocalizedName
        ) {
          return { file, folderDisplayName: firefoxLocalizedName };
        }
      }

      return { file, folderDisplayName: `\u2066${displayName}\u2069` };
    }

    file = this.desktopDir;
    return { file, folderDisplayName: "" };
  }

  _folderToIndex(aFolder) {
    if (!aFolder || aFolder.equals(this.desktopDir)) {
      return 0;
    } else if (aFolder.equals(this.downloadsDir)) {
      return 1;
    }
    return 2;
  }

  getFolderDetails() {
    let folderIndex = Preferences.get("browser.download.folderList").value;
    let { folderDisplayName, file } =
      this._getSystemDownloadFolderDetails(folderIndex);

    this.folderPath = file?.path ?? "";
    this.displayName = folderDisplayName;
  }

  setFolder(folder) {
    this.folder = folder;

    let folderListPref = Preferences.get("browser.download.folderList");
    folderListPref.value = this._folderToIndex(this.folder);
  }
})();

Preferences.addSetting({
  id: "browserDownloadFolderList",
  pref: "browser.download.folderList",
});
Preferences.addSetting({
  id: "downloadFolder",
  pref: "browser.download.dir",
  deps: ["browserDownloadFolderList"],
  get() {
    DownloadsHelpers.getFolderDetails();
    return DownloadsHelpers.folderPath;
  },
  set(folder) {
    DownloadsHelpers.setFolder(folder);
    return DownloadsHelpers.folder;
  },
  getControlConfig(config) {
    if (DownloadsHelpers.displayName) {
      return {
        ...config,
        controlAttrs: {
          ...config.controlAttrs,
          ".displayValue": DownloadsHelpers.displayName,
        },
      };
    }
    return {
      ...config,
    };
  },
  setup(emitChange) {
    DownloadsHelpers.setupDownloadsHelpersFields().then(emitChange);
  },
  disabled: ({ browserDownloadFolderList }) => {
    return browserDownloadFolderList.locked;
  },
});
Preferences.addSetting({
  id: "alwaysAsk",
  pref: "browser.download.useDownloadDir",
});

Preferences.addSetting({
  id: "applicationsHandlersView",
  setup: emitChange => {
    emitChange();

    async function appInitializer(event) {
      if (
        event.detail.category != "paneDownloads" &&
        event.detail.category != "paneSearchResults"
      ) {
        return;
      }
      await ApplicationsHandler.preInitApplications();
      Services.obs.notifyObservers(window, "app-handler-loaded");
      window.removeEventListener("paneshown", appInitializer);
    }
    window.addEventListener("paneshown", appInitializer);
    return () => {
      window.removeEventListener("paneshown", appInitializer);
    };
  },
});

Preferences.addSetting({
  id: "applicationsGroup",
});

Preferences.addSetting({
  id: "applicationsFilter",
  get(val) {
    return val || "";
  },
});

Preferences.addSetting({
  id: "handleNewFileTypes",
  pref: "browser.download.always_ask_before_handling_new_types",
});

Preferences.addSetting({
  id: "enableDeletePrivate",
  pref: "browser.download.enableDeletePrivate",
});
Preferences.addSetting({
  id: "deletePrivate",
  pref: "browser.download.deletePrivate",
  deps: ["enableDeletePrivate"],
  visible: ({ enableDeletePrivate }) => enableDeletePrivate.value,
  onUserChange() {
    Services.prefs.setBoolPref("browser.download.deletePrivate.chosen", true);
  },
});

const ICON_URL_APP =
  AppConstants.platform == "linux"
    ? "moz-icon://dummy.exe?size=16"
    : "chrome://browser/skin/preferences/application.png";

export class HandlerInfoWrapper {
  wrappedHandlerInfo;

  constructor(type, handlerInfo) {
    this.type = type;
    this.wrappedHandlerInfo = handlerInfo;
    this.disambiguateDescription = false;
  }

  get description() {
    if (this.wrappedHandlerInfo.description) {
      return { raw: this.wrappedHandlerInfo.description };
    }

    if (this.primaryExtension) {
      var extension = this.primaryExtension.toUpperCase();
      return { id: "applications-file-ending", args: { extension } };
    }

    return { raw: this.type };
  }

  get typeDescription() {
    if (this.disambiguateDescription) {
      const description = this.description;
      if (description.id) {
        let { args = {} } = description;
        args.type = this.type;
        return {
          id: description.id + "-with-type",
          args,
        };
      }

      return {
        id: "applications-type-description-with-type",
        args: {
          "type-description": description.raw,
          type: this.type,
        },
      };
    }

    return this.description;
  }

  get actionIconClass() {
    if (this.alwaysAskBeforeHandling) {
      return "ask";
    }

    switch (this.preferredAction) {
      case Ci.nsIHandlerInfo.saveToDisk:
        return "save";

      case Ci.nsIHandlerInfo.handleInternally:
        if (this instanceof InternalHandlerInfoWrapper) {
          return "handleInternally";
        }
        break;
    }

    return "";
  }

  get actionIconSrcset() {
    let icon = this.actionIcon;
    if (!icon || !icon.startsWith("moz-icon:")) {
      return icon;
    }
    let srcset = [];
    for (let scale of [1, 2, 3]) {
      let scaledIcon = icon + "&scale=" + scale;
      srcset.push(`${scaledIcon} ${scale}x`);
    }
    return srcset.join(", ");
  }

  get actionIcon() {
    switch (this.preferredAction) {
      case Ci.nsIHandlerInfo.useSystemDefault:
        return this.iconURLForSystemDefault;

      case Ci.nsIHandlerInfo.useHelperApp: {
        let preferredApp = this.preferredApplicationHandler;
        if (ApplicationsHandler.isValidHandlerApp(preferredApp)) {
          return getIconURLForHandlerApp(preferredApp);
        }
      }
      default:
        return ICON_URL_APP;
    }
  }

  get iconURLForSystemDefault() {
    if (
      this.wrappedHandlerInfo instanceof Ci.nsIMIMEInfo &&
      this.wrappedHandlerInfo instanceof Ci.nsIPropertyBag
    ) {
      try {
        let url = this.wrappedHandlerInfo.getProperty(
          "defaultApplicationIconURL"
        );
        if (url) {
          return url + "?size=16";
        }
      } catch (ex) {}
    }

    return ICON_URL_APP;
  }

  get preferredApplicationHandler() {
    return this.wrappedHandlerInfo.preferredApplicationHandler;
  }

  set preferredApplicationHandler(aNewValue) {
    this.wrappedHandlerInfo.preferredApplicationHandler = aNewValue;

    if (aNewValue) {
      this.addPossibleApplicationHandler(aNewValue);
    }
  }

  get possibleApplicationHandlers() {
    return this.wrappedHandlerInfo.possibleApplicationHandlers;
  }

  addPossibleApplicationHandler(aNewHandler) {
    for (let app of this.possibleApplicationHandlers.enumerate()) {
      if (app.equals(aNewHandler)) {
        return;
      }
    }
    this.possibleApplicationHandlers.appendElement(aNewHandler);
  }

  removePossibleApplicationHandler(aHandler) {
    var defaultApp = this.preferredApplicationHandler;
    if (defaultApp && aHandler.equals(defaultApp)) {
      this.alwaysAskBeforeHandling = true;
      this.preferredApplicationHandler = null;
    }

    var handlers = this.possibleApplicationHandlers;
    for (var i = 0; i < handlers.length; ++i) {
      var handler = handlers.queryElementAt(i, Ci.nsIHandlerApp);
      if (handler.equals(aHandler)) {
        handlers.removeElementAt(i);
        break;
      }
    }
  }

  get hasDefaultHandler() {
    return this.wrappedHandlerInfo.hasDefaultHandler;
  }

  get defaultDescription() {
    return this.wrappedHandlerInfo.defaultDescription;
  }

  get preferredAction() {
    if (
      this.wrappedHandlerInfo.preferredAction ==
        Ci.nsIHandlerInfo.useHelperApp &&
      !ApplicationsHandler.isValidHandlerApp(this.preferredApplicationHandler)
    ) {
      if (this.wrappedHandlerInfo.hasDefaultHandler) {
        return Ci.nsIHandlerInfo.useSystemDefault;
      }
      return Ci.nsIHandlerInfo.saveToDisk;
    }

    return this.wrappedHandlerInfo.preferredAction;
  }

  set preferredAction(aNewValue) {
    this.wrappedHandlerInfo.preferredAction = aNewValue;
  }

  get alwaysAskBeforeHandling() {
    if (
      !(this.wrappedHandlerInfo instanceof Ci.nsIMIMEInfo) &&
      this.preferredAction == Ci.nsIHandlerInfo.saveToDisk
    ) {
      return true;
    }

    return this.wrappedHandlerInfo.alwaysAskBeforeHandling;
  }

  set alwaysAskBeforeHandling(aNewValue) {
    this.wrappedHandlerInfo.alwaysAskBeforeHandling = aNewValue;
  }

  get primaryExtension() {
    try {
      if (
        this.wrappedHandlerInfo instanceof Ci.nsIMIMEInfo &&
        this.wrappedHandlerInfo.primaryExtension
      ) {
        return this.wrappedHandlerInfo.primaryExtension;
      }
    } catch (ex) {}

    return null;
  }

  store() {
    gHandlerService.store(this.wrappedHandlerInfo);
  }

  get iconSrcSet() {
    let srcset = [];
    for (let scale of [1, 2]) {
      let icon = this._getIcon(16, scale);
      if (!icon) {
        return null;
      }
      srcset.push(`${icon} ${scale}x`);
    }
    return srcset.join(", ");
  }

  _getIcon(aSize, aScale = 1) {
    if (this.primaryExtension) {
      return `moz-icon://goat.${this.primaryExtension}?size=${aSize}&scale=${aScale}`;
    }

    if (this.wrappedHandlerInfo instanceof Ci.nsIMIMEInfo) {
      return `moz-icon://goat?size=${aSize}&scale=${aScale}&contentType=${this.type}`;
    }

    return null;
  }
}

export class InternalHandlerInfoWrapper extends HandlerInfoWrapper {
  constructor(mimeType, extension) {
    let type = gMIMEService.getFromTypeAndExtension(mimeType, extension);
    super(mimeType || type.type, type);
  }

  store() {
    super.store();
  }

  get preventInternalViewing() {
    return false;
  }

  get enabled() {
    throw Components.Exception("", Cr.NS_ERROR_NOT_IMPLEMENTED);
  }
}

class PDFHandlerInfoWrapper extends InternalHandlerInfoWrapper {
  constructor() {
    super("application/pdf", null);
  }

  get preventInternalViewing() {
    return Services.prefs.getBoolPref("pdfjs.disabled");
  }

  get enabled() {
    return true;
  }
}

class ViewableInternallyHandlerInfoWrapper extends InternalHandlerInfoWrapper {
  get enabled() {
    return lazy.DownloadIntegration.shouldViewDownloadInternally(this.type);
  }
}

export const HandlerServiceHelpers = {
  loadInternalHandlers(handledTypes) {
    let internalHandlers = [new PDFHandlerInfoWrapper()];

    let enabledHandlers = Services.prefs
      .getCharPref("browser.download.viewableInternally.enabledTypes", "")
      .trim();
    if (enabledHandlers) {
      for (let ext of enabledHandlers.split(",")) {
        internalHandlers.push(
          new ViewableInternallyHandlerInfoWrapper(null, ext.trim())
        );
      }
    }

    for (let internalHandler of internalHandlers) {
      if (internalHandler.enabled) {
        handledTypes[internalHandler.type] = internalHandler;
      }
    }
  },
  loadApplicationHandlers(handledTypes) {
    for (let wrappedHandlerInfo of gHandlerService.enumerate()) {
      let type = wrappedHandlerInfo.type;
      let handlerInfoWrapper;
      if (type in handledTypes) {
        handlerInfoWrapper = handledTypes[type];
      } else {
        if (lazy.DownloadIntegration.shouldViewDownloadInternally(type)) {
          handlerInfoWrapper = new ViewableInternallyHandlerInfoWrapper(type);
        } else {
          handlerInfoWrapper = new HandlerInfoWrapper(type, wrappedHandlerInfo);
        }
        handledTypes[type] = handlerInfoWrapper;
      }
    }
  },
};


export function getFileDisplayName(file) {
  if (AppConstants.platform == "win") {
    if (file instanceof Ci.nsILocalFileWin) {
      try {
        return file.getVersionInfoField("FileDescription");
      } catch (e) {}
    }
  }
  if (AppConstants.platform == "macosx") {
    if (file instanceof Ci.nsILocalFileMac) {
      try {
        return file.bundleDisplayName;
      } catch (e) {}
    }
  }
  return file.leafName;
}

export function getLocalHandlerApp(aFile) {
  var localHandlerApp = Cc[
    "@mozilla.org/uriloader/local-handler-app;1"
  ].createInstance(Ci.nsILocalHandlerApp);
  localHandlerApp.name = getFileDisplayName(aFile);
  localHandlerApp.executable = aFile;

  return localHandlerApp;
}

function getIconURLForAppId(aAppId) {
  return "moz-icon://" + aAppId + "?size=16";
}

function getIconURLForFile(aFile) {
  var fph = Services.io
    .getProtocolHandler("file")
    .QueryInterface(Ci.nsIFileProtocolHandler);
  var urlSpec = fph.getURLSpecFromActualFile(aFile);

  return "moz-icon://" + urlSpec + "?size=16";
}

export function getIconURLForHandlerApp(aHandlerApp) {
  if (aHandlerApp instanceof Ci.nsILocalHandlerApp) {
    return getIconURLForFile(aHandlerApp.executable);
  }

  if (aHandlerApp instanceof Ci.nsIWebHandlerApp) {
    return getIconURLForWebApp(aHandlerApp.uriTemplate);
  }

  if (aHandlerApp instanceof Ci.nsIGIOHandlerApp) {
    return getIconURLForAppId(aHandlerApp.id);
  }

  return "";
}

export function getIconURLForWebApp(aWebAppURITemplate) {
  var uri = Services.io.newURI(aWebAppURITemplate);


  if (
    /^https?$/.test(uri.scheme) &&
    Services.prefs.getBoolPref("browser.chrome.site_icons")
  ) {
    return getMozRemoteImageURL(uri.prePath + "/favicon.ico", { size: 16 });
  }

  return "";
}

async function setLocalizedLabel(item, l10n) {
  let label;
  if (l10n.hasOwnProperty("raw")) {
    label = l10n.raw;
  } else {
    [label] = await document.l10n.formatValues([l10n]);
  }
  item.removeAttribute("data-l10n-id");
  item.setAttribute("label", label);
}

var gNodeToObjectMap = new WeakMap();

export class ApplicationListItem {
  static forNode(node) {
    return gNodeToObjectMap.get(node);
  }

  constructor(handlerInfoWrapper) {
    this.handlerInfoWrapper = handlerInfoWrapper;
  }

  actionsMenuOptionCount = 0;

  setOrRemoveAttributes(iterable) {
    for (let [element, attributeName, value] of iterable) {
      let node = element || this.node;
      if (value) {
        node.setAttribute(attributeName, value);
      } else {
        node.removeAttribute(attributeName);
      }
    }
  }

  async createNode() {
    this.node =  (
      document.createElement("moz-box-item")
    );

    const iconSrc = this.handlerInfoWrapper._getIcon(16, 1);
    if (iconSrc) {
      this.node.setAttribute("iconsrc", iconSrc);
    }

    this.setOrRemoveAttributes([[null, "type", this.handlerInfoWrapper.type]]);

    let typeDescription = this.handlerInfoWrapper.typeDescription;
    await setLocalizedLabel(this.node, typeDescription);

    this.actionsMenu =  (
      document.createElement("moz-select")
    );
    this.actionsMenu.slot = "actions";
    this.actionsMenu.classList.add("actionsMenu");

    this.node.appendChild(this.actionsMenu);

    this.buildActionsMenu();

    gNodeToObjectMap.set(this.node, this);
    return this.node;
  }

  _buildActionsMenuOption({
    iconSrc,
    l10nId,
    value,
    handlerActionId: handlerActionNumber,
    l10nIdArgs = {},
  }) {
    const option =  (
      document.createElement("moz-option")
    );
    value = value ? value : this.actionsMenuOptionCount++ + "";
    option.setAttribute("value", value);
    document.l10n.setAttributes(option, l10nId, l10nIdArgs);
    if (iconSrc) {
      option.setAttribute("iconsrc", iconSrc);
    }
    const action =
      handlerActionNumber || handlerActionNumber === 0
        ? handlerActionNumber + ""
        : "";
    if (action) {
      option.setAttribute("action", action);
    }
    return option;
  }

  _getSaveFileIcon() {
    if (AppConstants.platform == "linux") {
      return "moz-icon://stock/document-save?size=16";
    }
    return "chrome://browser/skin/preferences/saveFile.png";
  }

  _isInternalMenuItem(handlerInfo) {
    return (
      handlerInfo instanceof InternalHandlerInfoWrapper &&
      !handlerInfo.preventInternalViewing
    );
  }

  _buildActionsMenuDefaultItem(handlerInfo) {
    if (!handlerInfo.hasDefaultHandler) {
      return undefined;
    }
    const defaultMenuItem = this._buildActionsMenuOption({
      iconSrc: ICON_URL_APP,
      handlerActionId: Ci.nsIHandlerInfo.useSystemDefault,
    });
    if (this._isInternalMenuItem(handlerInfo)) {
      document.l10n.setAttributes(
        defaultMenuItem,
        "applications-use-os-default"
      );
    } else {
      document.l10n.setAttributes(
        defaultMenuItem,
        "applications-use-app-default",
        {
          "app-name": handlerInfo.defaultDescription,
        }
      );
    }
    let image = handlerInfo.iconURLForSystemDefault;
    if (image) {
      defaultMenuItem.setAttribute("iconsrc", image);
    }
    return defaultMenuItem;
  }

  buildActionsMenu() {
    const { handlerInfoWrapper: handlerInfo } = this;

    while (this.actionsMenu.hasChildNodes()) {
      this.actionsMenu.removeChild(this.actionsMenu.lastChild);
    }
    this.actionsMenuOptionCount = 0;

    let internalMenuItem;
    if (this._isInternalMenuItem(handlerInfo)) {
      internalMenuItem = this._buildActionsMenuOption({
        l10nId: "applications-open-inapp",
        iconSrc: "chrome://branding/content/icon32.png",
        handlerActionId: Ci.nsIHandlerInfo.handleInternally,
      });

      this.actionsMenu.appendChild(internalMenuItem);
    }

    const askMenuItem = this._buildActionsMenuOption({
      iconSrc: "chrome://browser/skin/preferences/alwaysAsk.png",
      l10nId: "applications-always-ask",
      handlerActionId: Ci.nsIHandlerInfo.alwaysAsk,
    });
    this.actionsMenu.appendChild(askMenuItem);

    let saveMenuItem;
    if (handlerInfo.wrappedHandlerInfo instanceof Ci.nsIMIMEInfo) {
      saveMenuItem = this._buildActionsMenuOption({
        l10nId: "applications-action-save",
        iconSrc: this._getSaveFileIcon(),
        handlerActionId: Ci.nsIHandlerInfo.saveToDisk,
      });
      saveMenuItem.className = "menuitem-iconic";
      this.actionsMenu.appendChild(saveMenuItem);
    }

    this.actionsMenu.appendChild(document.createElement("hr"));

    let defaultMenuItem = this._buildActionsMenuDefaultItem(handlerInfo);
    if (defaultMenuItem) {
      this.actionsMenu.appendChild(defaultMenuItem);
    }

    let preferredApp = handlerInfo.preferredApplicationHandler;
    var possibleAppMenuItems = [];
    for (let possibleApp of handlerInfo.possibleApplicationHandlers.enumerate()) {
      if (!ApplicationsHandler.isValidHandlerApp(possibleApp)) {
        continue;
      }

      let label;
      if (possibleApp instanceof Ci.nsILocalHandlerApp) {
        label = getFileDisplayName(possibleApp.executable);
      } else {
        label = possibleApp.name;
      }
      let menuItem = this._buildActionsMenuOption({
        l10nId: "applications-use-app",
        iconSrc: getIconURLForHandlerApp(possibleApp),
        handlerActionId: Ci.nsIHandlerInfo.useHelperApp,
        l10nIdArgs: {
          "app-name": label,
        },
      });

      menuItem.handlerApp = possibleApp;

      this.actionsMenu.appendChild(menuItem);
      possibleAppMenuItems.push(menuItem);
    }
    if (gGIOService) {
      var gioApps = gGIOService.getAppsForURIScheme(handlerInfo.type);
      let possibleHandlers = handlerInfo.possibleApplicationHandlers;
      for (let handler of gioApps.enumerate(Ci.nsIHandlerApp)) {
        if (handler.name == handlerInfo.defaultDescription) {
          continue;
        }
        let appAlreadyInHandlers = false;
        for (let i = possibleHandlers.length - 1; i >= 0; --i) {
          let app = possibleHandlers.queryElementAt(i, Ci.nsIHandlerApp);
          if (handler.equals(app)) {
            appAlreadyInHandlers = true;
            break;
          }
        }
        if (!appAlreadyInHandlers) {
          const menuItem = this._buildActionsMenuOption({
            value: Ci.nsIHandlerInfo.useHelperApp + "",
            l10nId: "applications-use-app",
            iconSrc: getIconURLForHandlerApp(handler),
            handlerActionId: Ci.nsIHandlerInfo.useHelperApp,
            l10nIdArgs: {
              "app-name": handler.name,
            },
          });
          menuItem.handlerApp = handler;

          this.actionsMenu.appendChild(menuItem);
          possibleAppMenuItems.push(menuItem);
        }
      }
    }

    let canOpenWithOtherApp = true;
    if (AppConstants.platform == "win") {
      let executableType = Cc["@mozilla.org/mime;1"]
        .getService(Ci.nsIMIMEService)
        .getTypeFromExtension("exe");
      canOpenWithOtherApp = handlerInfo.type != executableType;
    }
    if (canOpenWithOtherApp) {
      let menuItem = this._buildActionsMenuOption({
        value: "choose-app",
        l10nId: "applications-use-other",
      });
      menuItem.className = "choose-app-item";
      this.actionsMenu.appendChild(menuItem);
    }

    if (possibleAppMenuItems.length) {
      this.actionsMenu.appendChild(document.createElement("hr"));

      const menuItem = this._buildActionsMenuOption({
        value: "manage-app",
        l10nId: "applications-manage-app",
      });
      menuItem.className = "manage-app-item";
      this.actionsMenu.appendChild(menuItem);
    }

    if (handlerInfo.alwaysAskBeforeHandling) {
      this.actionsMenu.value = askMenuItem.value;
    } else {
      const kActionUsePlugin = 5;

      switch (handlerInfo.preferredAction) {
        case Ci.nsIHandlerInfo.handleInternally:
          if (internalMenuItem) {
            this.actionsMenu.value = internalMenuItem.value;
          } else {
            console.error("No menu item defined to set!");
          }
          break;
        case Ci.nsIHandlerInfo.useSystemDefault:
          this.actionsMenu.value = defaultMenuItem
            ? defaultMenuItem.value
            : askMenuItem.value;
          break;
        case Ci.nsIHandlerInfo.useHelperApp:
          if (preferredApp) {
            let preferredItem = possibleAppMenuItems.find(v =>
              v.handlerApp.equals(preferredApp)
            );
            if (preferredItem) {
              this.actionsMenu.value = preferredItem.value;
            } else {
              let possible = possibleAppMenuItems
                .map(v => v.handlerApp && v.handlerApp.name)
                .join(", ");
              console.error(
                new Error(
                  `Preferred handler for ${handlerInfo.type} not in list of possible handlers!? (List: ${possible})`
                )
              );
              this.actionsMenu.value = askMenuItem.value;
            }
          }
          break;
        case kActionUsePlugin:
          this.actionsMenu.value = askMenuItem.value;
          break;
        case Ci.nsIHandlerInfo.saveToDisk:
          if (saveMenuItem) {
            this.actionsMenu.value = saveMenuItem.value;
          }
          break;
      }
    }
  }
}

const ApplicationsHandler = (function () {
  return new (class Handler {
    _handledTypes = {};

    _visibleTypes = [];

    selectedHandlerListItem = null;

    items = [];

    initialized = false;

    get _list() {
      return  (
        document.getElementById("applicationsHandlersView")
      );
    }

    get _filter() {
      return  (
        document.getElementById("applicationsFilter")
      );
    }

    async preInitApplications() {
      if (this.initialized) {
        return;
      }
      this.initialized = true;

      if (!this._list) {
        return;
      }

      HandlerServiceHelpers.loadInternalHandlers(this._handledTypes);
      HandlerServiceHelpers.loadApplicationHandlers(this._handledTypes);
      await this._list.updateComplete;

      this.headerElement = this._buildHeader();
      this._list.appendChild(this.headerElement);
      await this._rebuildVisibleTypes();
      await this._buildView();
    }

    async _rebuildVisibleTypes() {
      this._visibleTypes = [];

      let visibleDescriptions = new Map();
      for (let type in this._handledTypes) {
        await new Promise(resolve => Services.tm.dispatchToMainThread(resolve));

        let handlerInfo = this._handledTypes[type];

        this._visibleTypes.push(handlerInfo);

        let key = JSON.stringify(handlerInfo.description);
        let otherHandlerInfo = visibleDescriptions.get(key);
        if (!otherHandlerInfo) {
          handlerInfo.disambiguateDescription = false;
          visibleDescriptions.set(key, handlerInfo);
        } else {
          handlerInfo.disambiguateDescription = true;
          otherHandlerInfo.disambiguateDescription = true;
        }
      }
    }

    _buildHeader() {
      const headerElement =  (
        document.createElement("moz-box-item")
      );
      headerElement.slot = "header";
      this.typeColumn = document.createElement("label");
      this.typeColumn.setAttribute("data-l10n-id", "applications-type-heading");
      headerElement.appendChild(this.typeColumn);

      this.actionColumn = document.createElement("label");
      this.actionColumn.slot = "actions";
      this.actionColumn.setAttribute(
        "data-l10n-id",
        "applications-action-heading"
      );
      headerElement.appendChild(this.actionColumn);

      return headerElement;
    }

    _sortItems(unorderedItems) {
      let comp = new Services.intl.Collator(undefined, {
        usage: "sort",
      });
      const textForNode = item => item.getAttribute("label");
      let multiplier = 1;
      return unorderedItems.sort(
        (a, b) => multiplier * comp.compare(textForNode(a), textForNode(b))
      );
    }

    async _rebuildView() {
      this.items = [];
      this._list.textContent = "";

      await this._rebuildVisibleTypes();
      await this._buildView();
    }

    async _buildView() {
      for (let item of this.items) {
        item.node.hidden = true;
      }
      let itemsFragment = document.createDocumentFragment();

      const unorderedItems = [];

      let promises = [];

      var visibleTypes = this._visibleTypes;
      for (const visibleType of visibleTypes) {
        const handlerItem = new ApplicationListItem(visibleType);

        promises.push(
          handlerItem
            .createNode()
            .then(node => {
              unorderedItems.push(node);

              this.items.push(handlerItem);

              let originalValue = handlerItem.actionsMenu.value;

              handlerItem.actionsMenu.addEventListener("change", async () => {
                const newValue = handlerItem.actionsMenu.value;

                await handlerItem.actionsMenu.updateComplete;

                if (newValue !== "choose-app" && newValue !== "manage-app") {
                  this._onSelectActionsMenuOption(handlerItem);
                } else {
                  handlerItem.actionsMenu.value = originalValue;

                  if (newValue === "choose-app") {
                    this.chooseApp(handlerItem);
                  } else {
                    this.manageApp(handlerItem);
                  }
                }
              });
            })
            .catch(console.error)
        );
      }

      await Promise.allSettled(promises);
      const sortedItems = this._sortItems(unorderedItems);
      for (const element of sortedItems) {
        itemsFragment.appendChild(element);
      }

      if (this._filter.value) {
        await document.l10n.translateFragment(itemsFragment);
        this.filter();

        document.l10n.pauseObserving();
        document.l10n.resumeObserving();
      }

      this._list.appendChild(itemsFragment);

      this._filter.addEventListener("MozInputSearch:search", () =>
        this.filter()
      );
    }

    filter() {
      const filterValue = this._filter.value.toLowerCase();
      for (let item of this.items) {
        item.node.hidden =
          !item.node.label.toLowerCase().includes(filterValue) &&
          !item.actionsMenu.selectedOption.label
            .toLowerCase()
            .includes(filterValue);
      }
    }


    _storingAction = false;

    _onSelectActionsMenuOption(handlerItem) {
      this._storeAction(handlerItem);
    }

    _storeAction(handlerItem) {
      this._storingAction = true;

      try {
        var handlerInfo = handlerItem.handlerInfoWrapper;
        const selectedOption = handlerItem.actionsMenu.querySelector(
          `moz-option[value="${handlerItem.actionsMenu.value}"]`
        );
        let action = parseInt(selectedOption.getAttribute("action"));

        if (action == Ci.nsIHandlerInfo.useHelperApp) {
          handlerInfo.preferredApplicationHandler = selectedOption.handlerApp;
        }

        if (action == Ci.nsIHandlerInfo.alwaysAsk) {
          handlerInfo.alwaysAskBeforeHandling = true;
        } else {
          handlerInfo.alwaysAskBeforeHandling = false;
        }

        handlerInfo.preferredAction = action;

        handlerInfo.store();
      } finally {
        this._storingAction = false;
      }
    }

    manageApp(handlerItem) {
      gSubDialog.open(
        "chrome://browser/content/preferences/dialogs/applicationManager.xhtml",
        {
          features: "resizable=no",
          closedCallback: () => {
            handlerItem.buildActionsMenu();
          },
        },
        handlerItem.handlerInfoWrapper
      );
    }

    async chooseApp(handlerItem) {
      var handlerInfo = handlerItem.handlerInfoWrapper;
      var handlerApp;
      let chooseAppCallback =
        aHandlerApp => {
          if (aHandlerApp) {
            handlerItem.buildActionsMenu();

            let actionsMenu = handlerItem.actionsMenu;
            for (const [idx, menuItem] of [
              ...actionsMenu.querySelectorAll("moz-option"),
            ].entries()) {
              if (
                menuItem.handlerApp &&
                menuItem.handlerApp.equals(aHandlerApp)
              ) {
                actionsMenu.value = idx + "";
                this._storeAction(handlerItem);
                break;
              }
            }
          }
        };

      if (AppConstants.platform == "win") {
        var params = {};

        params.mimeInfo = handlerInfo.wrappedHandlerInfo;
        params.title = await document.l10n.formatValue(
          "applications-select-helper"
        );
        if ("id" in handlerInfo.description) {
          params.description = await document.l10n.formatValue(
            handlerInfo.description.id,
            handlerInfo.description.args
          );
        } else {
          params.description = handlerInfo.typeDescription.raw;
        }
        params.filename = null;
        params.handlerApp = null;

        let onAppPickerClose = () => {
          if (this.isValidHandlerApp(params.handlerApp)) {
            handlerApp = params.handlerApp;

            handlerInfo.addPossibleApplicationHandler(handlerApp);
          }

          chooseAppCallback(handlerApp);
          handlerItem.buildActionsMenu();
        };

        gSubDialog.open(
          "chrome://global/content/appPicker.xhtml",
          { closingCallback: onAppPickerClose },
          params
        );
      } else {
        let winTitle = await document.l10n.formatValue(
          "applications-select-helper"
        );
        let fp = Cc["@mozilla.org/filepicker;1"].createInstance(
          Ci.nsIFilePicker
        );
        let fpCallback = aResult => {
          if (
            aResult == Ci.nsIFilePicker.returnOK &&
            fp.file &&
            this._isValidHandlerExecutable(fp.file)
          ) {
            handlerApp = Cc[
              "@mozilla.org/uriloader/local-handler-app;1"
            ].createInstance(Ci.nsILocalHandlerApp);
            handlerApp.name = getFileDisplayName(fp.file);
            handlerApp.executable = fp.file;

            let handler = handlerItem.handlerInfoWrapper;
            handler.addPossibleApplicationHandler(handlerApp);

            chooseAppCallback(handlerApp);
          } else {
            handlerItem.buildActionsMenu();
          }
        };

        fp.init(window.browsingContext, winTitle, Ci.nsIFilePicker.modeOpen);
        fp.appendFilters(Ci.nsIFilePicker.filterApps);
        fp.open(fpCallback);
      }
    }

    isValidHandlerApp(aHandlerApp) {
      if (!aHandlerApp) {
        return false;
      }

      if (aHandlerApp instanceof Ci.nsILocalHandlerApp) {
        return this._isValidHandlerExecutable(aHandlerApp.executable);
      }

      if (aHandlerApp instanceof Ci.nsIWebHandlerApp) {
        return aHandlerApp.uriTemplate;
      }

      if (aHandlerApp instanceof Ci.nsIGIOMimeApp) {
        return aHandlerApp.command;
      }
      if (aHandlerApp instanceof Ci.nsIGIOHandlerApp) {
        return aHandlerApp.id;
      }

      return false;
    }

    _isValidHandlerExecutable(aExecutable) {
      let leafName;
      if (AppConstants.platform == "win") {
        leafName = `${AppConstants.MOZ_APP_NAME}.exe`;
      } else if (AppConstants.platform == "macosx") {
        leafName = AppConstants.MOZ_MACBUNDLE_NAME;
      } else {
        leafName = `${AppConstants.MOZ_APP_NAME}-bin`;
      }
      return (
        aExecutable &&
        aExecutable.exists() &&
        aExecutable.isExecutable() &&
        aExecutable.leafName != leafName
      );
    }
  })();
})();

SettingGroupManager.registerGroups({
  downloads: {
    l10nId: "download-save-files-header",
    headingLevel: 2,
    items: [
      {
        id: "downloadFolder",
        l10nId: "download-save-where-3",
        control: "moz-input-folder",
        controlAttrs: {
          id: "chooseFolder",
        },
      },
      {
        id: "alwaysAsk",
        l10nId: "download-always-ask-where2",
      },
      {
        id: "deletePrivate",
        l10nId: "download-private-browsing-delete2",
      },
    ],
  },
  applications: {
    l10nId: "applications-setting2",
    headingLevel: 2,
    inProgress: true,
    items: [
      {
        id: "applicationsFilter",
        control: "moz-input-search",
        l10nId: "applications-filter",
        controlAttrs: {
          "aria-controls": "applicationsHandlersView",
          "data-l10n-attrs": "placeholder",
        },
      },
      {
        id: "applicationsHandlersView",
        control: "moz-box-group",
        controlAttrs: {
          type: "list",
        },
      },
      {
        id: "handleNewFileTypes",
        l10nId: "applications-setting-new-file-types",
        control: "moz-radio-group",
        options: [
          {
            l10nId: "applications-save-for-new-types2",
            control: "moz-radio",
            value: false,
          },
          {
            l10nId: "applications-ask-before-handling2",
            control: "moz-radio",
            value: true,
          },
        ],
      },
    ],
  },
});
