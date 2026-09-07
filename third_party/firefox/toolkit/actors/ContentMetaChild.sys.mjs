/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const TIMEOUT_DELAY = 1000;

const ACCEPTED_PROTOCOLS = new Set(["http:", "https:"]);

const DESCRIPTION_RULES = [
  "twitter:description",
  "description",
  "og:description",
];

const PREVIEW_IMAGE_RULES = [
  "thumbnail",
  "twitter:image",
  "og:image",
  "og:image:url",
  "og:image:secure_url",
];

function shouldExtractMetadata(aRules, aTag, aEntry) {
  return aRules.indexOf(aTag) > aEntry.currMaxScore;
}

function checkLoadURIStr(aURL) {
  if (!ACCEPTED_PROTOCOLS.has(aURL.protocol)) {
    return false;
  }
  try {
    let ssm = Services.scriptSecurityManager;
    let principal = ssm.createNullPrincipal({});
    ssm.checkLoadURIStrWithPrincipal(
      principal,
      aURL.href,
      ssm.DISALLOW_INHERIT_PRINCIPAL
    );
  } catch (e) {
    return false;
  }
  return true;
}

export class ContentMetaChild extends JSWindowActorChild {
  constructor() {
    super();

    this.metaTags = new Map();
  }

  didDestroy() {
    for (let entry of this.metaTags.values()) {
      if (entry.timeout) {
        entry.timeout.cancel();
      }
    }
  }

  handleEvent(event) {
    switch (event.type) {
      case "DOMContentLoaded": {
        const metaTags = this.contentWindow.document.querySelectorAll("meta");
        for (let metaTag of metaTags) {
          this.onMetaTag(metaTag);
        }
        break;
      }
      case "DOMMetaAdded":
        this.onMetaTag(event.originalTarget);
        break;
      default:
    }
  }

  onMetaTag(metaTag) {
    const window = metaTag.documentGlobal;

    if (!metaTag || !metaTag.ownerDocument || window != this.contentWindow) {
      return;
    }

    const url = metaTag.ownerDocument.documentURI;

    let name = metaTag.name;
    let prop = metaTag.getAttributeNS(null, "property");
    if (!name && !prop) {
      return;
    }

    let tag = name || prop;

    const entry = this.metaTags.get(url) || {
      description: { value: null, currMaxScore: -1 },
      image: { value: null, currMaxScore: -1 },
      timeout: null,
    };

    if (!entry.timeout) {
      entry.timeout = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
    }

    const content = metaTag.getAttributeNS(null, "content");
    if (!content) {
      return;
    }

    if (shouldExtractMetadata(DESCRIPTION_RULES, tag, entry.description)) {
      entry.description.value = content;
      entry.description.currMaxScore = DESCRIPTION_RULES.indexOf(tag);
    } else if (shouldExtractMetadata(PREVIEW_IMAGE_RULES, tag, entry.image)) {
      let value = URL.parse(content, url);
      if (!value) {
        return;
      }
      if (checkLoadURIStr(value)) {
        entry.image.value = value.href;
        entry.image.currMaxScore = PREVIEW_IMAGE_RULES.indexOf(tag);
      }
    } else {
      return;
    }

    if (!this.metaTags.has(url)) {
      this.metaTags.set(url, entry);
    }

    entry.timeout.initWithCallback(
      () => {
        entry.timeout = null;
        this.metaTags.delete(url);
        if (!this.manager || this.manager.isClosed) {
          return;
        }

        this.sendAsyncMessage("Meta:SetPageInfo", {
          url,
          description: entry.description.value,
          previewImageURL: entry.image.value,
        });
      },
      TIMEOUT_DELAY,
      Ci.nsITimer.TYPE_ONE_SHOT
    );
  }
}
