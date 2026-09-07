/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AppUpdater: "resource://gre/modules/AppUpdater.sys.mjs",
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  NLP: "resource://gre/modules/NLP.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  ResetProfile: "resource://gre/modules/ResetProfile.sys.mjs",
  Sanitizer: "resource:///modules/Sanitizer.sys.mjs",
  UrlbarProviderGlobalActions:
    "moz-src:///browser/components/urlbar/UrlbarProviderGlobalActions.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
  UrlUtils: "resource://gre/modules/UrlUtils.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "appUpdater", () => new lazy.AppUpdater());

const TIPS = {
  NONE: "",
  CLEAR: "intervention_clear",
  REFRESH: "intervention_refresh",

  UPDATE_ASK: "intervention_update_ask",

  UPDATE_CHECKING: "intervention_update_checking",

  UPDATE_REFRESH: "intervention_update_refresh",

  UPDATE_RESTART: "intervention_update_restart",

  UPDATE_WEB: "intervention_update_web",
};

const EN_LOCALE_MATCH = /^en(-.*)$/;

const DOCUMENTS = {
  clear: [
    "cache firefox",
    "clear cache firefox",
    "clear cache in firefox",
    "clear cookies firefox",
    "clear firefox cache",
    "clear history firefox",
    "cookies firefox",
    "delete cookies firefox",
    "delete history firefox",
    "firefox cache",
    "firefox clear cache",
    "firefox clear cookies",
    "firefox clear history",
    "firefox cookie",
    "firefox cookies",
    "firefox delete cookies",
    "firefox delete history",
    "firefox history",
    "firefox not loading pages",
    "history firefox",
    "how to clear cache",
    "how to clear history",
  ],
  refresh: [
    "firefox crashing",
    "firefox keeps crashing",
    "firefox not responding",
    "firefox not working",
    "firefox refresh",
    "firefox slow",
    "how to reset firefox",
    "refresh firefox",
    "reset firefox",
  ],
  update: [
    "download firefox",
    "download mozilla",
    "firefox browser",
    "firefox download",
    "firefox for mac",
    "firefox for windows",
    "firefox free download",
    "firefox install",
    "firefox installer",
    "firefox latest version",
    "firefox mac",
    "firefox quantum",
    "firefox update",
    "firefox version",
    "firefox windows",
    "get firefox",
    "how to update firefox",
    "install firefox",
    "mozilla download",
    "mozilla firefox 2019",
    "mozilla firefox 2020",
    "mozilla firefox download",
    "mozilla firefox for mac",
    "mozilla firefox for windows",
    "mozilla firefox free download",
    "mozilla firefox mac",
    "mozilla firefox update",
    "mozilla firefox windows",
    "mozilla update",
    "update firefox",
    "update mozilla",
    "www.firefox.com",
  ],
};

const UPDATE_CHECK_PERIOD_MS = 12 * 60 * 60 * 1000; 

// eslint-disable-next-line no-shadow
class Node {
  constructor(word) {
    this.word = word;
    this.documents = new Set();
    this.childrenByWord = new Map();
  }
}

export class QueryScorer {
  constructor({ distanceThreshold = 1, variations = new Map() } = {}) {
    this._distanceThreshold = distanceThreshold;
    this._variations = variations;
    this._documents = new Set();
    this._rootNode = new Node();
  }

  addDocument(doc) {
    this._documents.add(doc);

    for (let phraseStr of doc.phrases) {
      let phrase = phraseStr
        .trim()
        .split(/\s+/)
        .map(word => word.toLocaleLowerCase());

      let phrases = [phrase];
      for (let [triggerWord, variations] of this._variations) {
        let index = phrase.indexOf(triggerWord);
        if (index >= 0) {
          for (let variation of variations) {
            let variationPhrase = Array.from(phrase);
            variationPhrase.splice(index, 1, ...variation.split(/\s+/));
            phrases.push(variationPhrase);
          }
        }
      }

      for (let completedPhrase of phrases) {
        this._buildPhraseTree(this._rootNode, doc, completedPhrase, 0);
      }
    }
  }

