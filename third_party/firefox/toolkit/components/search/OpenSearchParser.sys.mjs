/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const OPENSEARCH_NS_10 = "http://a9.com/-/spec/opensearch/1.0/";
const OPENSEARCH_NS_11 = "http://a9.com/-/spec/opensearch/1.1/";

const OPENSEARCH_NAMESPACES = [
  OPENSEARCH_NS_11,
  OPENSEARCH_NS_10,
  "http://a9.com/-/spec/opensearchdescription/1.1/",
  "http://a9.com/-/spec/opensearchdescription/1.0/",
];

const OPENSEARCH_LOCALNAME = "OpenSearchDescription";

const MOZSEARCH_NS_10 = "http://www.mozilla.org/2006/browser/search/";
const MOZSEARCH_LOCALNAME = "SearchPlugin";

const URL_TYPE_SUGGEST_JSON = "application/x-suggestions+json";
const URL_TYPE_SEARCH = "text/html";




export class OpenSearchParser {
  static parseXMLData(xmlData) {
    var parser = new DOMParser();
    var doc = parser.parseFromBuffer(xmlData, "text/xml");

    if (!doc?.documentElement) {
      return { error: "Could not parse file" };
    }

    let element = doc.documentElement;
    if (!hasExpectedNamespace(element)) {
      return { error: "Not a valid OpenSearch xml file" };
    }

    try {
      return { data: processXMLDocument(element) };
    } catch (ex) {
      return { error: ex.message };
    }
  }
}

function processXMLDocument(xmlDocument) {
  let result = { name: "", urls: [], images: [] };

  for (let i = 0; i < xmlDocument.children.length; ++i) {
    var child = xmlDocument.children[i];
    switch (child.localName) {
      case "ShortName":
        result.name = child.textContent;
        break;
      case "Url":
        try {
          result.urls.push(parseURL(child));
        } catch (ex) {
          console.error("Failed to parse URL child:", ex);
        }
        break;
      case "Image": {
        let imageData = parseImage(child);
        if (imageData) {
          result.images.push(imageData);
        }
        break;
      }
      case "InputEncoding":
        result.queryCharset = child.textContent;
        break;

      case "SearchForm":
        result.searchForm = child.textContent;
        break;
      case "UpdateUrl":
        result.updateURL = child.textContent;
        break;
      case "UpdateInterval":
        result.updateInterval = parseInt(child.textContent);
        break;
    }
  }
  if (!result.name || !result.urls.length) {
    throw new Error("No name, or missing URL for search engine");
  }
  if (!result.urls.find(url => url.type == URL_TYPE_SEARCH)) {
    throw new Error("Missing text/html result type in URLs for search engine");
  }
  return result;
}

function parseURL(element) {
  var type = element.getAttribute("type");
  var method = element.getAttribute("method") || "GET";
  var template = element.getAttribute("template");

  let rels = [];
  if (element.hasAttribute("rel")) {
    rels = element.getAttribute("rel").toLowerCase().split(/\s+/);
  }

  if (type == "application/json" && rels.includes("suggestions")) {
    type = URL_TYPE_SUGGEST_JSON;
  }

  let url = {
    type,
    method,
    template,
    params: [],
    rels,
  };

  for (var i = 0; i < element.children.length; ++i) {
    var param = element.children[i];
    if (param.localName == "Param") {
      url.params.push({
        name: param.getAttribute("name"),
        value: param.getAttribute("value"),
      });
    }
  }

  return url;
}

function parseImage(element) {
  let width = parseInt(element.getAttribute("width"), 10);
  let height = parseInt(element.getAttribute("height"), 10);

  if (isNaN(width) || isNaN(height) || width <= 0 || width != height) {
    console.warn(
      "OpenSearch image element must have equal and positive width and height."
    );
    return null;
  }

  return {
    url: element.textContent,
    size: width,
  };
}

function hasExpectedNamespace(element) {
  return (
    (element.localName == MOZSEARCH_LOCALNAME &&
      element.namespaceURI == MOZSEARCH_NS_10) ||
    (element.localName == OPENSEARCH_LOCALNAME &&
      OPENSEARCH_NAMESPACES.includes(element.namespaceURI))
  );
}
