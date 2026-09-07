/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  HomePage: "resource:///modules/HomePage.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SearchUIUtils: "moz-src:///browser/components/search/SearchUIUtils.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "gSystemPrincipal", () =>
  Services.scriptSecurityManager.getSystemPrincipal()
);

function shouldLoadURI(aURI) {
  if (aURI && !aURI.schemeIs("chrome")) {
    return true;
  }

  dump("*** Preventing external load of chrome: URI into browser window\n");
  dump("    Use --chrome <uri> instead\n");
  return false;
}

function resolveURIInternal(aCmdLine, aArgument) {
  let principal = lazy.gSystemPrincipal;
  var uri = aCmdLine.resolveURI(aArgument);
  var uriFixup = Services.uriFixup;

  if (!(uri instanceof Ci.nsIFileURL)) {
    let prefURI = Services.uriFixup.getFixupURIInfo(
      aArgument,
      uriFixup.FIXUP_FLAG_FIX_SCHEME_TYPOS
    ).preferredURI;
    return { uri: prefURI, principal };
  }

  try {
    if (uri.file.exists()) {
      return { uri, principal };
    }
  } catch (e) {
    console.error(e);
  }


  try {
    uri = Services.uriFixup.getFixupURIInfo(aArgument).preferredURI;
  } catch (e) {
    console.error(e);
  }

  return { uri, principal };
}

let gKiosk = false;
var gFirstWindow = false;

function openBrowserWindow(
  cmdLine,
  triggeringPrincipal,
  urlOrUrlList,
  postData = null,
  forcePrivate = false
) {
  const isStartup =
    cmdLine && cmdLine.state == Ci.nsICommandLine.STATE_INITIAL_LAUNCH;

  let args;
  if (!urlOrUrlList) {
    if (isStartup) {
      args = [gBrowserContentHandler.getFirstWindowArgs()];
    } else {
      args = [gBrowserContentHandler.getNewWindowArgs()];
    }
  } else if (Array.isArray(urlOrUrlList)) {
    if (
      !triggeringPrincipal ||
      !triggeringPrincipal.equals(lazy.gSystemPrincipal)
    ) {
      throw new Error(
        "Can't open multiple URLs with something other than system principal."
      );
    }
    let uriArray = Cc["@mozilla.org/array;1"].createInstance(
      Ci.nsIMutableArray
    );
    urlOrUrlList.forEach(function (uri) {
      var sstring = Cc["@mozilla.org/supports-string;1"].createInstance(
        Ci.nsISupportsString
      );
      sstring.data = uri;
      uriArray.appendElement(sstring);
    });
    args = [uriArray];
  } else {
    let extraOptions = Cc["@mozilla.org/hash-property-bag;1"].createInstance(
      Ci.nsIWritablePropertyBag2
    );
    extraOptions.setPropertyAsBool("fromExternal", true);

    args = [
      urlOrUrlList,
      extraOptions,
      null, 
      postData,
      undefined, 
      undefined, 
      null, 
      null, 
      triggeringPrincipal,
    ];
  }

  if (isStartup) {
    let win = gBrowserContentHandler.replaceStartupWindow(args, forcePrivate);
    if (win) {
      return win;
    }
  }

  if (!urlOrUrlList) {
    let [url] = args;
    let string = Cc["@mozilla.org/supports-string;1"].createInstance(
      Ci.nsISupportsString
    );
    string.data = url;

    args = string;
  } else {
    if (args.length > 1) {
      let string = Cc["@mozilla.org/supports-string;1"].createInstance(
        Ci.nsISupportsString
      );
      string.data = args[0];
      args[0] = string;
    }
    let array = Cc["@mozilla.org/array;1"].createInstance(Ci.nsIMutableArray);
    args.forEach(a => {
      array.appendElement(a);
    });
    args = array;
  }

  return lazy.BrowserWindowTracker.openWindow({
    args,
    features: gBrowserContentHandler.getFeatures(cmdLine),
    private: forcePrivate,
  });
}

