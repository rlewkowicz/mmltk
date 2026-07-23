/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  SearchSettings: "moz-src:///toolkit/components/search/SearchSettings.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  OpenSearchEngine:
    "moz-src:///toolkit/components/search/OpenSearchEngine.sys.mjs",
  logConsole: () =>
    console.createInstance({
      prefix: "SearchEngine",
      maxLogLevel: lazy.SearchUtils.loggingEnabled ? "Debug" : "Warn",
    }),
  settingsRedesignEnabled: {
    pref: "browser.settings-redesign.enabled",
    default: false,
  },
});

const OS_PARAM_INPUT_ENCODING = "inputEncoding";
const OS_PARAM_LANGUAGE = "language";
const OS_PARAM_OUTPUT_ENCODING = "outputEncoding";

const OS_PARAM_LANGUAGE_DEF = "*";
const OS_PARAM_OUTPUT_ENCODING_DEF = "UTF-8";

const OS_PARAM_COUNT = "count";
const OS_PARAM_START_INDEX = "startIndex";
const OS_PARAM_START_PAGE = "startPage";

const OS_PARAM_COUNT_DEF = "20"; 
const OS_PARAM_START_INDEX_DEF = "1"; 
const OS_PARAM_START_PAGE_DEF = "1"; 

const PARAM_ACCEPT_LANGUAGES = "acceptLanguages";

var OS_UNSUPPORTED_PARAMS = [
  [OS_PARAM_COUNT, OS_PARAM_COUNT_DEF],
  [OS_PARAM_START_INDEX, OS_PARAM_START_INDEX_DEF],
  [OS_PARAM_START_PAGE, OS_PARAM_START_PAGE_DEF],
];

const USER_ATTRIBUTES = ["alias", "order", "hideOneOffButton"];

function limitURILength(str, len = 140) {
  if (str.length > len) {
    return str.slice(0, len) + "...";
  }
  return str;
}

function isDateStringTodayOrFuture(dateStr) {
  if (!dateStr) {
    return false;
  }
  let today = new Date().toISOString().slice(0, 10); 
  return today <= dateStr;
}

export class QueryParameter {
  constructor(name, value) {
    if (!name || value == null) {
      throw new TypeError("missing name or value for QueryParameter");
    }

    this.name = name;
    this._value = value;
  }

  get value() {
    return this._value;
  }

  toJSON() {
    return {
      name: this.name,
      value: this.value,
    };
  }
}

function paramSubstitution(paramValue, searchTerms, queryCharset) {
  const PARAM_REGEXP = /\{(\w+)(\??)\}/g;
  return paramValue.replace(PARAM_REGEXP, function (match, name, optional) {
    if (name == "searchTerms") {
      return searchTerms;
    }

    if (name == OS_PARAM_INPUT_ENCODING) {
      return queryCharset;
    }

    if (name == PARAM_ACCEPT_LANGUAGES) {
      return Services.locale.acceptLanguages.replace(/\s+/g, "");
    }

    if (name == OS_PARAM_LANGUAGE) {
      return Services.locale.requestedLocale || OS_PARAM_LANGUAGE_DEF;
    }
    if (name == OS_PARAM_OUTPUT_ENCODING) {
      return OS_PARAM_OUTPUT_ENCODING_DEF;
    }

    if (optional) {
      return "";
    }

    for (let param of OS_UNSUPPORTED_PARAMS) {
      if (name == param[0]) {
        return param[1];
      }
    }

    return match;
  });
}

export class EngineURL {
  params = [];
  rels = [];
  template;
  displayName;
  isNewUntil;
  excludePartnerCodeFromTelemetry;
  acceptedContentTypes;

  #searchTermParam = null;