  score(queryString) {
    let queryWords = queryString
      .trim()
      .split(/\s+/)
      .map(word => word.toLocaleLowerCase());
    let minDistanceByDoc = this._traverse({ queryWords });
    let results = [];
    for (let doc of this._documents) {
      let distance = minDistanceByDoc.get(doc);
      results.push({
        document: doc,
        score: distance === undefined ? Infinity : distance,
      });
    }
    results.sort((a, b) => a.score - b.score);
    return results;
  }

  _buildPhraseTree(node, doc, phrase, wordIndex) {
    if (phrase.length == wordIndex) {
      return;
    }

    let word = phrase[wordIndex].toLocaleLowerCase();
    let child = node.childrenByWord.get(word);
    if (!child) {
      child = new Node(word);
      node.childrenByWord.set(word, child);
    }
    child.documents.add(doc);

    this._buildPhraseTree(child, doc, phrase, wordIndex + 1);
  }

  _traverse({
    queryWords,
    node = this._rootNode,
    minDistanceByDoc = new Map(),
    queryWordsIndex = 0,
    phraseDistance = 0,
  }) {
    if (!node.childrenByWord.size) {
      for (let doc of node.documents) {
        minDistanceByDoc.set(
          doc,
          Math.min(
            phraseDistance,
            minDistanceByDoc.has(doc) ? minDistanceByDoc.get(doc) : Infinity
          )
        );
      }
      return minDistanceByDoc;
    }

    if (queryWordsIndex == queryWords.length) {
      return minDistanceByDoc;
    }

    let queryWord = queryWords[queryWordsIndex];
    for (let [childWord, child] of node.childrenByWord) {
      let distance = lazy.NLP.levenshtein(queryWord, childWord);
      if (distance <= this._distanceThreshold) {
        this._traverse({
          node: child,
          queryWords,
          queryWordsIndex: queryWordsIndex + 1,
          phraseDistance: phraseDistance + distance,
          minDistanceByDoc,
        });
      }
    }

    return minDistanceByDoc;
  }
}

function getPayloadForTip(tip) {
  const baseURL = Services.urlFormatter.formatURLPref("app.support.baseURL");
  switch (tip) {
    case TIPS.CLEAR:
      return {
        titleL10n: { id: "intervention-clear-data" },
        buttons: [{ l10n: { id: "intervention-clear-data-confirm" } }],
        helpUrl: baseURL + "delete-browsing-search-download-history-firefox",
      };
    case TIPS.REFRESH:
      return {
        titleL10n: { id: "intervention-refresh-profile" },
        buttons: [{ l10n: { id: "intervention-refresh-profile-confirm" } }],
        helpUrl: baseURL + "refresh-firefox-reset-add-ons-and-settings",
      };
    case TIPS.UPDATE_ASK:
      return {
        titleL10n: { id: "intervention-update-ask" },
        buttons: [{ l10n: { id: "intervention-update-ask-confirm" } }],
        helpUrl: baseURL + "update-firefox-latest-release",
      };
    case TIPS.UPDATE_REFRESH:
      return {
        titleL10n: { id: "intervention-update-refresh" },
        buttons: [{ l10n: { id: "intervention-update-refresh-confirm" } }],
        helpUrl: baseURL + "refresh-firefox-reset-add-ons-and-settings",
      };
    case TIPS.UPDATE_RESTART:
      return {
        titleL10n: { id: "intervention-update-restart" },
        buttons: [{ l10n: { id: "intervention-update-restart-confirm" } }],
        helpUrl: baseURL + "update-firefox-latest-release",
      };
    case TIPS.UPDATE_WEB:
      return {
        titleL10n: { id: "intervention-update-web" },
        buttons: [{ l10n: { id: "intervention-update-web-confirm" } }],
        helpUrl: baseURL + "update-firefox-latest-release",
      };
    default:
      throw new Error("Unknown TIP type.");
  }
}

export class UrlbarProviderInterventions extends UrlbarProvider {
  static lazy = XPCOMUtils.declareLazy({
    queryScorer: () => {
      let queryScorer = new QueryScorer({
        variations: new Map([
          ["firefox", ["fire fox", "fox fire", "foxfire"]],
          ["mozilla", ["mozila"]],
        ]),
      });
      for (let [id, phrases] of Object.entries(DOCUMENTS)) {
        queryScorer.addDocument({ id, phrases });
      }
      return queryScorer;
    },
  });