function openPreferences(cmdLine) {
  openBrowserWindow(cmdLine, lazy.gSystemPrincipal, "about:preferences");
}

async function doSearch(searchText, cmdLine) {
  let win = openBrowserWindow(cmdLine, lazy.gSystemPrincipal, "about:blank");
  await lazy.BrowserUtils.promiseObserved(
    "browser-delayed-startup-finished",
    subject => subject == win
  );

  lazy.SearchUIUtils.loadSearch({
    window: win,
    searchText,
    usePrivateWindow:
      lazy.PrivateBrowsingUtils.isInTemporaryAutoStartMode ||
      lazy.PrivateBrowsingUtils.isWindowPrivate(win),
    triggeringPrincipal: lazy.gSystemPrincipal,
    policyContainer: win.gBrowser.selectedBrowser.policyContainer,
    sapSource: "system",
  }).catch(console.error);
}

export function nsBrowserContentHandler() {
  if (!gBrowserContentHandler) {
    gBrowserContentHandler = this;
  }
  return gBrowserContentHandler;
}

nsBrowserContentHandler.prototype = {
  QueryInterface: ChromeUtils.generateQI([
    "nsICommandLineHandler",
    "nsIBrowserHandler",
    "nsIContentHandler",
    "nsICommandLineValidator",
  ]),

  handle: function bch_handle(cmdLine) {
    if (
      cmdLine.handleFlag("kiosk", false) ||
      cmdLine.handleFlagWithParam("kiosk-monitor", false)
    ) {
      gKiosk = true;
    }
    if (cmdLine.handleFlag("disable-pinch", false)) {
      let defaults = Services.prefs.getDefaultBranch(null);
      defaults.setBoolPref("apz.allow_zooming", false);
      Services.prefs.lockPref("apz.allow_zooming");
      defaults.setCharPref("browser.gesture.pinch.in", "");
      Services.prefs.lockPref("browser.gesture.pinch.in");
      defaults.setCharPref("browser.gesture.pinch.in.shift", "");
      Services.prefs.lockPref("browser.gesture.pinch.in.shift");
      defaults.setCharPref("browser.gesture.pinch.out", "");
      Services.prefs.lockPref("browser.gesture.pinch.out");
      defaults.setCharPref("browser.gesture.pinch.out.shift", "");
      Services.prefs.lockPref("browser.gesture.pinch.out.shift");
    }
    if (cmdLine.handleFlag("browser", false)) {
      openBrowserWindow(cmdLine, lazy.gSystemPrincipal);
      cmdLine.preventDefault = true;
    }

    var uriparam;
    try {
      while ((uriparam = cmdLine.handleFlagWithParam("new-window", false))) {
        let { uri, principal } = resolveURIInternal(cmdLine, uriparam);
        if (!shouldLoadURI(uri)) {
          continue;
        }
        openBrowserWindow(cmdLine, principal, uri.spec);
        cmdLine.preventDefault = true;
      }
    } catch (e) {
      console.error(e);
    }

    try {
      while ((uriparam = cmdLine.handleFlagWithParam("new-tab", false))) {
        let { uri, principal } = resolveURIInternal(cmdLine, uriparam);
        handURIToExistingBrowser(
          uri,
          Ci.nsIBrowserDOMWindow.OPEN_NEWTAB,
          cmdLine,
          false,
          principal
        );
        cmdLine.preventDefault = true;
      }
    } catch (e) {
      console.error(e);
    }

    var chromeParam = cmdLine.handleFlagWithParam("chrome", false);
    if (chromeParam) {
      if (
        chromeParam == "chrome://browser/content/pref/pref.xul" ||
        chromeParam == "chrome://browser/content/preferences/preferences.xul"
      ) {
        openPreferences(cmdLine);
        cmdLine.preventDefault = true;
      } else {
        try {
          let { uri: resolvedURI } = resolveURIInternal(cmdLine, chromeParam);
          let isLocal = uri => {
            let localSchemes = new Set(["chrome", "file", "resource"]);
            if (uri instanceof Ci.nsINestedURI) {
              uri = uri.QueryInterface(Ci.nsINestedURI).innerMostURI;
            }
            return localSchemes.has(uri.scheme);
          };
          if (isLocal(resolvedURI)) {
            let features = "chrome,dialog=no,all" + this.getFeatures(cmdLine);
            let argArray = Cc["@mozilla.org/array;1"].createInstance(
              Ci.nsIMutableArray
            );
            argArray.appendElement(null);
            Services.ww.openWindow(
              null,
              resolvedURI.spec,
              "_blank",
              features,
              argArray
            );
            cmdLine.preventDefault = true;
          } else {
            dump("*** Preventing load of web URI as chrome\n");
            dump(
              "    If you're trying to load a webpage, do not pass --chrome.\n"
            );
          }
        } catch (e) {
          console.error(e);
        }
      }
    }
    if (cmdLine.handleFlag("preferences", false)) {
      openPreferences(cmdLine);
      cmdLine.preventDefault = true;
    }
    if (cmdLine.handleFlag("silent", false)) {
      cmdLine.preventDefault = true;
    }

    try {
      var privateWindowParam = cmdLine.handleFlagWithParam(
        "private-window",
        false
      );
      if (privateWindowParam) {
        let forcePrivate = true;
        let resolvedInfo;
        if (!lazy.PrivateBrowsingUtils.enabled) {
          forcePrivate = false;
          resolvedInfo = {
            uri: Services.io.newURI("about:privatebrowsing"),
            principal: lazy.gSystemPrincipal,
          };
        } else {
          resolvedInfo = resolveURIInternal(cmdLine, privateWindowParam);
        }
        handURIToExistingBrowser(
          resolvedInfo.uri,
          Ci.nsIBrowserDOMWindow.OPEN_NEWTAB,
          cmdLine,
          forcePrivate,
          resolvedInfo.principal
        );
        cmdLine.preventDefault = true;
      }
    } catch (e) {
      if (e.result != Cr.NS_ERROR_INVALID_ARG) {
        throw e;
      }
      if (cmdLine.handleFlag("private-window", false)) {
        openBrowserWindow(
          cmdLine,
          lazy.gSystemPrincipal,
          "about:privatebrowsing",
          null,
          lazy.PrivateBrowsingUtils.enabled
        );
        cmdLine.preventDefault = true;
      }
    }

    var searchParam = cmdLine.handleFlagWithParam("search", false);
    if (searchParam) {
      doSearch(searchParam, cmdLine);
      cmdLine.preventDefault = true;
    }

    if (
      cmdLine.handleFlag("private", false) &&
      lazy.PrivateBrowsingUtils.enabled
    ) {
      lazy.PrivateBrowsingUtils.enterTemporaryAutoStartMode();
      if (cmdLine.state == Ci.nsICommandLine.STATE_INITIAL_LAUNCH) {
        let win = Services.wm.getMostRecentWindow("navigator:blank");
        if (win) {
          win.docShell.QueryInterface(Ci.nsILoadContext).usePrivateBrowsing =
            true;
        }
      }
    }
    var fileParam = cmdLine.handleFlagWithParam("file", false);
    if (fileParam) {
      var file = cmdLine.resolveFile(fileParam);
      var fileURI = Services.io.newFileURI(file);
      openBrowserWindow(cmdLine, lazy.gSystemPrincipal, fileURI.spec);
      cmdLine.preventDefault = true;
    }

  },

  get helpInfo() {
    let info =
      "  --browser          Open a browser window.\n" +
      "  --new-window <url> Open <url> in a new window.\n" +
      "  --new-tab <url>    Open <url> in a new tab.\n" +
      "  --private-window [<url>] Open <url> in a new private window.\n";
    info += "  --preferences      Open Preferences dialog.\n";
    info +=
      "  --search <term>    Search <term> with your default search engine.\n";
    info += "  --kiosk            Start the browser in kiosk mode.\n";
    info +=
      "  --kiosk-monitor <num> Place kiosk browser window on given monitor.\n";
    info +=
      "  --disable-pinch    Disable touch-screen and touch-pad pinch gestures.\n";
    return info;
  },


  get defaultArgs() {
    return this.getNewWindowArgs();
  },

  getNewWindowArgs(skipStartPage = false) {
    var startPage = "";
    var prefb = Services.prefs;
    try {
      var choice = prefb.getIntPref("browser.startup.page");
      if (choice == 1 || choice == 3) {
        startPage = lazy.HomePage.get();
      }
    } catch (e) {
      console.error(e);
    }

    if (startPage == "about:blank") {
      startPage = "";
    }

    return (!skipStartPage && startPage) || "about:blank";
  },

  getFirstWindowArgs() {
    if (!gFirstWindow) {
      gFirstWindow = true;
      if (lazy.PrivateBrowsingUtils.isInTemporaryAutoStartMode) {
        return "about:privatebrowsing";
      }
    }

    return this.getNewWindowArgs();
  },

  mFeatures: null,

  getFeatures: function bch_features(cmdLine) {
    if (this.mFeatures === null) {
      this.mFeatures = "";

      if (cmdLine) {
        try {
          var width = cmdLine.handleFlagWithParam("width", false);
          var height = cmdLine.handleFlagWithParam("height", false);
          var left = cmdLine.handleFlagWithParam("left", false);
          var top = cmdLine.handleFlagWithParam("top", false);

          if (width) {
            this.mFeatures += ",width=" + width;
          }
          if (height) {
            this.mFeatures += ",height=" + height;
          }
          if (left) {
            this.mFeatures += ",left=" + left;
          }
          if (top) {
            this.mFeatures += ",top=" + top;
          }
        } catch (e) {}
      }

      if (lazy.PrivateBrowsingUtils.isInTemporaryAutoStartMode) {
        this.mFeatures += ",private";
      }

      if (
        Services.prefs.getBoolPref("browser.suppress_first_window_animation") &&
        !Services.wm.getMostRecentWindow("navigator:browser")
      ) {
        this.mFeatures += ",suppressanimation";
      }
    }

    return this.mFeatures;
  },

  get kiosk() {
    return gKiosk;
  },


  handleContent: function bch_handleContent(contentType, context, request) {
    try {
      var webNavInfo = Cc["@mozilla.org/webnavigation-info;1"].getService(
        Ci.nsIWebNavigationInfo
      );
      if (!webNavInfo.isTypeSupported(contentType)) {
        throw new Components.Exception("", Cr.NS_ERROR_WONT_HANDLE_CONTENT);
      }
    } catch (e) {
      throw new Components.Exception("", Cr.NS_ERROR_WONT_HANDLE_CONTENT);
    }

    request.QueryInterface(Ci.nsIChannel);
    handURIToExistingBrowser(
      request.URI,
      Ci.nsIBrowserDOMWindow.OPEN_DEFAULTWINDOW,
      null,
      false,
      request.loadInfo.triggeringPrincipal
    );
    request.cancel(Cr.NS_BINDING_ABORTED);
  },

  replaceStartupWindow(args, forcePrivate) {
    let win = Services.wm.getMostRecentWindow("navigator:blank");
    if (win) {
      win.document.documentElement.removeAttribute("windowtype");

      if (forcePrivate) {
        win.docShell.QueryInterface(Ci.nsILoadContext).usePrivateBrowsing =
          true;
      }

      win.location = AppConstants.BROWSER_CHROME_URL;
      win.arguments = args; 

      lazy.BrowserWindowTracker.registerOpeningWindow(win, forcePrivate);
      return win;
    }
    return null;
  },

  validate: function bch_validate(cmdLine) {
    var urlFlagIdx = cmdLine.findFlag("url", false);
    if (
      urlFlagIdx > -1 &&
      cmdLine.state == Ci.nsICommandLine.STATE_REMOTE_EXPLICIT
    ) {
      var urlParam = cmdLine.getArgument(urlFlagIdx + 1);
      if (
        cmdLine.length != urlFlagIdx + 2 ||
        /firefoxurl(-[a-f0-9]+)?:/i.test(urlParam)
      ) {
        throw Components.Exception("", Cr.NS_ERROR_ABORT);
      }
    }
  },
};
var gBrowserContentHandler = new nsBrowserContentHandler();

