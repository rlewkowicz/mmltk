/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
});

export const SEARCH_SHORTCUTS = [
  { keyword: "@amazon", shortURL: "amazon", url: "https://amazon.com" },
  { keyword: "@\u767E\u5EA6", shortURL: "baidu", url: "https://baidu.com" },
  { keyword: "@google", shortURL: "google", url: "https://google.com" },
  {
    keyword: "@\u044F\u043D\u0434\u0435\u043A\u0441",
    shortURL: "yandex",
    url: "https://yandex.com",
  },
];

export const CUSTOM_SEARCH_SHORTCUTS = [
  ...SEARCH_SHORTCUTS,
  { keyword: "@bing", shortURL: "bing", url: "https://bing.com" },
  {
    keyword: "@duckduckgo",
    shortURL: "duckduckgo",
    url: "https://duckduckgo.com",
  },
  { keyword: "@ebay", shortURL: "ebay", url: "https://ebay.com" },
  { keyword: "@twitter", shortURL: "twitter", url: "https://twitter.com" },
  {
    keyword: "@wikipedia",
    shortURL: "wikipedia",
    url: "https://wikipedia.org",
  },
];

export const SEARCH_SHORTCUTS_EXPERIMENT =
  "improvesearch.topSiteSearchShortcuts";

export const SEARCH_SHORTCUTS_SEARCH_ENGINES_PREF =
  "improvesearch.topSiteSearchShortcuts.searchEngines";

export const SEARCH_SHORTCUTS_HAVE_PINNED_PREF =
  "improvesearch.topSiteSearchShortcuts.havePinned";

export function getSearchProvider(candidateShortURL) {
  return (
    SEARCH_SHORTCUTS.filter(match => candidateShortURL === match.shortURL)[0] ||
    null
  );
}

export async function checkHasSearchEngine(keyword) {
  try {
    return !!(await lazy.SearchService.getAppProvidedEngines()).find(
      e => e.aliases.includes(keyword) && !e.hidden
    );
  } catch {
    return false;
  }
}
