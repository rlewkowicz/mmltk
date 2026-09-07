/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { UrlbarShared } from "chrome://browser/content/urlbar/UrlbarShared.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  JsonSchemaValidator:
    "resource://gre/modules/components-utils/JsonSchemaValidator.sys.mjs",
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});


export class UrlbarResult {

  constructor({
    type,
    source,
    autofill,
    group,
    heuristic = false,
    hideRowLabel = false,
    isBestMatch = false,
    isBottomUrlSuggestion = false,
    isRichSuggestion = false,
    isSuggestedIndexRelativeToGroup = false,
    providerName,
    resultSpan,
    richSuggestionIconSize,
    richSuggestionIconVariation,
    rowLabel,
    showFeedbackMenu = false,
    suggestedIndex,
    payload,
    highlights = null,
    testForceNewContent,
  }) {
    if (!Object.values(UrlbarShared.RESULT_TYPE).includes(type)) {
      throw new Error("Invalid result type");
    }
    this.#type = type;

    if (!Object.values(UrlbarShared.RESULT_SOURCE).includes(source)) {
      throw new Error("Invalid result source");
    }
    this.#source = source;

    if (!payload || typeof payload != "object") {
      throw new Error("Invalid result payload");
    }

    payload = Object.fromEntries(
      Object.entries(payload).filter(([_, v]) => v != undefined)
    );

    if (highlights) {
      this.#highlights = Object.freeze(highlights);
    }

    this.#payload = this.#validatePayload(payload);

    this.#autofill = autofill;
    this.#group = group;
    this.#heuristic = heuristic;
    this.#hideRowLabel = hideRowLabel;
    this.#isBestMatch = isBestMatch;
    this.#isBottomUrlSuggestion = isBottomUrlSuggestion;
    this.#isRichSuggestion = isRichSuggestion;
    this.#isSuggestedIndexRelativeToGroup = isSuggestedIndexRelativeToGroup;
    this.#richSuggestionIconSize = richSuggestionIconSize;
    this.#richSuggestionIconVariation = richSuggestionIconVariation;
    this.#providerName = providerName;
    this.#resultSpan = resultSpan;
    this.#rowLabel = rowLabel;
    this.#showFeedbackMenu = showFeedbackMenu;
    this.#suggestedIndex = suggestedIndex;

    if (this.#type == UrlbarShared.RESULT_TYPE.TIP) {
      this.#isRichSuggestion = true;
      this.#richSuggestionIconSize = 24;
    }

    this.#testForceNewContent = testForceNewContent;
  }

  rowIndex = undefined;

  viewTemplate = undefined;

  commands = undefined;

  get type() {
    return this.#type;
  }

  get source() {
    return this.#source;
  }

  get autofill() {
    return this.#autofill;
  }

  get group() {
    return this.#group;
  }

  get heuristic() {
    return this.#heuristic;
  }

  get hideRowLabel() {
    return this.#hideRowLabel;
  }

  get isBestMatch() {
    return this.#isBestMatch;
  }

  get isBottomUrlSuggestion() {
    return this.#isBottomUrlSuggestion;
  }

  get isRichSuggestion() {
    return this.#isRichSuggestion;
  }
  set isRichSuggestion(value) {
    this.#isRichSuggestion = value;
  }

  get isSuggestedIndexRelativeToGroup() {
    return this.#isSuggestedIndexRelativeToGroup;
  }
  set isSuggestedIndexRelativeToGroup(value) {
    this.#isSuggestedIndexRelativeToGroup = value;
  }

  get providerName() {
    return this.#providerName;
  }
  set providerName(value) {
    this.#providerName = value;
  }

  get providerType() {
    return this.#providerType;
  }
  set providerType(value) {
    this.#providerType = value;
  }

  get resultSpan() {
    return this.#resultSpan;
  }

  get richSuggestionIconSize() {
    return this.#richSuggestionIconSize;
  }

  get richSuggestionIconVariation() {
    return this.#richSuggestionIconVariation;
  }
  set richSuggestionIconSize(value) {
    this.#richSuggestionIconSize = value;
  }

  get rowLabel() {
    return this.#rowLabel;
  }

  get showFeedbackMenu() {
    return this.#showFeedbackMenu;
  }

  get suggestedIndex() {
    return this.#suggestedIndex;
  }
  set suggestedIndex(value) {
    this.#suggestedIndex = value;
  }

  get payload() {
    return this.#payload;
  }

  get testForceNewContent() {
    return this.#testForceNewContent;
  }

  get testHighlights() {
    return this.#highlights;
  }

  get icon() {
    return this.payload.icon;
  }

  get hasSuggestedIndex() {
    return typeof this.suggestedIndex == "number";
  }

