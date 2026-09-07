/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  SearchService: "moz-src:///toolkit/components/search/SearchService.sys.mjs",
});

export class AboutPrivateBrowsingParent extends JSWindowActorParent {
  receiveMessage(aMessage) {
    let browser = this.browsingContext.top.embedderElement;
    if (!browser) {
      return undefined;
    }

    let win = browser.documentGlobal;

    switch (aMessage.name) {
      case "OpenPrivateWindow": {
        win.OpenBrowserWindow({ private: true });
        break;
      }
      case "SearchHandoff": {
        let urlBar = win.gURLBar;
        let searchEngine = lazy.SearchService.defaultPrivateEngine;
        let isFirstChange = true;

        if (!aMessage.data || !aMessage.data.text) {
          urlBar.setHiddenFocus();
        } else {
          urlBar.handoff(aMessage.data.text, searchEngine);
          isFirstChange = false;
        }

        let checkFirstChange = () => {
          if (isFirstChange) {
            isFirstChange = false;
            urlBar.removeHiddenFocus(true);
            urlBar.handoff("", searchEngine);
            this.sendAsyncMessage("DisableSearch");
            urlBar.removeEventListener("compositionstart", checkFirstChange);
            urlBar.removeEventListener("paste", checkFirstChange);
          }
        };

        let onKeydown = ev => {
          if (ev.key.length === 1 && !ev.altKey && !ev.ctrlKey && !ev.metaKey) {
            checkFirstChange();
          }
          if (ev.key === "Escape") {
            onDone();
          }
        };

        let onDone = ev => {
          this.sendAsyncMessage("ShowSearch");

          const forceSuppressFocusBorder = ev?.type === "mousedown";
          urlBar.removeHiddenFocus(forceSuppressFocusBorder);

          urlBar.inputField.removeEventListener("keydown", onKeydown);
          urlBar.inputField.removeEventListener("mousedown", onDone);
          urlBar.inputField.removeEventListener("blur", onDone);
          urlBar.inputField.removeEventListener(
            "compositionstart",
            checkFirstChange
          );
          urlBar.inputField.removeEventListener("paste", checkFirstChange);
        };

        urlBar.inputField.addEventListener("keydown", onKeydown);
        urlBar.inputField.addEventListener("mousedown", onDone);
        urlBar.inputField.addEventListener("blur", onDone);
        urlBar.inputField.addEventListener(
          "compositionstart",
          checkFirstChange
        );
        urlBar.inputField.addEventListener("paste", checkFirstChange);
        break;
      }
    }

    return undefined;
  }
}
