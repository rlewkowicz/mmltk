/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

class MozTextLabel extends HTMLLabelElement {
  #insertSeparator = false;
  #alwaysAppendAccessKey = false;
  #lastFormattedAccessKey = null;
  #observer = null;

  static #underlineAccesskey = !navigator.platform.includes("Mac");
  static get observedAttributes() {
    return ["accesskey", "shownaccesskey"];
  }

  static stylesheetUrl = "chrome://global/content/elements/moz-label.css";

  constructor() {
    super();
    this.#register();
    this.addEventListener("click", this._onClick);
  }

  #register() {
    if (window.IS_STORYBOOK) {
      MozTextLabel.#underlineAccesskey = true;
    } else if (typeof Services !== "undefined") {
      MozTextLabel.#underlineAccesskey = !!Services.prefs.getIntPref(
        "ui.key.menuAccessKey",
        Number(!navigator.platform.includes("Mac"))
      );
      if (MozTextLabel.#underlineAccesskey) {
        try {
          this.#insertSeparator =
            Services.locale.insertSeparatorBeforeAccesskeys;
          this.#alwaysAppendAccessKey = Services.locale.alwaysAppendAccesskeys;
        } catch {
          this.#insertSeparator = this.#alwaysAppendAccessKey = true;
        }
      }
    }
  }

  #startMutationObserver() {
    if (!this.#observer) {
      return;
    }
    this.#observer.observe(this, {
      characterData: true,
      childList: true,
      subtree: true,
    });
  }

  #stopMutationObserver() {
    if (!this.#observer) {
      return;
    }
    this.#observer.disconnect();
  }

  connectedCallback() {
    this.#setStyles();
    this.formatAccessKey();
    if (!this.#observer) {
      this.#observer = new MutationObserver(() => {
        this.#lastFormattedAccessKey = null;
        this.formatAccessKey();
      });
      this.#startMutationObserver();
    }
  }

  disconnectedCallback() {
    if (this.#observer) {
      this.#stopMutationObserver();
      this.#observer = null;
    }
  }

  #setStyles() {
    let root = this.getRootNode();
    if (root.__mozLabelCssAdded) {
      return;
    }

    let container = root.head ?? root;

    for (let link of container.querySelectorAll("link")) {
      if (link.getAttribute("href") == this.constructor.stylesheetUrl) {
        return;
      }
    }

    let style = document.createElement("link");
    style.rel = "stylesheet";
    style.href = this.constructor.stylesheetUrl;
    container.appendChild(style);
    root.__mozLabelCssAdded = true;
  }

  set textContent(val) {
    super.textContent = val;
    this.#lastFormattedAccessKey = null;
    this.formatAccessKey();
  }

  get textContent() {
    return super.textContent;
  }

  attributeChangedCallback(attrName, oldValue, newValue) {
    if (oldValue == newValue) {
      return;
    }

    this.formatAccessKey();
  }

  _onClick() {
    let controlElement = this.labeledControlElement;
    if (!controlElement || this.disabled) {
      return;
    }
    controlElement.focus();

    if (
      (controlElement.localName == "checkbox" ||
        controlElement.localName == "radio") &&
      controlElement.hasAttribute("disabled")
    ) {
      return;
    }

    if (controlElement.localName == "checkbox") {
      controlElement.checked = !controlElement.checked;
    } else if (controlElement.localName == "radio") {
      controlElement.control.selectedItem = controlElement;
    }
  }

  set accessKey(val) {
    this.setAttribute("accesskey", val);
    let control = this.labeledControlElement;
    if (control) {
      control.setAttribute("accesskey", val);
    }
  }

  get accessKey() {
    let accessKey = this.getAttribute("accesskey");
    return accessKey ? accessKey[0] : null;
  }

  get labeledControlElement() {
    let control = this.control;
    return control ? document.getElementById(control) : null;
  }

  set control(val) {
    this.setAttribute("control", val);
  }

  get control() {
    return this.getAttribute("control");
  }

  formatAccessKey() {
    let accessKey = this.accessKey || this.getAttribute("shownaccesskey");
    if (
      !MozTextLabel.#underlineAccesskey ||
      this.#lastFormattedAccessKey == accessKey ||
      !this.textContent ||
      !this.textContent.trim()
    ) {
      return;
    }
    this.#stopMutationObserver();
    try {
      this.#formatAccessKey(accessKey);
    } finally {
      queueMicrotask(() => this.#startMutationObserver());
    }
  }

  #formatAccessKey(accessKey) {
    this.#lastFormattedAccessKey = accessKey;
    if (this.accessKeySpan) {
      mergeElement(this.accessKeySpan);
      this.accessKeySpan = null;
    }

    if (this.hiddenColon) {
      mergeElement(this.hiddenColon);
      this.hiddenColon = null;
    }

    if (this.accessKeyParens) {
      this.accessKeyParens.remove();
      this.accessKeyParens = null;
    }

    if (!accessKey) {
      return;
    }

    let labelText = this.textContent;
    let accessKeyIndex = -1;
    if (!this.#alwaysAppendAccessKey) {
      accessKeyIndex = labelText.indexOf(accessKey);
      if (accessKeyIndex < 0) {
        accessKeyIndex = labelText
          .toUpperCase()
          .indexOf(accessKey.toUpperCase());
      }
    } else if (labelText.endsWith(`(${accessKey.toUpperCase()})`)) {
      accessKeyIndex = labelText.length - (1 + accessKey.length); 
    }

    const HTML_NS = "http://www.w3.org/1999/xhtml";
    this.accessKeySpan = document.createElementNS(HTML_NS, "span");
    this.accessKeySpan.className = "accesskey";


    if (accessKeyIndex >= 0) {
      wrapChar(this, this.accessKeySpan, accessKeyIndex);
      return;
    }

    let colonHidden = false;
    if (/:$/.test(labelText)) {
      labelText = labelText.slice(0, -1);
      this.hiddenColon = document.createElementNS(HTML_NS, "span");
      this.hiddenColon.className = "hiddenColon";
      this.hiddenColon.style.display = "none";
      wrapChar(this, this.hiddenColon, labelText.length);
      colonHidden = true;
    }
    let endIsSpace = false;
    if (/ $/.test(labelText)) {
      endIsSpace = true;
    }

    this.accessKeyParens = document.createElementNS(
      "http://www.w3.org/1999/xhtml",
      "span"
    );
    this.appendChild(this.accessKeyParens);
    if (this.#insertSeparator && !endIsSpace) {
      this.accessKeyParens.textContent = " (";
    } else {
      this.accessKeyParens.textContent = "(";
    }
    this.accessKeySpan.textContent = accessKey.toUpperCase();
    this.accessKeyParens.appendChild(this.accessKeySpan);
    if (!colonHidden) {
      this.accessKeyParens.appendChild(document.createTextNode(")"));
    } else {
      this.accessKeyParens.appendChild(document.createTextNode("):"));
    }
  }
}
customElements.define("moz-label", MozTextLabel, { extends: "label" });

function mergeElement(element) {
  if (!element.isConnected) {
    return;
  }
  if (
    Text.hasOwnProperty("isInstance")
      ? Text.isInstance(element.previousSibling)
      : // eslint-disable-next-line mozilla/use-isInstance
        element.previousSibling instanceof Text
  ) {
    element.previousSibling.appendData(element.textContent);
  } else {
    element.parentNode.insertBefore(element.firstChild, element);
  }
  element.remove();
}

function wrapChar(parentNode, element, index) {
  let treeWalker = document.createNodeIterator(
    parentNode,
    NodeFilter.SHOW_TEXT,
    null
  );
  let node = treeWalker.nextNode();
  while (index >= node.length) {
    index -= node.length;
    node = treeWalker.nextNode();
  }
  if (index) {
    node = node.splitText(index);
  }

  node.parentNode.insertBefore(element, node);
  if (node.length > 1) {
    node.splitText(1);
  }
  element.appendChild(node);
}