  getDisplayableValueAndHighlights(payloadName, options = {}) {
    if (!this.#displayValuesCache) {
      this.#displayValuesCache = new Map();
    }

    if (this.#displayValuesCache.has(payloadName)) {
      let cached = this.#displayValuesCache.get(payloadName);
      if (
        options.isURL == cached.options.isURL &&
        (options.tokens == undefined ||
          lazy.ObjectUtils.deepEqual(options.tokens, cached.options.tokens))
      ) {
        return this.#displayValuesCache.get(payloadName);
      }
    }

    let highlightType;
    let { isURL } = options;

    let value = this.payload[payloadName];
    if (!value) {
      if (payloadName != "title" || !this.payload.url) {
        return {};
      }

      highlightType = lazy.UrlbarUtils.HIGHLIGHT.TYPED;
      try {
        value = new URL(this.payload.url).URI.displayHostPort;
        isURL = !value;
      } catch (e) {
        isURL = false;
      }

      value ||= this.payload.url;
    }

    if (isURL) {
      value = lazy.UrlbarUtils.prepareUrlForDisplay(value);
    }

    if (typeof value == "string") {
      value = value.substring(0, lazy.UrlbarUtils.MAX_TEXT_LENGTH);
    }

    if (Array.isArray(this.#highlights?.[payloadName])) {
      return { value, highlights: this.#highlights[payloadName] };
    }

    highlightType ??= this.#highlights?.[payloadName];

    options = structuredClone(options);

    if (!options.tokens?.length || !highlightType) {
      let cached = { value, options };
      this.#displayValuesCache.set(payloadName, cached);
      return cached;
    }

    let highlights = Array.isArray(value)
      ? value.map(subval =>
          lazy.UrlbarUtils.getTokenMatches(
            options.tokens,
            subval,
            highlightType
          )
        )
      : lazy.UrlbarUtils.getTokenMatches(options.tokens, value, highlightType);

    let cached = { value, highlights, options };
    this.#displayValuesCache.set(payloadName, cached);
    return cached;
  }

  #validatePayload(payload) {
    let schema = lazy.UrlbarUtils.getPayloadSchema(this.type);
    if (!schema) {
      throw new Error(`Unrecognized result type: ${this.type}`);
    }
    let result = lazy.JsonSchemaValidator.validate(payload, schema, {
      allowExplicitUndefinedProperties: true,
      allowNullAsUndefinedProperties: true,
      allowAdditionalProperties: this.type == UrlbarShared.RESULT_TYPE.DYNAMIC,
    });
    if (!result.valid) {
      throw result.error;
    }
    return payload;
  }

  toString() {
    if (this.payload.url) {
      return this.payload.title + " - " + this.payload.url.substr(0, 100);
    }
    if (this.payload.keyword) {
      return this.payload.keyword + " - " + this.payload.query;
    }
    if (this.payload.engine) {
      return this.payload.engine + " - " + this.payload.query;
    }
    return JSON.stringify(this);
  }

  toWire() {
    return {
      type: this.#type,
      source: this.#source,
      autofill: this.#autofill,
      group: this.#group,
      heuristic: this.#heuristic,
      hideRowLabel: this.#hideRowLabel,
      isBestMatch: this.#isBestMatch,
      isBottomUrlSuggestion: this.#isBottomUrlSuggestion,
      isRichSuggestion: this.#isRichSuggestion,
      isSuggestedIndexRelativeToGroup: this.#isSuggestedIndexRelativeToGroup,
      providerName: this.#providerName,
      providerType: this.#providerType,
      resultSpan: this.#resultSpan,
      richSuggestionIconSize: this.#richSuggestionIconSize,
      richSuggestionIconVariation: this.#richSuggestionIconVariation,
      rowLabel: this.#rowLabel,
      showFeedbackMenu: this.#showFeedbackMenu,
      suggestedIndex: this.#suggestedIndex,
      payload: this.#payload,
      highlights: this.#highlights,
      rowIndex: this.rowIndex,
      viewTemplate: this.viewTemplate,
      commands: this.commands,
    };
  }

  static fromWire(wire) {
    let result = new UrlbarResult(wire);
    result.providerType = wire.providerType;
    result.rowIndex = wire.rowIndex;
    result.viewTemplate = wire.viewTemplate;
    result.commands = wire.commands;
    return result;
  }

  #type;
  #source;
  #autofill;
  #group;
  #heuristic;
  #hideRowLabel;
  #isBestMatch;
  #isBottomUrlSuggestion;
  #isRichSuggestion;
  #isSuggestedIndexRelativeToGroup;
  #providerName;
  #providerType;
  #resultSpan;
  #richSuggestionIconSize;
  #richSuggestionIconVariation;
  #rowLabel;
  #showFeedbackMenu;
  #suggestedIndex;
  #payload;
  #highlights;
  #displayValuesCache;
  #testForceNewContent;
}
