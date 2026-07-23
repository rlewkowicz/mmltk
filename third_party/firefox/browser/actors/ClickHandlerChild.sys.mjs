/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  E10SUtils: "resource://gre/modules/E10SUtils.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "autoscrollEnabled",
  "general.autoScroll",
  true
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "blockJavascript",
  "browser.link.alternative_click.block_javascript",
  true
);

export class MiddleMousePasteHandlerChild extends JSWindowActorChild {
  handleEvent(clickEvent) {
    if (
      clickEvent.defaultPrevented ||
      clickEvent.button != 1 ||
      lazy.autoscrollEnabled
    ) {
      return;
    }
    this.manager
      .getActor("ClickHandler")
      .handleClickEvent(
        clickEvent,
         true
      );
  }

  onProcessedClick(data) {
    this.sendAsyncMessage("MiddleClickPaste", data);
  }
}

export class ClickHandlerChild extends JSWindowActorChild {
  handleEvent(wrapperEvent) {
    this.handleClickEvent(wrapperEvent.sourceEvent);
  }

  handleClickEvent(event, isFromMiddleMousePasteHandler = false) {
    if (event.defaultPrevented || event.button == 2) {
      return;
    }
    let composedTarget = event.composedTarget;
    if (
      composedTarget.isContentEditable ||
      (composedTarget.ownerDocument &&
        composedTarget.ownerDocument.designMode == "on") ||
      ChromeUtils.getClassName(composedTarget) == "HTMLInputElement" ||
      ChromeUtils.getClassName(composedTarget) == "HTMLTextAreaElement"
    ) {
      return;
    }

    let originalTarget = event.originalTarget;
    let ownerDoc = originalTarget.ownerDocument;
    if (!ownerDoc) {
      return;
    }

    if (event.button == 0) {
      if (ownerDoc.documentURI.startsWith("about:blocked")) {
        return;
      }
    }

    if (!event.isTrusted && !ownerDoc.hasValidTransientUserGestureActivation) {
      return;
    }

    let [href, node, principal] =
      lazy.BrowserUtils.hrefAndLinkNodeForClickEvent(event);

    let policyContainer = ownerDoc.policyContainer;
    if (policyContainer) {
      policyContainer =
        lazy.E10SUtils.serializePolicyContainer(policyContainer);
    }

    let referrerInfo = Cc["@mozilla.org/referrer-info;1"].createInstance(
      Ci.nsIReferrerInfo
    );
    if (node) {
      referrerInfo.initWithElement(node);
    } else {
      referrerInfo.initWithDocument(ownerDoc);
    }
    referrerInfo = lazy.E10SUtils.serializeReferrerInfo(referrerInfo);

    let json = {
      button: event.button,
      shiftKey: event.shiftKey,
      ctrlKey: event.ctrlKey,
      metaKey: event.metaKey,
      altKey: event.altKey,
      href: null,
      title: null,
      policyContainer,
      referrerInfo,
    };

    if (href && !isFromMiddleMousePasteHandler) {
      if (
        lazy.blockJavascript &&
        Services.io.extractScheme(href) == "javascript"
      ) {
        return;
      }

      try {
        Services.scriptSecurityManager.checkLoadURIStrWithPrincipal(
          principal,
          href
        );
      } catch (e) {
        return;
      }

      if (
        !event.isTrusted &&
        lazy.BrowserUtils.whereToOpenLink(event) != "current"
      ) {
        ownerDoc.consumeTransientUserGestureActivation();
      }

      json.href = href;
      if (node) {
        json.title = node.getAttribute("title");
      }

      if (
        (ownerDoc.URL === "about:newtab" || ownerDoc.URL === "about:home") &&
        node.dataset.isSponsoredLink === "true"
      ) {
        json.globalHistoryOptions = {
          triggeringSource: "newtab",
          triggeringSponsoredURL: href,
        };
      }

      event.preventMultipleActions();

      this.sendAsyncMessage("Content:Click", json);
    }

    if (!href && event.button == 1 && isFromMiddleMousePasteHandler) {
      this.manager.getActor("MiddleMousePasteHandler").onProcessedClick(json);
    }
  }
}