  constructor({
    type,
    template,
    method = "GET",
    displayName = "",
    isNewUntil = "",
    excludePartnerCodeFromTelemetry = false,
    acceptedContentTypes = null,
  }) {
    if (!type || !method || !template) {
      throw new Error("Missing arguments for EngineURL");
    }

    this.method = method.toUpperCase();
    if (this.method != "GET" && this.method != "POST") {
      throw new TypeError('Method must be "GET" or "POST"');
    }

    var templateURI = lazy.SearchUtils.makeURI(template);
    if (!templateURI) {
      throw new Error("template is not a valid URI");
    }

    switch (templateURI.scheme) {
      case "http":
      case "https":
        this.template = template;
        break;
      default:
        throw new Error("template uses an invalid scheme");
    }

    this.templateHost = templateURI.host;

    let urlParms = new URLSearchParams(templateURI.query);
    for (let [name, value] of urlParms.entries()) {
      if (value == "{searchTerms}") {
        this.#searchTermParam = name;
      }
    }

    this.type = type.toLowerCase();
    this.displayName = displayName ?? "";
    this.isNewUntil = isNewUntil ?? "";
    this.excludePartnerCodeFromTelemetry = !!excludePartnerCodeFromTelemetry;
    this.acceptedContentTypes = acceptedContentTypes;
  }

  addQueryParameter(param) {
    if (param.value == "{searchTerms}") {
      this.setSearchTermParamName(param.name);
      return;
    }
    this.params.push(param);
  }

  addParam(name, value) {
    this.addQueryParameter(new QueryParameter(name, value));
  }

  setSearchTermParamName(name) {
    if (this.#searchTermParam) {
      lazy.logConsole.warn(
        "set searchTermParamName: searchTermParamName was set twice."
      );
    }
    this.params.push(new QueryParameter(name, "{searchTerms}"));
    this.#searchTermParam = name;
  }

  get searchTermParamName() {
    return this.#searchTermParam;
  }

  getSubmission(searchTerms, queryCharset) {
    let escapedSearchTerms;
    try {
      escapedSearchTerms = Services.textToSubURI.ConvertAndEscape(
        queryCharset,
        searchTerms
      );
    } catch (ex) {
      lazy.logConsole.warn(
        "getSubmission: Falling back to default queryCharset!"
      );
      escapedSearchTerms = Services.textToSubURI.ConvertAndEscape(
        lazy.SearchUtils.DEFAULT_QUERY_CHARSET,
        searchTerms
      );
    }

    let templateURI = new URL(this.template);
    let paramString = this.#encodeParams(escapedSearchTerms, queryCharset);

    let postData = null;
    let query = paramSubstitution(
      templateURI.search,
      escapedSearchTerms,
      queryCharset
    );
    if (this.method == "GET" && paramString) {
      if (query) {
        query += "&" + paramString;
      } else {
        query = paramString;
      }
    } else if (this.method == "POST") {
      let stringStream = Cc[
        "@mozilla.org/io/string-input-stream;1"
      ].createInstance(Ci.nsIStringInputStream);
      stringStream.setByteStringData(paramString);

      postData = Cc["@mozilla.org/network/mime-input-stream;1"].createInstance(
        Ci.nsIMIMEInputStream
      );
      postData.addHeader("Content-Type", "application/x-www-form-urlencoded");
      postData.setData(stringStream);
    }

    templateURI.search = query;

    let urlSearchTerms = escapedSearchTerms.replaceAll("+", "%20");
    templateURI.pathname = paramSubstitution(
      decodeURIComponent(templateURI.pathname),
      urlSearchTerms,
      queryCharset
    );
    templateURI.hash = paramSubstitution(
      templateURI.hash,
      urlSearchTerms,
      queryCharset
    );

    return { uri: templateURI.URI, postData };
  }

  isNew() {
    return isDateStringTodayOrFuture(this.isNewUntil);
  }

  #encodeParams(escapedSearchTerms, queryCharset) {
    let dataArray = [];
    for (let param of this.params) {
      if (param.value != null) {
        let value = paramSubstitution(
          param.value,
          escapedSearchTerms,
          queryCharset
        );
        dataArray.push(param.name + "=" + value);
      }
    }
    return dataArray.join("&");
  }

  _hasRelation(rel) {
    return this.rels.some(e => e == rel.toLowerCase());
  }

  _initWithJSON(json) {
    if (!json.params) {
      return;
    }

    this.rels = json.rels;

    for (let param of json.params) {
      if (!param.mozparam && !param.purpose) {
        this.addParam(param.name, param.value);
      }
    }
  }

  toJSON() {
    var json = {
      params: this.params,
      rels: this.rels,
      template: this.template,
    };

    if (this.type != lazy.SearchUtils.URL_TYPE.SEARCH) {
      json.type = this.type;
    }
    if (this.method != "GET") {
      json.method = this.method;
    }

    return json;
  }
}

