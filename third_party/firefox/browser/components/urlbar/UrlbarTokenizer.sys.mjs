/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { UrlbarShared } from "chrome://browser/content/urlbar/UrlbarShared.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
  UrlUtils: "resource://gre/modules/UrlUtils.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logger", () =>
  UrlbarShared.getLogger({ prefix: "Tokenizer" })
);

ChromeUtils.defineLazyGetter(lazy, "gFluentStrings", function () {
  return new Localization(["browser/browser.ftl"]);
});


let tokenToKeywords = new Map();

export var UrlbarTokenizer = {
  async loadL10nRestrictKeywords() {
    let l10nKeywords = await lazy.gFluentStrings.formatValues(
      lazy.UrlbarUtils.LOCAL_SEARCH_MODES.map(mode => {
        let name = lazy.UrlbarUtils.getResultSourceName(mode.source);
        return { id: `urlbar-search-mode-${name}` };
      })
    );

    let englishSearchStrings = new Localization([
      "preview/enUS-searchFeatures.ftl",
    ]);

    let englishKeywords = await englishSearchStrings.formatValues(
      lazy.UrlbarUtils.LOCAL_SEARCH_MODES.map(mode => {
        let name = lazy.UrlbarUtils.getResultSourceName(mode.source);
        return { id: `urlbar-search-mode-${name}-en` };
      })
    );

    for (let { restrict } of lazy.UrlbarUtils.LOCAL_SEARCH_MODES) {
      let uniqueKeywords = [
        ...new Set([l10nKeywords.shift(), englishKeywords.shift()]),
      ];

      tokenToKeywords.set(restrict, uniqueKeywords);
    }
  },

  async getL10nRestrictKeywords() {
    if (tokenToKeywords.size === 0) {
      await this.loadL10nRestrictKeywords();
    }

    return tokenToKeywords;
  },

  tokenize(context) {
    lazy.logger.debug("Tokenizing search string", {
      searchString: context.searchString,
    });
    if (!context.trimmedSearchString) {
      return [];
    }
    let unfiltered = splitString(context);
    return filterTokens(unfiltered);
  },

  isRestrictionToken(token) {
    return (
      token &&
      token.type >= UrlbarShared.TOKEN_TYPE.RESTRICT_HISTORY &&
      token.type <= UrlbarShared.TOKEN_TYPE.RESTRICT_URL
    );
  },
};

const CHAR_TO_TYPE_MAP = new Map(
  Object.entries(UrlbarShared.RESTRICT_TOKENS).map(([type, char]) => [
    char,
    UrlbarShared.TOKEN_TYPE[`RESTRICT_${type}`],
  ])
);

function splitString({ searchString, searchMode }) {
  let trimmed = searchString.trim();
  let tokens;
  if (trimmed.startsWith("data:")) {
    tokens = [trimmed];
  } else if (trimmed.length < 500) {
    tokens = trimmed.split(lazy.UrlUtils.REGEXP_SPACES);
  } else {
    tokens = trimmed.substring(0, 500).split(lazy.UrlUtils.REGEXP_SPACES);
    tokens[tokens.length - 1] += trimmed.substring(500);
  }

  if (!tokens.length) {
    return tokens;
  }

  const hasRestrictionToken = tokens.some(t => CHAR_TO_TYPE_MAP.has(t));

  const firstToken = tokens[0];
  const isFirstTokenAKeyword =
    AppConstants.MOZ_PLACES &&
    !Object.values(UrlbarShared.RESTRICT_TOKENS).includes(
       (firstToken)
    ) && lazy.PlacesUtils.keywords.isKeywordFromCache(firstToken);

  if (hasRestrictionToken || isFirstTokenAKeyword) {
    return tokens;
  }

  if (
    CHAR_TO_TYPE_MAP.has(firstToken[0]) &&
    !lazy.UrlUtils.REGEXP_PERCENT_ENCODED_START.test(firstToken) &&
    !searchMode
  ) {
    tokens[0] = firstToken.substring(1);
    tokens.splice(0, 0, firstToken[0]);
    return tokens;
  }

  return tokens;
}

function filterTokens(tokens) {
  let filtered = [];
  let restrictions = [];
  const isFirstTokenAKeyword =
    AppConstants.MOZ_PLACES &&
    !Object.values(UrlbarShared.RESTRICT_TOKENS).includes(tokens[0]) &&
    lazy.PlacesUtils.keywords.isKeywordFromCache(tokens[0]);

  for (let i = 0; i < tokens.length; ++i) {
    let token = tokens[i];
    let tokenObj = {
      value: token,
      lowerCaseValue: token.toLocaleLowerCase(),
      type: UrlbarShared.TOKEN_TYPE.TEXT,
    };
    if (tokens.length > 1 && token.length > 500) {
      filtered.push(tokenObj);
      break;
    }

    if (isFirstTokenAKeyword) {
      filtered.push(tokenObj);
      continue;
    }

    let restrictionType = CHAR_TO_TYPE_MAP.get(token);
    if (restrictionType) {
      restrictions.push({ index: i, type: restrictionType });
    } else {
      let looksLikeOrigin = lazy.UrlUtils.looksLikeOrigin(token);
      if (
        looksLikeOrigin == lazy.UrlUtils.LOOKS_LIKE_ORIGIN.OTHER &&
        lazy.UrlbarPrefs.get("allowSearchSuggestionsForSimpleOrigins")
      ) {
        tokenObj.type =
          UrlbarShared.TOKEN_TYPE.POSSIBLE_ORIGIN_BUT_SEARCH_ALLOWED;
      } else if (looksLikeOrigin != lazy.UrlUtils.LOOKS_LIKE_ORIGIN.NONE) {
        tokenObj.type = UrlbarShared.TOKEN_TYPE.POSSIBLE_ORIGIN;
      } else if (lazy.UrlUtils.looksLikeUrl(token, { requirePath: true })) {
        tokenObj.type = UrlbarShared.TOKEN_TYPE.POSSIBLE_URL;
      }
    }
    filtered.push(tokenObj);
  }

  if (restrictions.length) {
    let matchingRestrictionFound = false;
    let typeRestrictionFound = false;
    function assignRestriction(r) {
      if (r && !(matchingRestrictionFound && typeRestrictionFound)) {
        if (
          [
            UrlbarShared.TOKEN_TYPE.RESTRICT_TITLE,
            UrlbarShared.TOKEN_TYPE.RESTRICT_URL,
          ].includes(r.type)
        ) {
          if (!matchingRestrictionFound) {
            matchingRestrictionFound = true;
            filtered[r.index].type = r.type;
            return true;
          }
        } else if (!typeRestrictionFound) {
          typeRestrictionFound = true;
          filtered[r.index].type = r.type;
          return true;
        }
      }
      return false;
    }

    let found = assignRestriction(restrictions.find(r => r.index == 0));
    if (found) {
      assignRestriction(restrictions.find(r => r.index == 1));
    }
    let lastIndex = tokens.length - 1;
    found = assignRestriction(restrictions.find(r => r.index == lastIndex));
    if (found) {
      assignRestriction(restrictions.find(r => r.index == lastIndex - 1));
    }
  }

  lazy.logger.info("Filtered Tokens", filtered);
  return filtered;
}
