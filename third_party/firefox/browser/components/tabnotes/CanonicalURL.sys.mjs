/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export function findCandidates(document) {
  return {
    link: getLinkRelCanonical(document),
    opengraph: getOpenGraphUrl(document),
    jsonLd: getJSONLDUrl(document),
    fallback: getFallbackCanonicalUrl(document),
  };
}

export function pickCanonicalUrl(sources) {
  return (
    sources.link ?? sources.opengraph ?? sources.jsonLd ?? sources.fallback
  );
}

export function getLinkRelCanonical(document) {
  const url = document
    .querySelector('link[rel="canonical"]')
    ?.getAttribute("href");

  return parseUrl(url, document);
}

export function getOpenGraphUrl(document) {
  const url = document
    .querySelector('meta[property="og:url"]')
    ?.getAttribute("content");

  return parseUrl(url, document);
}

export function getJSONLDUrl(document) {
  const firstMatch = Array.from(
    document.querySelectorAll('script[type="application/ld+json"]')
  )
    .map(script => {
      try {
        return JSON.parse(script.textContent);
      } catch {
        return null;
      }
    })
    .find(obj => obj && typeof obj.url === "string");
  const url = firstMatch?.url;

  return parseUrl(url, document);
}

export function getFallbackCanonicalUrl(document) {
  return cleanNoncanonicalUrl(document.documentURI);
}

export function cleanNoncanonicalUrl(url) {
  const parsed = URL.parse(url);
  if (parsed) {
    return [parsed.origin, parsed.pathname, parsed.search].join("");
  }
  return null;
}

export function parseUrl(urlString, document) {
  if (urlString == null) {
    return null;
  }

  return URL.parse(urlString, document.documentURI)?.toString() || null;
}
