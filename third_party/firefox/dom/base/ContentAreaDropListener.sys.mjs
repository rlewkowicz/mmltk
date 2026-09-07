/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


export function ContentAreaDropListener() {}

ContentAreaDropListener.prototype = {
  classID: Components.ID("{1f34bc80-1bc7-11d6-a384-d705dd0746fc}"),
  QueryInterface: ChromeUtils.generateQI(["nsIDroppedLinkHandler"]),

  _addLink(links, url, name, type) {
    links.push({ url, name, type });
  },

  _addLinksFromItem(links, dt, i) {
    let types = dt.mozTypesAt(i);
    let type, data;

    type = "text/uri-list";
    if (types.contains(type)) {
      data = dt.mozGetDataAt(type, i);
      if (data) {
        let urls = data.split("\n");
        for (let url of urls) {
          if (url.startsWith("#")) {
            continue;
          }
          url = url.replace(/^\s+|\s+$/g, "");
          this._addLink(links, url, url, type);
        }
        return;
      }
    }

    type = "text/x-moz-url";
    if (types.contains(type)) {
      data = dt.mozGetDataAt(type, i);
      if (data) {
        let lines = data.split("\n");
        for (let i = 0, length = lines.length; i < length; i += 2) {
          this._addLink(links, lines[i], lines[i + 1], type);
        }
        return;
      }
    }

    for (let type of ["text/plain", "text/x-moz-text-internal"]) {
      if (types.contains(type)) {
        data = dt.mozGetDataAt(type, i);
        if (data) {
          const maxNonUriLines = 50;
          let numNonLinks = 0;
          let hasURI = false;
          let flags =
            Ci.nsIURIFixup.FIXUP_FLAG_FIX_SCHEME_TYPOS |
            Ci.nsIURIFixup.FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP;
          for (let line of data.split("\n")) {
            line = line.trim();
            if (!line || line.startsWith("#")) {
              continue;
            }

            let info = Services.uriFixup.getFixupURIInfo(line, flags);
            if (info.fixedURI && !info.keywordProviderId) {
              hasURI = true;
              this._addLink(links, line, line, type);
            } else {
              numNonLinks++;
              if (numNonLinks > maxNonUriLines) {
                break;
              }
            }
          }

          if (!hasURI && numNonLinks > 0) {
            this._addLink(links, data.replace(/\s+/g, " ").trim(), data, type);
          }
          return;
        }
      }
    }

    let files = dt.files;
    if (files && i < files.length) {
      this._addLink(
        links,
        PathUtils.toFileURI(files[i].mozFullPath),
        files[i].name,
        "application/x-moz-file"
      );
    }
  },

  _getDropLinks(dt) {
    let links = [];
    for (let i = 0; i < dt.mozItemCount; i++) {
      this._addLinksFromItem(links, dt, i);
    }
    return links;
  },

  _validateURI(dataTransfer, uriString, disallowInherit, triggeringPrincipal) {
    if (!uriString) {
      return "";
    }

    uriString = uriString.replace(/^\s*|\s*$/g, "");

    let fixupFlags =
      Ci.nsIURIFixup.FIXUP_FLAG_FIX_SCHEME_TYPOS |
      Ci.nsIURIFixup.FIXUP_FLAG_ALLOW_KEYWORD_LOOKUP;
    let info = Services.uriFixup.getFixupURIInfo(uriString, fixupFlags);
    if (!info.fixedURI || info.keywordProviderId) {
      return uriString;
    }
    let uri = info.fixedURI;

    let secMan = Services.scriptSecurityManager;
    let flags = secMan.STANDARD;
    if (disallowInherit) {
      flags |= secMan.DISALLOW_INHERIT_PRINCIPAL;
    }

    secMan.checkLoadURIWithPrincipal(triggeringPrincipal, uri, flags);

    return uri.spec;
  },

  _getTriggeringPrincipalFromDataTransfer(
    aDataTransfer,
    fallbackToSystemPrincipal
  ) {
    let sourceNode = aDataTransfer.mozSourceNode;
    if (
      sourceNode &&
      (sourceNode.localName !== "browser" ||
        sourceNode.namespaceURI !==
          "http://www.mozilla.org/keymaster/gatekeeper/there.is.only.xul")
    ) {
      if (sourceNode.nodePrincipal) {
        return sourceNode.nodePrincipal;
      }
    }

    let principalURISpec = aDataTransfer.mozTriggeringPrincipalURISpec;
    if (!principalURISpec) {
      if (fallbackToSystemPrincipal) {
        return Services.scriptSecurityManager.getSystemPrincipal();
      }

      principalURISpec = "file:///";
    }
    return Services.scriptSecurityManager.createContentPrincipal(
      Services.io.newURI(principalURISpec),
      {}
    );
  },

  getTriggeringPrincipal(aEvent) {
    let dataTransfer = aEvent.dataTransfer;
    return this._getTriggeringPrincipalFromDataTransfer(dataTransfer, true);
  },

  getPolicyContainer(aEvent) {
    let sourceNode = aEvent.dataTransfer.mozSourceNode;
    if (aEvent.dataTransfer.policyContainer !== null) {
      return aEvent.dataTransfer.policyContainer;
    }

    if (
      sourceNode &&
      (sourceNode.localName !== "browser" ||
        sourceNode.namespaceURI !==
          "http://www.mozilla.org/keymaster/gatekeeper/there.is.only.xul")
    ) {
      return sourceNode.policyContainer;
    }
    return null;
  },

  canDropLink(aEvent, aAllowSameDocument) {
    if (this._eventTargetIsDisabled(aEvent)) {
      return false;
    }

    let dataTransfer = aEvent.dataTransfer;
    let types = dataTransfer.types;
    if (
      !dataTransfer.files.length &&
      !types.includes("text/x-moz-url") &&
      !types.includes("text/uri-list") &&
      !types.includes("text/x-moz-text-internal") &&
      !types.includes("text/plain")
    ) {
      return false;
    }

    if (aAllowSameDocument) {
      return true;
    }

    let sourceTopWC = dataTransfer.sourceTopWindowContext;
    if (!sourceTopWC) {
      return true;
    }

    let eventWC =
      aEvent.originalTarget.documentGlobal.browsingContext.currentWindowContext;
    if (eventWC && sourceTopWC == eventWC.topWindowContext) {
      return false;
    }

    return true;
  },

  dropLinks(aEvent, aDisallowInherit) {
    if (aEvent && this._eventTargetIsDisabled(aEvent)) {
      return [];
    }

    let dataTransfer = aEvent.dataTransfer;
    let links = this._getDropLinks(dataTransfer);
    let triggeringPrincipal = this._getTriggeringPrincipalFromDataTransfer(
      dataTransfer,
      false
    );

    for (let link of links) {
      try {
        link.url = this._validateURI(
          dataTransfer,
          link.url,
          aDisallowInherit,
          triggeringPrincipal
        );
      } catch (ex) {
        aEvent.stopPropagation();
        aEvent.preventDefault();
        throw ex;
      }
    }

    return links;
  },

  validateURIsForDrop(aEvent, aURIs, aDisallowInherit) {
    let dataTransfer = aEvent.dataTransfer;
    let triggeringPrincipal = this._getTriggeringPrincipalFromDataTransfer(
      dataTransfer,
      false
    );

    for (let uri of aURIs) {
      this._validateURI(
        dataTransfer,
        uri,
        aDisallowInherit,
        triggeringPrincipal
      );
    }
  },

  queryLinks(aDataTransfer) {
    return this._getDropLinks(aDataTransfer);
  },

  _eventTargetIsDisabled(aEvent) {
    let ownerDoc = aEvent.originalTarget.ownerDocument;
    if (!ownerDoc || !ownerDoc.defaultView) {
      return false;
    }

    return ownerDoc.defaultView.windowUtils.isNodeDisabledForEvents(
      aEvent.originalTarget
    );
  },
};