function handURIToExistingBrowser(
  uri,
  location,
  cmdLine,
  forcePrivate,
  triggeringPrincipal
) {
  if (!shouldLoadURI(uri)) {
    return;
  }

  let openInWindow = ({ browserDOMWindow }) => {
    browserDOMWindow.openURI(
      uri,
      null,
      location,
      Ci.nsIBrowserDOMWindow.OPEN_EXTERNAL,
      triggeringPrincipal
    );
  };

  let allowPrivate =
    forcePrivate || lazy.PrivateBrowsingUtils.permanentPrivateBrowsing;
  let navWin = lazy.BrowserWindowTracker.getTopWindow({
    private: allowPrivate,
  });

  if (navWin) {
    openInWindow(navWin);
    return;
  }

  let pending = lazy.BrowserWindowTracker.getPendingWindow({
    private: allowPrivate,
  });
  if (pending) {
    pending.then(openInWindow);
    return;
  }

  openBrowserWindow(cmdLine, triggeringPrincipal, uri.spec, null, forcePrivate);
}

export function nsDefaultCommandLineHandler() {}

nsDefaultCommandLineHandler.prototype = {
  QueryInterface: ChromeUtils.generateQI(["nsICommandLineHandler"]),

  handle: function dch_handle(cmdLine) {
    var urilist = [];
    var principalList = [];

    if (
      cmdLine.state == Ci.nsICommandLine.STATE_INITIAL_LAUNCH &&
      Services.startup.wasSilentlyStarted
    ) {
      Services.startup.enterLastWindowClosingSurvivalArea();
      Services.obs.addObserver(function windowOpenObserver() {
        Services.startup.exitLastWindowClosingSurvivalArea();
        Services.obs.removeObserver(windowOpenObserver, "domwindowopened");
      }, "domwindowopened");
      return;
    }

    try {
      var ar;
      while ((ar = cmdLine.handleFlagWithParam("url", false))) {
        let { uri, principal } = resolveURIInternal(cmdLine, ar);
        urilist.push(uri);
        principalList.push(principal);
      }
    } catch (e) {
      console.error(e);
    }

    for (let i = 0; i < cmdLine.length; ++i) {
      var curarg = cmdLine.getArgument(i);
      if (curarg.match(/^-/)) {
        console.error("Warning: unrecognized command line flag", curarg);
        ++i;
      } else {
        try {
          let { uri, principal } = resolveURIInternal(cmdLine, curarg);
          urilist.push(uri);
          principalList.push(principal);
        } catch (e) {
          console.error(
            `Error opening URI ${curarg} from the command line:`,
            e
          );
        }
      }
    }

    if (urilist.length) {
      if (
        cmdLine.state != Ci.nsICommandLine.STATE_INITIAL_LAUNCH &&
        urilist.length == 1
      ) {
        try {
          handURIToExistingBrowser(
            urilist[0],
            Ci.nsIBrowserDOMWindow.OPEN_DEFAULTWINDOW,
            cmdLine,
            false,
            principalList[0] ?? lazy.gSystemPrincipal
          );
          return;
        } catch (e) {}
      }

      var URLlist = urilist.filter(shouldLoadURI).map(u => u.spec);
      if (URLlist.length) {
        openBrowserWindow(cmdLine, lazy.gSystemPrincipal, URLlist);
      }
    } else if (!cmdLine.preventDefault) {
      openBrowserWindow(cmdLine, lazy.gSystemPrincipal);
    } else {
      let win = Services.wm.getMostRecentWindow("navigator:blank");
      if (win) {
        win.close();
      }
    }
  },

  helpInfo: "",
};