  constructor() {
    super();
    this.currentTip = TIPS.NONE;
  }

  static get TIP_TYPE() {
    return TIPS;
  }

  get type() {
    return UrlbarUtils.PROVIDER_TYPE.PROFILE;
  }

  async isActive(queryContext) {
    if (
      !queryContext.searchString ||
      queryContext.searchString.length > UrlbarUtils.MAX_TEXT_LENGTH ||
      lazy.UrlUtils.REGEXP_LIKE_PROTOCOL.test(queryContext.searchString) ||
      !EN_LOCALE_MATCH.test(Services.locale.appLocaleAsBCP47) ||
      !Services.policies.isAllowed("urlbarinterventions") ||
      (await this.queryInstance
        .getProvider(lazy.UrlbarProviderGlobalActions.name)
        ?.isActive(queryContext))
    ) {
      return false;
    }

    this.currentTip = TIPS.NONE;

    let docScores = UrlbarProviderInterventions.lazy.queryScorer.score(
      queryContext.searchString
    );
    let topDocScore = docScores[0];

    let topDocIDs = new Set();
    if (topDocScore.score != Infinity) {
      for (let { score, document } of docScores) {
        if (score != topDocScore.score) {
          break;
        }
        topDocIDs.add(document.id);
      }
    }

    if (topDocIDs.has("update")) {
      this._setCurrentTipFromAppUpdaterStatus();
    } else if (topDocIDs.has("clear")) {
      let window = lazy.BrowserWindowTracker.getTopWindow();
      if (!lazy.PrivateBrowsingUtils.isWindowPrivate(window)) {
        this.currentTip = TIPS.CLEAR;
      }
    } else if (topDocIDs.has("refresh")) {
      this.currentTip = TIPS.REFRESH;
    }

    return (
      this.currentTip != TIPS.NONE &&
      (this.currentTip != TIPS.REFRESH ||
        Services.policies.isAllowed("profileRefresh"))
    );
  }

  async _setCurrentTipFromAppUpdaterStatus() {
    try {
      UrlbarProviderInterventions.checkForBrowserUpdate();
    } catch (ex) {
      return;
    }

    switch (lazy.appUpdater.status) {
      case lazy.AppUpdater.STATUS.READY_FOR_RESTART:
        this.currentTip = TIPS.UPDATE_RESTART;
        break;
      case lazy.AppUpdater.STATUS.DOWNLOAD_AND_INSTALL:
        this.currentTip = TIPS.UPDATE_ASK;
        break;
      case lazy.AppUpdater.STATUS.NO_UPDATES_FOUND:
        this.currentTip = TIPS.UPDATE_REFRESH;
        break;
      case lazy.AppUpdater.STATUS.CHECKING:
        this.currentTip = TIPS.UPDATE_CHECKING;
        break;
      case lazy.AppUpdater.STATUS.NO_UPDATER:
      case lazy.AppUpdater.STATUS.UPDATE_DISABLED_BY_POLICY:
        this.currentTip = TIPS.NONE;
        break;
      default:
        this.currentTip = TIPS.UPDATE_WEB;
        break;
    }
  }

  async startQuery(queryContext, addCallback) {
    let instance = this.queryInstance;

    if (this.currentTip == TIPS.UPDATE_CHECKING) {
      this._setCurrentTipFromAppUpdaterStatus();
      if (this.currentTip == TIPS.UPDATE_CHECKING) {
        await new Promise(resolve => {
          this._appUpdaterListener = () => {
            lazy.appUpdater.removeListener(this._appUpdaterListener);
            delete this._appUpdaterListener;
            resolve();
          };
          lazy.appUpdater.addListener(this._appUpdaterListener);
        });
        if (instance != this.queryInstance) {
          return;
        }
        this._setCurrentTipFromAppUpdaterStatus();
        if (this.currentTip == TIPS.UPDATE_CHECKING) {
          return;
        }
      }
    }

    let result = new lazy.UrlbarResult({
      type: lazy.UrlbarShared.RESULT_TYPE.TIP,
      source: lazy.UrlbarShared.RESULT_SOURCE.OTHER_LOCAL,
      suggestedIndex: 1,
      payload: {
        ...getPayloadForTip(this.currentTip),
        type: this.currentTip,
        icon: UrlbarUtils.ICON.TIP,
        helpL10n: {
          id: "urlbar-result-menu-tip-get-help2",
        },
      },
    });
    addCallback(this, result);
  }

