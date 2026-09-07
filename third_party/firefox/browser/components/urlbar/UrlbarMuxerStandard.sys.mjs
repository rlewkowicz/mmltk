/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import {
  UrlbarMuxer,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
  UrlbarPrefs: "moz-src:///browser/components/urlbar/UrlbarPrefs.sys.mjs",
  UrlbarProviderOpenTabs:
    "moz-src:///browser/components/urlbar/UrlbarProviderOpenTabs.sys.mjs",
  UrlbarSearchUtils:
    "moz-src:///browser/components/urlbar/UrlbarSearchUtils.sys.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logger", () =>
  lazy.UrlbarShared.getLogger({ prefix: "MuxerUnifiedComplete" })
);

const MS_PER_DAY = 1000 * 60 * 60 * 24;

function makeMapKeyForTabResult(result) {
  return UrlbarUtils.tupleString(
    result.payload.url,
    result.type == lazy.UrlbarShared.RESULT_TYPE.TAB_SWITCH &&
      lazy.UrlbarProviderOpenTabs.isNonPrivateUserContextId(
        result.payload.userContextId
      )
      ? result.payload.userContextId
      : undefined
  );
}

class MuxerUnifiedComplete extends UrlbarMuxer {
  constructor() {
    super();
  }

  get name() {
    return "UnifiedComplete";
  }

  sort(context, unsortedResults) {

    let state = {
      context,
      resultsByGroup: new Map(),
      suggestedIndexResultsByGroup: new Map(),
      availableResultSpan: context.maxResults,
      globalSuggestedIndexResultSpan: 0,
      usedResultSpan: 0,
      strippedUrlToTopPrefixAndTitle: new Map(),
      baseAndTitleToTopRef: new Map(),
      urlToTabResultType: new Map(),
      addedSwitchTabUrls: new Set(),
      addedResultUrls: new Set(),
      canShowPrivateSearch: unsortedResults.length > 1,
      maxHeuristicResultSpan: 0,
    };

    let rootGroup = lazy.UrlbarPrefs.getResultGroups({ context });
    lazy.logger.debug("Root groups", rootGroup);

    let indicesToSort = new Map();
    for (let i = 0; i < unsortedResults.length; ++i) {
      let group = UrlbarUtils.getResultGroup(unsortedResults[i]);
      let groupObj = this.#getGroupAsObject(rootGroup, group);
      let sortingField = groupObj?.orderBy;
      if (sortingField) {
        let indices = indicesToSort.get(group);
        if (!indices) {
          indices = { sortingField, start: i, length: 1 };
          indicesToSort.set(group, indices);
        } else {
          indices.length++;
        }
      }
    }
    for (let { sortingField, start, length } of indicesToSort.values()) {
      let toSort = unsortedResults.slice(start, start + length);
      toSort.sort((a, b) => b.payload[sortingField] - a.payload[sortingField]);
      unsortedResults.splice(start, length, ...toSort);
    }

    for (let result of unsortedResults) {
      let group = UrlbarUtils.getResultGroup(result);
      let resultsByGroup =
        result.hasSuggestedIndex && result.isSuggestedIndexRelativeToGroup
          ? state.suggestedIndexResultsByGroup
          : state.resultsByGroup;
      let results = resultsByGroup.get(group);
      if (!results) {
        results = [];
        resultsByGroup.set(group, results);
      }
      results.push(result);

      this._updateStatePreAdd(result, state);
    }

    let globalSuggestedIndexAvailableSpan = Math.min(
      state.availableResultSpan,
      state.globalSuggestedIndexResultSpan
    );
    state.availableResultSpan -= globalSuggestedIndexAvailableSpan;

    if (state.maxHeuristicResultSpan) {
      if (lazy.UrlbarPrefs.get("experimental.hideHeuristic")) {
        state.availableResultSpan += state.maxHeuristicResultSpan;
      } else if (context.maxResults > 0) {
        state.availableResultSpan = Math.max(
          state.availableResultSpan,
          state.maxHeuristicResultSpan
        );
      }
    }

    let [sortedResults] = this._fillGroup(
      rootGroup,
      { availableSpan: state.availableResultSpan, maxResultCount: Infinity },
      state
    );

    let globalSuggestedIndexResults = state.resultsByGroup.get(
      UrlbarUtils.RESULT_GROUP.SUGGESTED_INDEX
    );
    if (globalSuggestedIndexResults) {
      this._addSuggestedIndexResults(
        globalSuggestedIndexResults,
        sortedResults,
        {
          availableSpan: globalSuggestedIndexAvailableSpan,
          maxResultCount: Infinity,
        },
        state
      );
    }

    context.results = sortedResults;
  }

