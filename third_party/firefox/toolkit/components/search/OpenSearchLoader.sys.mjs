/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */



import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  HiddenBrowserManager: "resource://gre/modules/HiddenFrame.sys.mjs",
  OpenSearchParser:
    "moz-src:///toolkit/components/search/OpenSearchParser.sys.mjs",
  SearchEngineInstallError:
    "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  logConsole: () =>
    console.createInstance({
      prefix: "OpenSearchLoader",
      maxLogLevel: lazy.SearchUtils.loggingEnabled ? "Debug" : "Warn",
    }),
});


export async function loadAndParseOpenSearchEngine(
  sourceURI,
  lastModified,
  originAttributes
) {
  if (!sourceURI) {
    throw new TypeError("No URI");
  }
  if (!/^https?$/i.test(sourceURI.scheme)) {
    throw new TypeError(
      "Unsupported URI scheme passed to SearchEngine constructor"
    );
  }

  lazy.logConsole.debug("Downloading OpenSearch engine from:", sourceURI.spec);

  let xmlData = await loadEngineXML(sourceURI, lastModified, originAttributes);

  lazy.logConsole.debug("Loading search plugin");

  let engineData = await parseXMLData(xmlData);

  engineData.installURL = sourceURI;
  return engineData;
}

async function parseXMLData(xmlData) {
  return parseInHiddenBrowser(xmlData);
}

async function parseInHiddenBrowser(xmlData) {
  return lazy.HiddenBrowserManager.withHiddenBrowser(
    async browser => {
      let { promise, resolve } = Promise.withResolvers();

      let progressListener = {
        QueryInterface: ChromeUtils.generateQI([
          "nsIWebProgressListener",
          "nsISupportsWeakReference",
        ]),
        onStateChange(webProgress, _request, flags) {
          if (
            !(flags & Ci.nsIWebProgressListener.STATE_STOP) ||
            !(flags & Ci.nsIWebProgressListener.STATE_IS_NETWORK)
          ) {
            return;
          }
          browser.removeProgressListener(progressListener);
          resolve();
        },
      };

      browser.addProgressListener(
        progressListener,
        Ci.nsIWebProgress.NOTIFY_STATE_NETWORK
      );

      browser.loadURI(Services.io.newURI("about:blank"), {
        triggeringPrincipal: Services.scriptSecurityManager.createNullPrincipal(
          {}
        ),
      });

      await promise;

      let actor =
        browser.browsingContext.currentWindowGlobal.getActor(
          "OpenSearchLoader"
        );
      let result = await actor.sendQuery(
        "OpenSearchLoader:getEngineData",
        xmlData
      );

      if ("error" in result) {
        lazy.logConsole.error(
          "parseInHiddenBrowser: Failed to init engine!",
          result.error
        );
        throw new lazy.SearchEngineInstallError("corrupted", result.error);
      }

      return result.data;
    },
    { messageManagerGroup: "opensearch" }
  );
}

function loadEngineXML(sourceURI, lastModified, originAttributes = null) {
  var chan = lazy.SearchUtils.makeChannel(
    sourceURI,
    Ci.nsIContentPolicy.TYPE_DOCUMENT,
    originAttributes
  );

  chan.loadInfo.httpsUpgradeTelemetry = sourceURI.schemeIs("https")
    ? Ci.nsILoadInfo.ALREADY_HTTPS
    : Ci.nsILoadInfo.NO_UPGRADE;

  if (lastModified && chan instanceof Ci.nsIHttpChannel) {
    chan.setRequestHeader("If-Modified-Since", lastModified, false);
  }
  let loadPromise = Promise.withResolvers();

  let loadHandler = data => {
    if (!data) {
      loadPromise.reject(new lazy.SearchEngineInstallError("download-failure"));
      return;
    }
    loadPromise.resolve(data);
  };

  var listener = new lazy.SearchUtils.LoadListener(
    chan,
    /(^text\/|xml$)/,
    loadHandler
  );
  chan.notificationCallbacks = listener;
  chan.asyncOpen(listener);

  return loadPromise.promise;
}