export class SearchEngine {
  _metaData = {};

  _loadPath = null;

  _name = null;
  _queryCharset = null;
  _engineAddedToStore = false;
  _definedAliases = [];
  _urls = [];
  _searchUrlPublicSuffix = null;
  #id;
  clickUrl = null;

  isNewUntil;

  constructor(options) {
    this.#id = options.id ?? this.#uuid();
    if (!("loadPath" in options)) {
      throw new Error("loadPath missing from options.");
    }
    this._loadPath = options.loadPath;
  }

  getURLOfType(type, rel) {
    for (let url of this._urls) {
      if (url.type == type && (!rel || url._hasRelation(rel))) {
        return url;
      }
    }

    return null;
  }

  _addIconToMap(iconURL, size, override = true) {
    this._iconMapObj = this._iconMapObj || {};
    if (!(size in this._iconMapObj) || override) {
      this._iconMapObj[size] = iconURL;
    }
  }

  async _setIcon(iconURL, options = { override: true }) {
    lazy.logConsole.debug(
      "_setIcon: Setting icon url for",
      this.name,
      "to",
      limitURILength(iconURL)
    );

    let size;
    [iconURL, size] = await this._downloadAndRescaleIcon(iconURL, {
      size: options.size,
      originAttributes: options.originAttributes,
    });
    this._addIconToMap(iconURL, size, options.override);

    if (this._engineAddedToStore) {
      lazy.SearchUtils.notifyAction(
        this,
        lazy.SearchUtils.MODIFIED_TYPE.ICON_CHANGED
      );
    }
  }

  async _downloadAndRescaleIcon(iconURL, options = {}) {
    let uri = lazy.SearchUtils.makeURI(iconURL);

    if (!uri) {
      throw new Error(`Invalid URI`);
    }

    let size = options.size;

    switch (uri.scheme) {
      case "data":
      case "http":
      case "https": {
        let [byteArray, contentType] = await lazy.SearchUtils.fetchIcon(
          uri,
          options.originAttributes
        );
        if (byteArray.length > lazy.SearchUtils.MAX_ICON_SIZE) {
          lazy.logConsole.debug(
            `Rescaling icon for search engine ${this.name}.`
          );
          [byteArray, contentType] = lazy.SearchUtils.rescaleIcon(
            byteArray,
            contentType,
            32
          );
          size = 32;
        }

        if (!size) {
          size = lazy.SearchUtils.decodeSize(byteArray, contentType, 16);
        }

        let dataURL = "data:" + contentType + ";base64," + byteArray.toBase64();
        return [dataURL, size];
      }
      default:
        throw new Error(`URL scheme ${uri.scheme} is not allowed`);
    }
  }

  _getEngineURLFromMetaData(type, params) {
    let url = new EngineURL({ ...params, type });

    if (params.postParams) {
      if (Array.isArray(params.postParams)) {
        for (let { name, value } of params.postParams) {
          url.addParam(name, value);
        }
      } else {
        for (let [name, value] of new URLSearchParams(params.postParams)) {
          url.addParam(name, value);
        }
      }
    }

    if (params.getParams) {
      if (Array.isArray(params.getParams)) {
        for (let { name, value } of params.getParams) {
          url.addParam(name, value);
        }
      } else {
        for (let [name, value] of new URLSearchParams(params.getParams)) {
          url.addParam(name, value);
        }
      }
    }

    return url;
  }

  _initWithDetails(details) {
    this._name = details.name.trim();

    this._definedAliases = [];
    if (Array.isArray(details.keyword)) {
      this._definedAliases = details.keyword.map(k => k.trim());
    } else if (details.keyword?.trim()) {
      this._definedAliases = [details.keyword?.trim()];
    }

    if (details.iconURL) {
      this._setIcon(details.iconURL).catch(e =>
        lazy.logConsole.warn(
          `Error while setting icon for search engine ${details.name}:`,
          e.message
        )
      );
    }
    this._setUrls(details);
  }