  cancelQuery() {
    if (this._appUpdaterListener) {
      lazy.appUpdater.removeListener(this._appUpdaterListener);
      delete this._appUpdaterListener;
    }
  }

  #pickResult(result, window) {
    let tip = result.payload.type;

    switch (tip) {
      case TIPS.CLEAR:
        openClearHistoryDialog(window);
        break;
      case TIPS.REFRESH:
      case TIPS.UPDATE_REFRESH:
        resetBrowser(window);
        break;
      case TIPS.UPDATE_ASK:
        installBrowserUpdateAndRestart();
        break;
      case TIPS.UPDATE_RESTART:
        restartBrowser();
        break;
      case TIPS.UPDATE_WEB:
        window.gBrowser.selectedTab = window.gBrowser.addWebTab(
          "https://www.mozilla.org/firefox/new/"
        );
        break;
    }
  }

  onEngagement(queryContext, controller, details) {
    if (details.selType == "tip") {
      this.#pickResult(details.result, controller.browserWindow);
    }
  }

  static _lastUpdateCheckTime;
  static checkForBrowserUpdate(force = false) {
    if (
      force ||
      !UrlbarProviderInterventions._lastUpdateCheckTime ||
      Date.now() - UrlbarProviderInterventions._lastUpdateCheckTime >=
        UPDATE_CHECK_PERIOD_MS
    ) {
      UrlbarProviderInterventions._lastUpdateCheckTime = Date.now();
      lazy.appUpdater.check();
    }
  }

  static resetAppUpdater() {
    if (!Object.getOwnPropertyDescriptor(lazy, "appUpdater").get) {
      lazy.appUpdater = new lazy.AppUpdater();
    }
  }
}


function installBrowserUpdateAndRestart() {
  if (lazy.appUpdater.status != lazy.AppUpdater.STATUS.DOWNLOAD_AND_INSTALL) {
    return Promise.resolve();
  }
  return new Promise(resolve => {
    let listener = () => {
      if (
        lazy.appUpdater.status != lazy.AppUpdater.STATUS.READY_FOR_RESTART &&
        lazy.appUpdater.status != lazy.AppUpdater.STATUS.DOWNLOAD_FAILED
      ) {
        return;
      }
      lazy.appUpdater.removeListener(listener);
      if (lazy.appUpdater.status == lazy.AppUpdater.STATUS.READY_FOR_RESTART) {
        restartBrowser();
      }
      resolve();
    };
    lazy.appUpdater.addListener(listener);
    lazy.appUpdater.allowUpdateDownload();
  });
}

function openClearHistoryDialog(window) {
  if (lazy.PrivateBrowsingUtils.isWindowPrivate(window)) {
    return;
  }
  lazy.Sanitizer.showUI(window);
}

function restartBrowser() {
  let cancelQuit = Cc["@mozilla.org/supports-PRBool;1"].createInstance(
    Ci.nsISupportsPRBool
  );
  Services.obs.notifyObservers(
    cancelQuit,
    "quit-application-requested",
    "restart"
  );
  if (cancelQuit.data) {
    return;
  }
  if (Services.appinfo.inSafeMode) {
    Services.startup.restartInSafeMode(Ci.nsIAppStartup.eAttemptQuit);
  } else {
    Services.startup.quit(
      Ci.nsIAppStartup.eAttemptQuit | Ci.nsIAppStartup.eRestart
    );
  }
}

function resetBrowser(window) {
  if (!lazy.ResetProfile.resetSupported()) {
    return;
  }
  lazy.ResetProfile.openConfirmationDialog(window);
}