  #getGroupAsObject(rootGroup, group) {
    if ("children" in rootGroup) {
      for (let child of rootGroup.children) {
        if ("children" in child) {
          let groupObj = this.#getGroupAsObject(child, group);
          if (groupObj) {
            return groupObj;
          }
        } else if (child.group == group) {
          return child;
        }
      }
    }
    return null;
  }

  _copyState(state) {
    let copy = Object.assign({}, state, {
      resultsByGroup: new Map(),
      suggestedIndexResultsByGroup: new Map(),
      strippedUrlToTopPrefixAndTitle: new Map(
        state.strippedUrlToTopPrefixAndTitle
      ),
      baseAndTitleToTopRef: new Map(state.baseAndTitleToTopRef),
      urlToTabResultType: new Map(state.urlToTabResultType),
      addedSwitchTabUrls: new Set(state.addedSwitchTabUrls),
      addedResultUrls: new Set(state.addedResultUrls),
    });

    for (let key of ["resultsByGroup", "suggestedIndexResultsByGroup"]) {
      for (let [group, results] of state[key]) {
        copy[key].set(group, [...results]);
      }
    }

    return copy;
  }

  _fillGroup(group, limits, state) {
    let suggestedIndexResults;
    let suggestedIndexAvailableSpan = 0;
    let suggestedIndexAvailableCount = 0;
    if ("group" in group) {
      let results = state.suggestedIndexResultsByGroup.get(group.group);
      if (results) {
        let span = 0;
        let resultCount = 0;
        for (let result of results) {
          if (this._canAddResult(result, state)) {
            suggestedIndexResults ??= [];
            suggestedIndexResults.push(result);
            const spanSize = UrlbarUtils.getSpanForResult(result);
            span += spanSize;
            if (spanSize) {
              resultCount++;
            }
          }
        }

        suggestedIndexAvailableSpan = Math.min(limits.availableSpan, span);
        suggestedIndexAvailableCount = Math.min(
          limits.maxResultCount,
          resultCount
        );

        limits = { ...limits };
        limits.availableSpan -= suggestedIndexAvailableSpan;
        limits.maxResultCount -= suggestedIndexAvailableCount;
      }
    }

    let [results, usedLimits, hasMoreResults] = group.children
      ? this._fillGroupChildren(group, limits, state)
      : this._addResults(group.group, limits, state);

    if (suggestedIndexResults) {
      let suggestedIndexUsedLimits = this._addSuggestedIndexResults(
        suggestedIndexResults,
        results,
        {
          availableSpan: suggestedIndexAvailableSpan,
          maxResultCount: suggestedIndexAvailableCount,
        },
        state
      );
      for (let [key, value] of Object.entries(suggestedIndexUsedLimits)) {
        usedLimits[key] += value;
      }
    }

    return [results, usedLimits, hasMoreResults];
  }

  _fillGroupChildren(group, limits, state, flexDataArray = null) {
    let stateCopy;
    if (group.flexChildren) {
      stateCopy = this._copyState(state);
      flexDataArray = this._updateFlexData(group, limits, flexDataArray);
    }

    let results = [];
    let usedLimits = {};
    for (let key of Object.keys(limits)) {
      usedLimits[key] = 0;
    }
    let anyChildUnderfilled = false;
    let anyChildHasMoreResults = false;
    for (let i = 0; i < group.children.length; i++) {
      let child = group.children[i];
      let flexData = flexDataArray?.[i];

      let childLimits = {};
      for (let key of Object.keys(limits)) {
        childLimits[key] = flexData
          ? flexData.limits[key]
          : Math.min(
              typeof child[key] == "number" ? child[key] : Infinity,
              limits[key] - usedLimits[key]
            );
      }

      let [childResults, childUsedLimits, childHasMoreResults] =
        this._fillGroup(child, childLimits, state);
      results = results.concat(childResults);
      for (let key of Object.keys(usedLimits)) {
        usedLimits[key] += childUsedLimits[key];
      }
      anyChildHasMoreResults = anyChildHasMoreResults || childHasMoreResults;

      if (flexData?.hasMoreResults) {
        flexData.usedLimits = childUsedLimits;
        flexData.hasMoreResults = childHasMoreResults;
        anyChildUnderfilled =
          anyChildUnderfilled ||
          (!childHasMoreResults &&
            [...Object.entries(childLimits)].every(
              ([key, limit]) => flexData.usedLimits[key] < limit
            ));
      }
    }

    if (anyChildUnderfilled && anyChildHasMoreResults) {
      [results, usedLimits, anyChildHasMoreResults] = this._fillGroupChildren(
        group,
        limits,
        stateCopy,
        flexDataArray
      );

      for (let [key, value] of Object.entries(stateCopy)) {
        state[key] = value;
      }
    }

    return [results, usedLimits, anyChildHasMoreResults];
  }

  _updateFlexData(group, limits, flexDataArray) {
    flexDataArray =
      flexDataArray ||
      group.children.map((child, index) => {
        let data = {
          index,
          limits: {},
          limitFractions: {},
          usedLimits: {},
          hasMoreResults: true,
          flex: typeof child.flex == "number" ? child.flex : 0,
        };
        for (let key of Object.keys(limits)) {
          data.limits[key] = 0;
          data.limitFractions[key] = 0;
          data.usedLimits[key] = 0;
        }
        return data;
      });

    let fillableDataArray = [];

    let fillableFlexSum = 0;

    for (let data of flexDataArray) {
      if (data.hasMoreResults) {
        fillableFlexSum += data.flex;
        fillableDataArray.push(data);
      }
    }

    for (let [key, limit] of Object.entries(limits)) {
      let fillableLimit = limit;
      for (let data of flexDataArray) {
        if (!data.hasMoreResults) {
          fillableLimit -= data.usedLimits[key];
        }
      }

      fillableLimit = Math.max(fillableLimit, 0);

      let summedFillableLimit = 0;

      for (let data of fillableDataArray) {
        let unroundedLimit = fillableLimit * (data.flex / fillableFlexSum);
        data.limitFractions[key] = unroundedLimit - Math.floor(unroundedLimit);
        data.limits[key] = Math.round(unroundedLimit);
        summedFillableLimit += data.limits[key];
      }

      if (summedFillableLimit != fillableLimit) {
        let fractionalDataArray = fillableDataArray.filter(
          data => data.limitFractions[key]
        );

        let diff;
        if (summedFillableLimit < fillableLimit) {
          diff = 1;
          fractionalDataArray.sort((a, b) => {
            let cmp = b.limitFractions[key] - a.limitFractions[key];
            return cmp || a.index - b.index;
          });
        } else if (fillableLimit < summedFillableLimit) {
          diff = -1;
          fractionalDataArray.sort((a, b) => {
            let cmp = a.limitFractions[key] - b.limitFractions[key];
            return cmp || b.index - a.index;
          });
        }

        while (summedFillableLimit != fillableLimit) {
          if (!fractionalDataArray.length) {
            lazy.logger.error("fractionalDataArray is empty!");
            break;
          }
          let data = flexDataArray[fractionalDataArray.shift().index];
          data.limits[key] += diff;
          summedFillableLimit += diff;
        }
      }
    }

    return flexDataArray;
  }

  _addResults(groupConst, limits, state) {
    let usedLimits = {};
    for (let key of Object.keys(limits)) {
      usedLimits[key] = 0;
    }

    let addedResults = [];
    let groupResults = state.resultsByGroup.get(groupConst);
    while (
      groupResults?.length &&
      state.usedResultSpan < state.availableResultSpan &&
      [...Object.entries(limits)].every(([k, limit]) => usedLimits[k] < limit)
    ) {
      let result = groupResults[0];
      if (this._canAddResult(result, state)) {
        if (!this.#updateUsedLimits(result, limits, usedLimits, state)) {
          break;
        }
        addedResults.push(result);
      }

      groupResults.shift();
    }

    return [addedResults, usedLimits, !!groupResults?.length];
  }

  // eslint-disable-next-line complexity
  _canAddResult(result, state) {
    if (
      !result.heuristic &&
      result.type == lazy.UrlbarShared.RESULT_TYPE.URL &&
      result.payload.url
    ) {
      let [strippedUrl, prefix] = UrlbarUtils.stripPrefixAndTrim(
        result.payload.url,
        {
          stripHttp: true,
          stripHttps: true,
          stripWww: true,
          trimEmptyQuery: true,
        }
      );
      let topPrefixData = state.strippedUrlToTopPrefixAndTitle.get(strippedUrl);
      if (
        topPrefixData &&
        (prefix != topPrefixData.prefix ||
          result.providerName != topPrefixData.providerName)
      ) {
        let prefixRank = UrlbarUtils.getPrefixRank(prefix);
        if (
          (prefixRank < topPrefixData.rank &&
            (prefix.endsWith("www.") == topPrefixData.prefix.endsWith("www.") ||
              result.payload?.title == topPrefixData.title)) ||
          (prefix == topPrefixData.prefix &&
            result.providerName != topPrefixData.providerName)
        ) {
          return false;
        }
      }
    }

    if (
      state.context.heuristicResult &&
      state.context.heuristicResult.autofill &&
      !result.autofill &&
      state.context.heuristicResult.payload?.url == result.payload.url &&
      state.context.heuristicResult.type == result.type &&
      !lazy.UrlbarPrefs.get("experimental.hideHeuristic")
    ) {
      return false;
    }

    if (
      !result.heuristic &&
      result.providerName == "UrlbarProviderHeuristicFallback" &&
      state.context.heuristicResult?.providerName !=
        "UrlbarProviderHeuristicFallback"
    ) {
      return false;
    }

    if (
      result.type == lazy.UrlbarShared.RESULT_TYPE.SEARCH &&
      result.payload.inPrivateWindow &&
      !state.canShowPrivateSearch
    ) {
      return false;
    }

    if (
      result.type == lazy.UrlbarShared.RESULT_TYPE.TAB_SWITCH &&
      state.addedSwitchTabUrls.has(makeMapKeyForTabResult(result))
    ) {
      return false;
    }

    if (
      !result.heuristic &&
      result.type == lazy.UrlbarShared.RESULT_TYPE.URL &&
      result.payload.url &&
      state.urlToTabResultType.has(result.payload.url)
    ) {
      return false;
    }

    if (
      state.context.searchMode?.engineName &&
      result.payload.url &&
      state.context.restrictInSearchMode()
    ) {
      let engine = lazy.SearchService.getEngineByName(
        state.context.searchMode.engineName
      );
      if (engine) {
        let searchModeRootDomain =
          lazy.UrlbarSearchUtils.getRootDomainFromEngine(engine);
        let resultUrl = new URL(result.payload.url);
        if (!resultUrl.hostname.includes(`${searchModeRootDomain}.`)) {
          return false;
        }
      }
    }

    if (result.heuristic && state.usedResultSpan) {
      return false;
    }

    if (result.payload.url) {
      let urlParams = result.payload.url.split("?").pop();
      let embeddedUrl = new URLSearchParams(urlParams).get("url");

      if (state.addedResultUrls.has(embeddedUrl)) {
        return false;
      }
    }

    if (
      lazy.UrlbarPrefs.get("deduplication.enabled") &&
      result.source == lazy.UrlbarShared.RESULT_SOURCE.HISTORY &&
      result.type == lazy.UrlbarShared.RESULT_TYPE.URL &&
      !result.heuristic &&
      result.payload.lastVisit
    ) {
      let { base, ref } = UrlbarUtils.extractRefFromUrl(result.payload.url);
      let baseAndTitle = `${base} ${result.payload.title}`;
      let topRef = state.baseAndTitleToTopRef.get(baseAndTitle);

      let msSinceLastVisit = Date.now() - result.payload.lastVisit;
      let daysSinceLastVisit = msSinceLastVisit / MS_PER_DAY;
      let thresholdDays = lazy.UrlbarPrefs.get("deduplication.thresholdDays");

      if (daysSinceLastVisit >= thresholdDays && ref != topRef) {
        return false;
      }
    }

    return true;
  }

  _updateStatePreAdd(result, state) {
    if (result.heuristic && this._canAddResult(result, state)) {
      state.maxHeuristicResultSpan = Math.max(
        state.maxHeuristicResultSpan,
        UrlbarUtils.getSpanForResult(result)
      );
    }

    if (
      result.hasSuggestedIndex &&
      !result.isSuggestedIndexRelativeToGroup &&
      this._canAddResult(result, state)
    ) {
      state.globalSuggestedIndexResultSpan +=
        UrlbarUtils.getSpanForResult(result);
    }

    if (
      (result.type == lazy.UrlbarShared.RESULT_TYPE.URL ||
        result.type == lazy.UrlbarShared.RESULT_TYPE.KEYWORD) &&
      result.payload.url &&
      (!result.heuristic || !lazy.UrlbarPrefs.get("experimental.hideHeuristic"))
    ) {
      let [strippedUrl, prefix] = UrlbarUtils.stripPrefixAndTrim(
        result.payload.url,
        {
          stripHttp: true,
          stripHttps: true,
          stripWww: true,
          trimEmptyQuery: true,
        }
      );
      let prefixRank = UrlbarUtils.getPrefixRank(prefix);
      let topPrefixData = state.strippedUrlToTopPrefixAndTitle.get(strippedUrl);
      let topPrefixRank = topPrefixData ? topPrefixData.rank : -1;
      if (topPrefixRank < prefixRank) {
        state.strippedUrlToTopPrefixAndTitle.set(strippedUrl, {
          prefix,
          title: result.payload.title,
          rank: prefixRank,
          providerName: result.providerName,
        });
      }
    }

    if (
      (result.type == lazy.UrlbarShared.RESULT_TYPE.URL ||
        result.type == lazy.UrlbarShared.RESULT_TYPE.TAB_SWITCH)
    ) {
      let { base, ref } = UrlbarUtils.extractRefFromUrl(result.payload.url);

      let baseAndTitle = `${base} ${result.payload.title}`;

      if (!state.baseAndTitleToTopRef.has(baseAndTitle) || result.heuristic) {
        state.baseAndTitleToTopRef.set(baseAndTitle, ref);
      }
    }

    if (
      result.payload.url &&
      result.type == lazy.UrlbarShared.RESULT_TYPE.TAB_SWITCH
    ) {
      state.urlToTabResultType.set(makeMapKeyForTabResult(result), result.type);
    }

    if (result.payload.url) {
      state.addedResultUrls.add(result.payload.url);
    }
  }

  _updateStatePostAdd(result, state) {
    if (result.heuristic) {
      state.context.heuristicResult = result;
    }

    if (
      !lazy.SearchService.separatePrivateDefaultUrlbarResultEnabled ||
      (state.canShowPrivateSearch &&
        (result.type != lazy.UrlbarShared.RESULT_TYPE.SEARCH ||
          result.payload.providesSearchMode ||
          (result.heuristic && result.payload.keyword)))
    ) {
      state.canShowPrivateSearch = false;
    }

    if (result.type == lazy.UrlbarShared.RESULT_TYPE.TAB_SWITCH) {
      state.addedSwitchTabUrls.add(makeMapKeyForTabResult(result));
    }
  }

  _addSuggestedIndexResults(
    suggestedIndexResults,
    sortedResults,
    limits,
    state
  ) {
    let usedLimits = {
      availableSpan: 0,
      maxResultCount: 0,
    };

    if (!suggestedIndexResults?.length) {
      return usedLimits;
    }

    let positive = [];
    let negative = [];
    for (let result of suggestedIndexResults) {
      let results = result.suggestedIndex < 0 ? negative : positive;
      results.push(result);
    }

    positive.sort((a, b) => {
      if (a.suggestedIndex !== b.suggestedIndex) {
        return a.suggestedIndex - b.suggestedIndex;
      }

      if (a.providerName === b.providerName) {
        return 0;
      }

      if (a.providerName === "UrlbarProviderGlobalActions") {
        return 1;
      }
      if (b.providerName === "UrlbarProviderGlobalActions") {
        return -1;
      }
      return 0;
    });

    negative.sort((a, b) => b.suggestedIndex - a.suggestedIndex);

    for (let results of [positive, negative]) {
      let prevResult;
      let prevIndex;
      for (let result of results) {
        if (this._canAddResult(result, state)) {
          if (!this.#updateUsedLimits(result, limits, usedLimits, state)) {
            return usedLimits;
          }

          let index;
          if (
            prevResult &&
            prevResult.suggestedIndex == result.suggestedIndex
          ) {
            index = prevIndex;
          } else {
            index =
              result.suggestedIndex >= 0
                ? Math.min(result.suggestedIndex, sortedResults.length)
                : Math.max(result.suggestedIndex + sortedResults.length + 1, 0);
          }
          prevResult = result;
          prevIndex = index;
          sortedResults.splice(index, 0, result);
        }
      }
    }

    return usedLimits;
  }

  #updateUsedLimits(result, limits, usedLimits, state) {
    let span = UrlbarUtils.getSpanForResult(result);
    let newUsedSpan = usedLimits.availableSpan + span;
    if (limits.availableSpan < newUsedSpan) {
      return false;
    }

    usedLimits.availableSpan = newUsedSpan;
    if (span) {
      usedLimits.maxResultCount++;
    }

    state.usedResultSpan += span;
    this._updateStatePostAdd(result, state);

    return true;
  }

}

export var UrlbarMuxerStandard = new MuxerUnifiedComplete();