  _setUrls(details) {
    let postParams = details.search_url_post_params || "";
    let url = this._getEngineURLFromMetaData(lazy.SearchUtils.URL_TYPE.SEARCH, {
      method: (postParams && "POST") || "GET",
      template: decodeURI(details.search_url),
      getParams: details.search_url_get_params || "",
      postParams,
    });

    this._urls.push(url);

    if (details.suggest_url) {
      let suggestPostParams = details.suggest_url_post_params || "";
      url = this._getEngineURLFromMetaData(
        lazy.SearchUtils.URL_TYPE.SUGGEST_JSON,
        {
          method: (suggestPostParams && "POST") || "GET",
          template: details.suggest_url,
          getParams: details.suggest_url_get_params || "",
          postParams: suggestPostParams,
        }
      );

      this._urls.push(url);
    }

    if (details.encoding) {
      this._queryCharset = details.encoding;
    }
  }

  checkSearchUrlMatchesManifest(details) {
    let existingUrl = this.getURLOfType(lazy.SearchUtils.URL_TYPE.SEARCH);

    let newUrl = this._getEngineURLFromMetaData(
      lazy.SearchUtils.URL_TYPE.SEARCH,
      {
        method: (details.search_url_post_params && "POST") || "GET",
        template: decodeURI(details.search_url),
        getParams: details.search_url_get_params || "",
        postParams: details.search_url_post_params || "",
      }
    );

    let existingSubmission = existingUrl.getSubmission("", this.queryCharset);
    let newSubmission = newUrl.getSubmission("", this.queryCharset);

    return (
      existingSubmission.uri.equals(newSubmission.uri) &&
      existingSubmission.postData?.data.QueryInterface(Ci.nsISupportsCString)
        .data ==
        newSubmission.postData?.data.QueryInterface(Ci.nsISupportsCString).data
    );
  }

  overrideWithEngine({ engine }) {
    this._overriddenData = {
      urls: this._urls,
      queryCharset: this._queryCharset,
    };
    this.copyUserSettingsFrom(engine);

    this._urls = engine._urls;
    this.setAttr("overriddenBy", engine.id);
    this.setAttr("overriddenByOpenSearch", engine.toJSON());

    if (this.searchURLWithNoTerms.spec != this.getAttr("overriddenURL")) {
      this.setAttr("overriddenURL", this.searchURLWithNoTerms.spec, true);
    }
  }

  removeExtensionOverride() {
    if (this.getAttr("overriddenBy")) {
      if (this._overriddenData) {
        this._urls = this._overriddenData.urls;
        this._queryCharset = this._overriddenData.queryCharset;
        delete this._overriddenData;
      } else {
        lazy.logConsole.error(
          `${this._name} had overriddenBy set, but no _overriddenData`
        );
      }
      this.clearAttr("overriddenBy");
      this.clearAttr("overriddenURL");
      lazy.SearchUtils.notifyAction(
        this,
        lazy.SearchUtils.MODIFIED_TYPE.CHANGED
      );
    }
  }

  copyUserSettingsFrom(engine) {
    for (let attribute of USER_ATTRIBUTES) {
      if (attribute in engine._metaData) {
        this._metaData[attribute] = engine._metaData[attribute];
      }
    }
  }

  _initWithJSON(json) {
    this.#id = json.id ?? this.#id;
    this._name = json._name;
    this._queryCharset =
      json.queryCharset || lazy.SearchUtils.DEFAULT_QUERY_CHARSET;
    this._iconMapObj = json._iconMapObj || null;
    this._metaData = json._metaData || {};
    this._definedAliases = json._definedAliases || [];
    if (json._definedAlias) {
      this._definedAliases.push(json._definedAlias);
    }
    this._filePath = json.filePath || json._filePath || null;

    for (let i = 0; i < json._urls.length; ++i) {
      let url = json._urls[i];
      let engineURL = new EngineURL({
        ...url,
        type: url.type || lazy.SearchUtils.URL_TYPE.SEARCH,
      });
      engineURL._initWithJSON(url);
      this._urls.push(engineURL);
    }
  }

