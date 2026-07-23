/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});


export class L10nCache {
  static MAX_ENTRIES_PER_ID = 5;

  constructor(l10n) {
    this.l10n = l10n ?? document.l10n;
    this.QueryInterface = ChromeUtils.generateQI([
      "nsIObserver",
      "nsISupportsWeakReference",
    ]);
    Services.obs.addObserver(this, "intl:app-locales-changed", true);
  }

  get({ id, args = undefined }) {
    return this.#messagesByArgsById.get(id)?.get(this.#argsKey(args)) ?? null;
  }

  async add({ id, args = undefined }) {
    let messages = await this.l10n.formatMessages([{ id, args }]);
    if (!messages?.length) {
      console.error(
        "l10n.formatMessages returned an unexpected value for ID: ",
        id
      );
      return;
    }

    let message = { value: messages[0].value, attributes: null };
    if (messages[0].attributes) {
      message.attributes = messages[0].attributes.reduce((valuesByName, a) => {
        valuesByName[a.name] = a.value;
        return valuesByName;
      }, {});
    }

    this.#update({ id, args, message });
  }

  async ensure({ id, args = undefined }) {
    let message = this.get({ id, args });
    if (message) {
      this.#update({ id, args, message });
    } else {
      await this.add({ id, args });
    }
  }

  async ensureAll(objects) {
    let promises = [];
    for (let obj of objects) {
      promises.push(this.ensure(obj));
    }
    await Promise.all(promises);
  }

  delete({ id, args = undefined }) {
    let messagesByArgs = this.#messagesByArgsById.get(id);
    if (messagesByArgs) {
      messagesByArgs.delete(this.#argsKey(args));
      if (!messagesByArgs.size) {
        this.#messagesByArgsById.delete(id);
      }
    }
  }

  clear() {
    this.#messagesByArgsById.clear();
  }

  size() {
    return this.#messagesByArgsById
      .values()
      .reduce((total, messagesByArg) => total + messagesByArg.size, 0);
  }

  setElementL10n(
    element,
    {
      id,
      args = undefined,
      argsHighlights = undefined,
      attribute = undefined,
      parseMarkup = false,
    }
  ) {
    let message = this.get({ id, args });
    if (message) {
      if (message.attributes) {
        for (let [key, value] of Object.entries(message.attributes)) {
          element.setAttribute(key, value);
        }
      }
      if (typeof message.value == "string") {
        if (!parseMarkup) {
          element.textContent = message.value;
        } else {
          element.setHTML(message.value, {
            sanitizer: {
              elements: ["a", "br", "em", "span", "strong"],
              attributes: ["data-l10n-name", "href"],
            },
          });
        }
      }
    }

    if (!message && !attribute && argsHighlights) {
      args = { ...args };

      let span = element.ownerDocument.createElement("span");
      for (let key in argsHighlights) {
        lazy.UrlbarUtils.addTextContentWithHighlights(
          span,
          args[key],
          argsHighlights[key]
        );
        args[key] = span.innerHTML;
      }
    }

    if (attribute) {
      element.setAttribute("data-l10n-attrs", attribute);
    } else {
      element.removeAttribute("data-l10n-attrs");
    }

    element.ownerDocument.l10n.setAttributes(element, id, args);

    return this.ensure({ id, args });
  }

  removeElementL10n(element, { attribute = undefined } = {}) {
    if (attribute) {
      element.removeAttribute(attribute);
      element.removeAttribute("data-l10n-attrs");
    } else {
      element.textContent = "";
    }
    element.removeAttribute("data-l10n-id");
    element.removeAttribute("data-l10n-args");
  }

  async observe(subject, topic) {
    switch (topic) {
      case "intl:app-locales-changed": {
        this.clear();
        break;
      }
    }
  }

  #messagesByArgsById = new Map();

  #maxEntriesPerId = L10nCache.MAX_ENTRIES_PER_ID;

  #update({ id, args, message }) {
    let messagesByArgs = this.#messagesByArgsById.get(id);
    if (!messagesByArgs) {
      messagesByArgs = new Map();
      this.#messagesByArgsById.set(id, messagesByArgs);
    }

    let argsKey = this.#argsKey(args);

    messagesByArgs.delete(argsKey);

    if (messagesByArgs.size == this.#maxEntriesPerId) {
      messagesByArgs.delete(messagesByArgs.keys().next().value);
    }

    messagesByArgs.set(argsKey, message);
  }

  #argsKey(args) {
    let argValues = Object.entries(args ?? [])
      .sort(([key1], [key2]) => key1.localeCompare(key2))
      .map(([_, value]) => value);
    return JSON.stringify(argValues);
  }
}