  toJSON() {
    const fieldsToCopy = [
      "id",
      "_name",
      "_loadPath",
      "_iconMapObj",
      "_metaData",
      "_urls",
      "_filePath",
      "_definedAliases",
    ];

    let json = {};
    for (const field of fieldsToCopy) {
      if (field in this) {
        json[field] = this[field];
      }
    }

    if (this.queryCharset != lazy.SearchUtils.DEFAULT_QUERY_CHARSET) {
      json.queryCharset = this.queryCharset;
    }

    return json;
  }

  getAttr(name) {
    return this._metaData[name] || undefined;
  }

  setAttr(name, val, sendNotification = false) {
    let hasChangedAttr = val != this[name];
    this._metaData[name] = val;
    if (hasChangedAttr && sendNotification) {
      lazy.SearchUtils.notifyAction(
        this,
        lazy.SearchUtils.MODIFIED_TYPE.CHANGED
      );
    }
  }

  clearAttr(name) {
    delete this._metaData[name];
  }

  get partnerCode() {
    return "";
  }

  _loadSettings(settings) {
    if (!settings) {
      return;
    }

    let engineSettings = lazy.SearchSettings.findSettingsForEngine(
      settings,
      this.id,
      this.name
    );
    if (engineSettings?._metaData) {
      this._metaData = structuredClone(engineSettings._metaData);
    }
  }

  get orderHint() {
    return null;
  }

  get alias() {
    return this.getAttr("alias") || "";
  }

  set alias(val) {
    var value = val ? val.trim() : "";
    this.setAttr("alias", value, true);
  }

  get aliases() {
    return [
      ...(this.getAttr("alias") ? [this.getAttr("alias")] : []),
      ...this._definedAliases,
    ];
  }

  get telemetryId() {
    return `other-${this.name}`;
  }

  get hidden() {
    return this.getAttr("hidden") || false;
  }

  set hidden(val) {
    var value = !!val;
    this.setAttr("hidden", value, true);
  }

  get hideOneOffButton() {
    if (lazy.settingsRedesignEnabled) {
      return false;
    }
    return this.getAttr("hideOneOffButton") || false;
  }

  set hideOneOffButton(val) {
    const value = !!val;
    this.setAttr("hideOneOffButton", value, true);
  }

  get inMemory() {
    return false;
  }

  get overriddenById() {
    return this.getAttr("overriddenBy");
  }

  get isGeneralPurposeEngine() {
    return false;
  }

  get name() {
    return this._name;
  }

  get loadPath() {
    return this._loadPath;
  }

  get queryCharset() {
    return this._queryCharset || lazy.SearchUtils.DEFAULT_QUERY_CHARSET;
  }

  getSubmission(searchTerms, responseType) {
    if (!responseType) {
      responseType = lazy.SearchUtils.URL_TYPE.SEARCH;
    }

    var url = this.getURLOfType(responseType);

    if (!url) {
      return null;
    }

    if (
      !searchTerms &&
      (responseType == lazy.SearchUtils.URL_TYPE.SEARCH ||
        responseType == lazy.SearchUtils.URL_TYPE.SUGGEST_JSON)
    ) {
      lazy.logConsole.warn("getSubmission: searchTerms is empty!");
    }

    return url.getSubmission(searchTerms, this.queryCharset);
  }

  get searchURLWithNoTerms() {
    return this.getURLOfType(lazy.SearchUtils.URL_TYPE.SEARCH).getSubmission(
      "",
      this.queryCharset
    ).uri;
  }

  searchTermFromResult(uri) {
    let url = this.getURLOfType(lazy.SearchUtils.URL_TYPE.SEARCH);
    if (!url) {
      return "";
    }

    let url1 = new URL(url.template);
    let url2 = URL.fromURI(uri);
    if (url1.origin != url2.origin || url1.pathname != url2.pathname) {
      return "";
    }

    let engineParams;
    if (url.params.length) {
      engineParams = new URLSearchParams();
      for (let { name, value } of url.params) {
        if (value) {
          engineParams.append(name, value);
        }
      }
    } else {
      engineParams = url1.searchParams;
    }

    let uriParams = url2.searchParams;
    if (
      new Set([...uriParams.keys()]).size !=
      new Set([...engineParams.keys()]).size
    ) {
      return "";
    }

    let termsParameterName = this.searchUrlQueryParamName;

    for (let [name, value] of uriParams.entries()) {
      if (name == termsParameterName) {
        continue;
      }
      if (!engineParams.getAll(name).includes(value)) {
        return "";
      }
    }

    if (this.queryCharset.toLowerCase() != "utf-8") {
      let name = `${termsParameterName}=`;
      let queryString = uri.query
        .split("&")
        .filter(str => str.startsWith(name))
        .pop();
      return Services.textToSubURI.UnEscapeAndConvert(
        this.queryCharset,
        queryString.substring(queryString.indexOf("=") + 1).replace(/\+/g, " ")
      );
    }

    return uriParams.get(termsParameterName) ?? "";
  }

  get searchUrlQueryParamName() {
    return (
      this.getURLOfType(lazy.SearchUtils.URL_TYPE.SEARCH).searchTermParamName ||
      ""
    );
  }

  get searchUrlPublicSuffix() {
    if (this._searchUrlPublicSuffix != null) {
      return this._searchUrlPublicSuffix;
    }
    let searchURLPublicSuffix = Services.eTLD.getKnownPublicSuffix(
      this.searchURLWithNoTerms
    );
    return (this._searchUrlPublicSuffix = searchURLPublicSuffix);
  }

  supportsResponseType(type) {
    return this.getURLOfType(type) != null;
  }

  get searchUrlDomain() {
    let url = this.getURLOfType(lazy.SearchUtils.URL_TYPE.SEARCH);
    if (url) {
      return url.templateHost;
    }
    return "";
  }

  get searchForm() {
    let url = this.getURLOfType(lazy.SearchUtils.URL_TYPE.SEARCH_FORM);
    if (url) {
      return url.getSubmission("", this.queryCharset).uri.spec;
    }
    return this.searchURLWithNoTerms.prePath;
  }

  getURLParsingInfo() {
    let url = this.getURLOfType(lazy.SearchUtils.URL_TYPE.SEARCH);
    if (!url || url.method != "GET") {
      return null;
    }

    let termsParameterName = url.searchTermParamName;
    if (!termsParameterName) {
      return null;
    }

    let templateUrl = Services.io.newURI(url.template);
    return {
      mainDomain: templateUrl.host,
      path: templateUrl.filePath.toLowerCase(),
      termsParameterName,
    };
  }

  async getIconURL(preferredWidth) {
    preferredWidth ||= 16;

    if (!this._iconMapObj) {
      return undefined;
    }

    let availableWidths = Object.keys(this._iconMapObj).map(k => parseInt(k));
    if (!availableWidths.length) {
      return undefined;
    }

    let bestWidth = lazy.SearchUtils.chooseIconSize(
      preferredWidth,
      availableWidths
    );
    return this._iconMapObj[bestWidth];
  }

  speculativeConnect(options) {
    if (!options || !options.window) {
      console.error(
        "invalid options arg passed to SearchEngine.speculativeConnect"
      );
      throw new TypeError("invalid options arguments");
    }
    let connector = Services.io.QueryInterface(Ci.nsISpeculativeConnect);

    let searchURI = this.searchURLWithNoTerms;

    let callbacks = options.window.docShell.QueryInterface(
      Ci.nsIInterfaceRequestor
    );

    let attrs = options.originAttributes;

    if (!attrs) {
      attrs = options.window.docShell.getOriginAttributes();
    }

    let principal = Services.scriptSecurityManager.createContentPrincipal(
      searchURI,
      attrs
    );

    try {
      connector.speculativeConnect(searchURI, principal, callbacks, false);
    } catch (e) {
      console.error(e);
    }

    if (this.supportsResponseType(lazy.SearchUtils.URL_TYPE.SUGGEST_JSON)) {
      let suggestURI = this.getSubmission(
        "dummy",
        lazy.SearchUtils.URL_TYPE.SUGGEST_JSON
      ).uri;
      if (suggestURI.prePath != searchURI.prePath) {
        try {
          connector.speculativeConnect(suggestURI, principal, callbacks, false);
        } catch (e) {
          console.error(e);
        }
      }
    }
  }

  get id() {
    return this.#id;
  }

  isNew() {
    return isDateStringTodayOrFuture(this.isNewUntil);
  }

  #uuid() {
    let uuid = Services.uuid.generateUUID().toString();
    return uuid.slice(1, uuid.length - 1);
  }
}
