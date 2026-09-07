/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};
ChromeUtils.defineESModuleGetters(lazy, {
  CustomizableUI:
    "moz-src:///browser/components/customizableui/CustomizableUI.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "gBundle", function () {
  return Services.strings.createBundle(
    "chrome://browser/locale/browser.properties"
  );
});

var DocumentWalker = class {

  #onlyWantElements = true;
  #showAnonymousContent = false;
  #walker = Cc["@mozilla.org/inspector/deep-tree-walker;1"].createInstance(
    Ci.inIDeepTreeWalker
  );

  constructor(
    node,
    { filter, onlyWantElements = true, showAnonymousContent = true } = {}
  ) {
    if (
      Cu.isDeadWrapper(node.documentGlobal) ||
      !node.documentGlobal.location
    ) {
      throw new Error("Got an invalid root window in DocumentWalker");
    }

    this.#showAnonymousContent = showAnonymousContent;
    this.#walker.showAnonymousContent = showAnonymousContent;
    this.#walker.showSubDocuments = true;
    this.#walker.showDocumentsAsNodes = true;
    this.#walker.init(node);

    this.filter = filter;
    this.#onlyWantElements = onlyWantElements;
  }

  get currentNode() {
    return this.#walker.currentNode;
  }

  set currentNode(val) {
    this.#walker.currentNode = val ?? this.#walker.root;
  }

  parentNode() {
    return this.#walker.parentNode();
  }

  get root() {
    return this.#walker.root;
  }

  previousNode() {
    return this.#previousGoodNodeInRoot();
  }

  nextNode() {
    return this.#nextGoodNodeInRoot();
  }

  #getLastPreOrderDepthFirstNodeIn(node) {
    let last = node;
    while (last?.lastChild) {
      last = last.lastChild;
    }
    return last;
  }

  previousSibling() {
    let node = this.#walker.previousSibling();
    while (node && this.isSkippedNode(node)) {
      node = this.#walker.previousSibling();
    }
    return node;
  }

  #previousGoodNodeInRoot() {
    let { currentNode } = this.#walker;
    if (!currentNode) {
      return null;
    }

    let previousNode;
    do {
      previousNode = this.#walker.previousNode();

      const host = previousNode?.getRootNode()?.host;
      if (host && !this.isSkippedNode(host)) {
        this.#walker.currentNode = host;
        return host;
      }
    } while (previousNode && this.isSkippedNode(previousNode));

    return previousNode;
  }

  #nextGoodNodeInRoot() {
    let { currentNode } = this.#walker;
    if (!currentNode) {
      return null;
    }

    if (
      this.#showAnonymousContent &&
      currentNode.shadowRoot &&
      !this.isSkippedNode(currentNode)
    ) {
      this.#walker.showAnonymousContent = false;
    }

    let nextNode = this.#walker.nextNode();

    this.#walker.showAnonymousContent = this.#showAnonymousContent;

    while (nextNode && this.isSkippedNode(nextNode)) {
      nextNode = this.#walker.nextNode();
    }

    return nextNode;
  }

  firstChild() {
    this.#walker.currentNode = this.#walker.root;

    let node = this.#walker.currentNode;
    if (!node || !this.isSkippedNode(node)) {
      return node;
    }

    return this.#nextGoodNodeInRoot();
  }

  lastChild() {
    this.#walker.currentNode = this.#getLastPreOrderDepthFirstNodeIn(
      this.#walker.root
    );

    let node = this.#walker.currentNode;
    if (!node || !this.isSkippedNode(node)) {
      return node;
    }

    return this.#previousGoodNodeInRoot();
  }

  isSkippedNode(node) {
    if (this.#onlyWantElements && node.nodeType != Node.ELEMENT_NODE) {
      return true;
    }
    return this.filter(node) !== NodeFilter.FILTER_ACCEPT;
  }
};

const BLOCKERS_TIMEOUT_MS = 10000;

const TRANSITION_PHASES = Object.freeze({
  START: 1,
  PREPARE: 2,
  TRANSITION: 3,
});

let gNodeToObjectMap = new WeakMap();
let gWindowsWithUnloadHandler = new WeakSet();

var AssociatedToNode = class {
  constructor(node) {
    this.node = node;

    this._blockersPromise = Promise.resolve();
  }

  static forNode(node) {
    let associatedToNode = gNodeToObjectMap.get(node);
    if (!associatedToNode) {
      associatedToNode = new this(node);
      gNodeToObjectMap.set(node, associatedToNode);
    }
    return associatedToNode;
  }

  get document() {
    return this.node.ownerDocument;
  }

  get window() {
    return this.node.documentGlobal;
  }

  _getBoundsWithoutFlushing(element) {
    return this.window.windowUtils.getBoundsWithoutFlushing(element);
  }

  dispatchCustomEvent(eventName, detail, cancelable = false) {
    let event = new this.window.CustomEvent(eventName, {
      detail,
      bubbles: true,
      cancelable,
    });
    this.node.dispatchEvent(event);
    return event.defaultPrevented;
  }

  async dispatchAsyncEvent(eventName) {
    let blockersPromise = this._blockersPromise.catch(() => {});
    return (this._blockersPromise = blockersPromise.then(async () => {
      let blockers = new Set();
      let cancel = this.dispatchCustomEvent(
        eventName,
        {
          addBlocker(promise) {
            blockers.add(
              promise.catch(ex => {
                console.error(ex);
                return true;
              })
            );
          },
        },
        true
      );
      if (blockers.size) {
        let timeoutPromise = new Promise((resolve, reject) => {
          this.window.setTimeout(reject, BLOCKERS_TIMEOUT_MS);
        });
        try {
          let results = await Promise.race([
            Promise.all(blockers),
            timeoutPromise,
          ]);
          cancel = cancel || results.some(result => result === false);
        } catch (ex) {
          console.error(
            new Error(`One of the blockers for ${eventName} timed out.`)
          );
          return true;
        }
      }
      return cancel;
    }));
  }
};

export var PanelMultiView = class extends AssociatedToNode {
  static async openPopup(panelNode, ...args) {
    let panelMultiViewNode = panelNode.querySelector("panelmultiview");
    if (panelMultiViewNode) {
      return this.forNode(panelMultiViewNode).openPopup(...args);
    }
    panelNode.openPopup(...args);
    return true;
  }

  static hidePopup(panelNode, animate = false) {
    let panelMultiViewNode = panelNode.querySelector("panelmultiview");
    if (panelMultiViewNode) {
      this.forNode(panelMultiViewNode).hidePopup(animate);
    } else {
      panelNode.hidePopup(animate);
    }
  }

  static removePopup(panelNode) {
    try {
      let panelMultiViewNode = panelNode.querySelector("panelmultiview");
      if (panelMultiViewNode) {
        let panelMultiView = this.forNode(panelMultiViewNode);
        panelMultiView._moveOutKids();
        panelMultiView.disconnect();
      }
    } finally {
      panelNode.remove();
    }
  }

  static getViewNode(doc, id) {
    let viewCacheTemplate = doc.getElementById("appMenu-viewCache");

    return (
      doc.getElementById(id) ||
      viewCacheTemplate?.content.querySelector("#" + id)
    );
  }

  static ensureUnloadHandlerRegistered(window) {
    if (gWindowsWithUnloadHandler.has(window)) {
      return;
    }

    window.addEventListener(
      "unload",
      () => {
        for (let panelMultiViewNode of window.document.querySelectorAll(
          "panelmultiview"
        )) {
          this.forNode(panelMultiViewNode).disconnect();
        }
      },
      { once: true }
    );

    gWindowsWithUnloadHandler.add(window);
  }

  get #panel() {
    return this.node.parentNode;
  }

  set #transitioning(val) {
    if (val) {
      this.node.setAttribute("transitioning", "true");
    } else {
      this.node.removeAttribute("transitioning");
    }
  }

  constructor(node) {
    super(node);
    this._openPopupPromise = Promise.resolve(false);
  }

  connect() {
    this.connected = true;

    PanelMultiView.ensureUnloadHandlerRegistered(this.window);

    let viewContainer = (this._viewContainer =
      this.document.createXULElement("box"));
    viewContainer.classList.add("panel-viewcontainer");

    let viewStack = (this._viewStack = this.document.createXULElement("box"));
    viewStack.classList.add("panel-viewstack");
    viewContainer.append(viewStack);

    let offscreenViewContainer = this.document.createXULElement("box");
    offscreenViewContainer.classList.add("panel-viewcontainer", "offscreen");

    let offscreenViewStack = (this._offscreenViewStack =
      this.document.createXULElement("box"));
    offscreenViewStack.classList.add("panel-viewstack");
    offscreenViewContainer.append(offscreenViewStack);

    this.node.prepend(offscreenViewContainer);
    this.node.prepend(viewContainer);

    this.openViews = [];

    this.#panel.addEventListener("popupshowing", this);
    this.#panel.addEventListener("popuphidden", this);
    this.#panel.addEventListener("popupshown", this);

    ["goBack", "showSubView"].forEach(method => {
      Object.defineProperty(this.node, method, {
        enumerable: true,
        value: (...args) => this[method](...args),
      });
    });
  }

  disconnect() {
    if (!this.node || !this.connected) {
      return;
    }

    this.#panel.removeEventListener("mousemove", this);
    this.#panel.removeEventListener("popupshowing", this);
    this.#panel.removeEventListener("popupshown", this);
    this.#panel.removeEventListener("popuphidden", this);
    this.document.documentElement.removeEventListener("keydown", this, true);
    this.node =
      this._openPopupPromise =
      this._openPopupCancelCallback =
      this._viewContainer =
      this._viewStack =
      this._transitionDetails =
        null;
  }

  async openPopup(anchor, options, ...args) {
    let canCancel = true;
    let cancelCallback = (this._openPopupCancelCallback = () => {
      if (canCancel && this.node) {
        canCancel = false;
        this.dispatchCustomEvent("popuphidden");
      }
      if (cancelCallback == this._openPopupCancelCallback) {
        delete this._openPopupCancelCallback;
      }
    });

    let openPopupPromise = this._openPopupPromise.catch(() => {
      return false;
    });

    return (this._openPopupPromise = openPopupPromise.then(async wasShown => {
      if (!this.node) {
        return false;
      }
      if (wasShown && ["open", "showing"].includes(this.#panel.state)) {
        if (cancelCallback == this._openPopupCancelCallback) {
          delete this._openPopupCancelCallback;
        }
        return true;
      }
      try {
        if (!this.connected) {
          this.connect();
        }
        if (!(await this.#showMainView())) {
          cancelCallback();
        }
      } catch (ex) {
        cancelCallback();
        throw ex;
      }
      if (!canCancel || !this.node) {
        return false;
      }
      try {
        canCancel = false;
        this.#panel.openPopup(anchor, options, ...args);
        if (cancelCallback == this._openPopupCancelCallback) {
          delete this._openPopupCancelCallback;
        }
        this.#panel.setAttribute("mainviewshowing", true);

        if (this.#panel.state == "closed" && this.openViews.length) {
          this.dispatchCustomEvent("popuphidden");
          return false;
        }

        if (
          options &&
          typeof options == "object" &&
          options.triggerEvent &&
          (options.triggerEvent.type == "keypress" ||
            options.triggerEvent.type == "keydown" ||
            options.triggerEvent?.inputSource ==
              MouseEvent.MOZ_SOURCE_KEYBOARD) &&
          this.openViews.length
        ) {
          this.openViews[0].focusWhenActive = true;
        }

        return true;
      } catch (ex) {
        this.dispatchCustomEvent("popuphidden");
        throw ex;
      }
    }));
  }

  hidePopup(animate = false) {
    if (!this.node || !this.connected) {
      return;
    }

    if (["open", "showing"].includes(this.#panel.state)) {
      this.#panel.hidePopup(animate);
    } else {
      this._openPopupCancelCallback?.();
    }

    this.closeAllViews();
  }

  _moveOutKids() {
    let viewCacheId = this.node?.getAttribute("viewCacheId");
    if (!viewCacheId) {
      return;
    }

    let subviews = Array.from(this._viewStack.children);
    let viewCache = this.document.getElementById("appMenu-viewCache");
    for (let subview of subviews) {
      viewCache.moveBefore(subview, null);
    }
  }

  showSubView(viewIdOrNode, anchor) {
    this.#showSubView(viewIdOrNode, anchor).catch(console.error);
  }

  async #showSubView(viewIdOrNode, anchor) {
    let viewNode =
      typeof viewIdOrNode == "string"
        ? PanelMultiView.getViewNode(this.document, viewIdOrNode)
        : viewIdOrNode;
    if (!viewNode) {
      console.error(new Error(`Subview ${viewIdOrNode} doesn't exist.`));
      return;
    }

    if (!this.openViews.length) {
      console.error(new Error(`Cannot show a subview in a closed panel.`));
      return;
    }

    let prevPanelView = this.openViews[this.openViews.length - 1];
    let nextPanelView = PanelView.forNode(viewNode);
    if (this.openViews.includes(nextPanelView)) {
      console.error(new Error(`Subview ${viewNode.id} is already open.`));
      return;
    }

    if (!prevPanelView.active) {
      return;
    }
    let doingKeyboardActivation = prevPanelView._doingKeyboardActivation;
    prevPanelView.active = false;

    anchor?.setAttribute("open", "true");
    try {
      if (!(await this.#openView(nextPanelView))) {
        if (prevPanelView.isOpenIn(this)) {
          prevPanelView.active = true;
        }
        return;
      }

      prevPanelView.captureKnownSize();

      nextPanelView.mainview = false;
      let title;
      const l10nId = viewNode.getAttribute("data-l10n-id");
      if (l10nId) {
        const l10nArgs = viewNode.getAttribute("data-l10n-args");
        const args = l10nArgs ? JSON.parse(l10nArgs) : undefined;
        const [msg] = await viewNode.ownerDocument.l10n.formatMessages([
          { id: l10nId, args },
        ]);
        title = msg.attributes.find(a => a.name === "title")?.value;
      }
      title ??= viewNode.getAttribute("title") || anchor?.getAttribute("label");
      nextPanelView.headerText = title;
      nextPanelView.minMaxWidth = prevPanelView.knownWidth;
      let lockPanelVertical =
        this.openViews[0].node.getAttribute("lockpanelvertical") == "true";
      nextPanelView.minMaxHeight = lockPanelVertical
        ? prevPanelView.knownHeight
        : 0;

      if (anchor) {
        viewNode.classList.add("PanelUI-subView");
      }

      await this.#transitionViews(prevPanelView.node, viewNode, false);
    } finally {
      anchor?.removeAttribute("open");
    }

    nextPanelView.focusWhenActive = doingKeyboardActivation;
    this.#activateView(nextPanelView);
  }

  goBack() {
    this.#goBack().catch(console.error);
  }

  async #goBack() {
    if (this.openViews.length < 2) {
      return;
    }

    let prevPanelView = this.openViews[this.openViews.length - 1];
    let nextPanelView = this.openViews[this.openViews.length - 2];

    if (!prevPanelView.active) {
      return;
    }
    prevPanelView.active = false;

    prevPanelView.captureKnownSize();
    await this.#transitionViews(prevPanelView.node, nextPanelView.node, true);

    this.#closeLatestView();

    this.#activateView(nextPanelView);
  }

  async #showMainView() {
    let nextPanelView = PanelView.forNode(
      PanelMultiView.getViewNode(
        this.document,
        this.node.getAttribute("mainViewId")
      )
    );

    let oldPanelMultiViewNode = nextPanelView.node.panelMultiView;
    if (oldPanelMultiViewNode) {
      PanelMultiView.forNode(oldPanelMultiViewNode).hidePopup();
      await this.window.promiseDocumentFlushed(() => {});
    }

    if (!(await this.#openView(nextPanelView))) {
      return false;
    }

    nextPanelView.mainview = true;
    nextPanelView.headerText = "";
    nextPanelView.minMaxWidth = 0;
    nextPanelView.minMaxHeight = 0;

    nextPanelView.visible = true;

    return true;
  }

  async #openView(panelView) {
    if (panelView.node.parentNode != this._viewStack) {
      this._viewStack.appendChild(panelView.node);
    }

    panelView.node.panelMultiView = this.node;
    this.openViews.push(panelView);

    this.#panel.toggleAttribute(
      "remote",
      panelView.node.hasAttribute("remote")
    );

    let canceled = await panelView.dispatchAsyncEvent("ViewShowing");

    if (!this.openViews.length) {
      return false;
    }

    if (canceled) {
      this.#closeLatestView();
      return false;
    }

    let { style } = panelView.node;
    style.removeProperty("outline");
    style.removeProperty("width");

    return true;
  }

  #activateView(panelView) {
    if (panelView.isOpenIn(this)) {
      panelView.active = true;
      if (panelView.focusWhenActive) {
        panelView.focusFirstNavigableElement(false, true);
        panelView.focusWhenActive = false;
      }
      panelView.dispatchCustomEvent("ViewShown");
    }
  }

  #closeLatestView() {
    let panelView = this.openViews.pop();
    panelView.clearNavigation();
    panelView.dispatchCustomEvent("ViewHiding");
    panelView.node.panelMultiView = null;
    panelView.visible = false;
  }

  closeAllViews() {
    while (this.openViews.length) {
      this.#closeLatestView();
    }
  }

  async #transitionViews(previousViewNode, viewNode, reverse) {
    const { window } = this;

    let nextPanelView = PanelView.forNode(viewNode);
    let prevPanelView = PanelView.forNode(previousViewNode);

    let details = (this._transitionDetails = {
      phase: TRANSITION_PHASES.START,
    });

    let olderView = reverse ? nextPanelView : prevPanelView;
    this._viewContainer.style.minHeight = olderView.knownHeight + "px";
    this._viewContainer.style.height = prevPanelView.knownHeight + "px";
    this._viewContainer.style.width = prevPanelView.knownWidth + "px";
    let rect = this._getBoundsWithoutFlushing(this.#panel);
    this.#panel.style.width = rect.width + "px";
    this.#panel.style.height = rect.height + "px";

    let viewRect;
    if (reverse) {
      viewRect = {
        width: nextPanelView.knownWidth,
        height: nextPanelView.knownHeight,
      };
      nextPanelView.visible = true;
    } else if (viewNode.customRectGetter) {
      let width = prevPanelView.knownWidth;
      let height = prevPanelView.knownHeight;
      viewRect = Object.assign({ height, width }, viewNode.customRectGetter());
      nextPanelView.visible = true;
      let header = viewNode.firstElementChild;
      if (header && header.classList.contains("panel-header")) {
        viewRect.height += await window.promiseDocumentFlushed(() => {
          return this._getBoundsWithoutFlushing(header).height;
        });
      }
      if (!nextPanelView.isOpenIn(this)) {
        return;
      }
    } else {
      this._offscreenViewStack.style.minHeight = olderView.knownHeight + "px";
      this._offscreenViewStack.appendChild(viewNode);
      nextPanelView.visible = true;

      viewRect = await window.promiseDocumentFlushed(() => {
        return this._getBoundsWithoutFlushing(viewNode);
      });
      if (!nextPanelView.isOpenIn(this)) {
        return;
      }

      this._viewStack.appendChild(viewNode);

      this._offscreenViewStack.style.removeProperty("min-height");
    }

    this.#transitioning = true;
    details.phase = TRANSITION_PHASES.PREPARE;

    let moveToLeft =
      (this.window.RTL_UI && !reverse) || (!this.window.RTL_UI && reverse);
    let deltaX = prevPanelView.knownWidth;
    let deepestNode = reverse ? previousViewNode : viewNode;

    if (reverse) {
      this._viewStack.style.marginInlineStart = "-" + deltaX + "px";
    }

    this._viewStack.style.transition =
      "transform var(--animation-easing-function)" +
      " var(--panelui-subview-transition-duration)";
    this._viewStack.style.willChange = "transform";
    deepestNode.style.outline = "1px solid var(--panel-separator-color)";

    await window.promiseDocumentFlushed(() => {});
    if (!nextPanelView.isOpenIn(this)) {
      return;
    }

    this._viewContainer.style.height = viewRect.height + "px";
    this._viewContainer.style.width = viewRect.width + "px";
    this.#panel.style.removeProperty("width");
    this.#panel.style.removeProperty("height");
    viewNode.style.width = viewRect.width + "px";

    details.phase = TRANSITION_PHASES.TRANSITION;

    if (viewNode.getAttribute("mainview")) {
      this._viewContainer.style.removeProperty("min-height");
      this.#panel.setAttribute("mainviewshowing", true);
    } else {
      this.#panel.removeAttribute("mainviewshowing");
    }

    if (
      this.window.matchMedia("(prefers-reduced-motion: no-preference)")
        .matches &&
      !viewNode.getAttribute("no-panelview-transition")
    ) {
      this._viewStack.style.transform =
        "translateX(" + (moveToLeft ? "" : "-") + deltaX + "px)";

      await new Promise(resolve => {
        details.resolve = resolve;
        this._viewContainer.addEventListener(
          "transitionend",
          (details.listener = ev => {
            if (
              ev.target != this._viewStack ||
              ev.propertyName != "transform"
            ) {
              return;
            }
            this._viewContainer.removeEventListener(
              "transitionend",
              details.listener
            );
            delete details.listener;
            resolve();
          })
        );
        this._viewContainer.addEventListener(
          "transitioncancel",
          (details.cancelListener = ev => {
            if (ev.target != this._viewStack) {
              return;
            }
            this._viewContainer.removeEventListener(
              "transitioncancel",
              details.cancelListener
            );
            delete details.cancelListener;
            resolve();
          })
        );
      });
    }

    if (!nextPanelView.isOpenIn(this)) {
      return;
    }
    prevPanelView.visible = false;

    nextPanelView.node.style.removeProperty("width");
    deepestNode.style.removeProperty("outline");
    this.#cleanupTransitionPhase();
    await this.window.promiseDocumentFlushed(() => {});
    nextPanelView.focusSelectedElement();
  }

  #cleanupTransitionPhase() {
    if (!this._transitionDetails) {
      return;
    }

    let { phase, resolve, listener, cancelListener } = this._transitionDetails;
    this._transitionDetails = null;

    if (phase >= TRANSITION_PHASES.START) {
      this.#panel.removeAttribute("width");
      this.#panel.removeAttribute("height");
      this._viewContainer.style.removeProperty("height");
      this._viewContainer.style.removeProperty("width");
    }
    if (phase >= TRANSITION_PHASES.PREPARE) {
      this.#transitioning = false;
      this._viewStack.style.removeProperty("margin-inline-start");
      this._viewStack.style.removeProperty("transition");
    }
    if (phase >= TRANSITION_PHASES.TRANSITION) {
      this._viewStack.style.removeProperty("transform");
      if (listener) {
        this._viewContainer.removeEventListener("transitionend", listener);
      }
      if (cancelListener) {
        this._viewContainer.removeEventListener(
          "transitioncancel",
          cancelListener
        );
      }
      if (resolve) {
        resolve();
      }
    }
  }

  handleEvent(aEvent) {
    if (
      aEvent.type.startsWith("popup") &&
      aEvent.target != this.#panel &&
      aEvent.target != this.node
    ) {
      return;
    }
    switch (aEvent.type) {
      case "keydown": {
        let currentView = this.openViews[this.openViews.length - 1];
        currentView.keyNavigation(aEvent);
        break;
      }
      case "mousemove": {
        this.openViews.forEach(panelView => {
          if (!panelView.ignoreMouseMove) {
            panelView.clearNavigation();
          }
        });
        break;
      }
      case "popupshowing": {
        this._viewContainer.setAttribute("panelopen", "true");
        if (!this.node.hasAttribute("disablekeynav")) {
          this.document.documentElement.addEventListener("keydown", this, true);
          this.#panel.addEventListener("mousemove", this);
        }
        break;
      }
      case "popupshown": {
        let mainPanelView = this.openViews[0];
        this.#activateView(mainPanelView);
        break;
      }
      case "popuphidden": {
        this.#transitioning = false;
        this._viewContainer.removeAttribute("panelopen");
        this.#cleanupTransitionPhase();
        this.document.documentElement.removeEventListener(
          "keydown",
          this,
          true
        );
        this.#panel.removeEventListener("mousemove", this);
        this.closeAllViews();

        this._viewContainer.style.removeProperty("min-height");
        this._viewStack.style.removeProperty("max-height");
        this._viewContainer.style.removeProperty("width");
        this._viewContainer.style.removeProperty("height");

        this.dispatchCustomEvent("PanelMultiViewHidden");
        break;
      }
    }
  }
};

export var PanelView = class extends AssociatedToNode {
  constructor(node) {
    super(node);

    this.active = false;

    this.focusWhenActive = false;

    this.window.addEventListener(
      "unload",
      () => {
        this.#_tabNavigableWalker = null;
        this.#_arrowNavigableWalker = null;
      },
      { once: true }
    );
  }

  isOpenIn(panelMultiView) {
    return this.node.panelMultiView == panelMultiView.node;
  }

  set mainview(value) {
    if (value) {
      this.node.setAttribute("mainview", true);
    } else {
      this.node.removeAttribute("mainview");
    }
  }

  set visible(value) {
    if (value) {
      this.node.setAttribute("visible", true);
    } else {
      this.node.removeAttribute("visible");
      this.active = false;
      this.focusWhenActive = false;
    }
  }

  set minMaxWidth(value) {
    let style = this.node.style;
    if (value) {
      style.minWidth = style.maxWidth = value + "px";
    } else {
      style.removeProperty("min-width");
      style.removeProperty("max-width");
    }
  }

  set minMaxHeight(value) {
    let style = this.node.style;
    if (value) {
      style.minHeight = style.maxHeight = value + "px";
    } else {
      style.removeProperty("min-height");
      style.removeProperty("max-height");
    }
  }

  set headerText(value) {
    let ensureHeaderSeparator = headerNode => {
      if (headerNode.nextSibling.tagName != "toolbarseparator") {
        let separator = this.document.createXULElement("toolbarseparator");
        this.node.insertBefore(separator, headerNode.nextSibling);
      }
    };

    let isMainView = this.node.getAttribute("mainview");
    let header = this.node.querySelector(".panel-header");
    if (header) {
      let headerBackButton = header.querySelector(".subviewbutton-back");
      if (isMainView) {
        if (headerBackButton) {
          headerBackButton.remove();
        }
      }
      if (value) {
        if (
          !isMainView &&
          !headerBackButton &&
          !this.node.getAttribute("no-back-button")
        ) {
          header.prepend(this.createHeaderBackButton());
        }
        header.querySelector(".panel-header > h1 > span").textContent = value;
        ensureHeaderSeparator(header);
      } else if (
        !this.node.getAttribute("has-custom-header") &&
        !this.node.getAttribute("mainview-with-header")
      ) {
        if (header.nextSibling.tagName == "toolbarseparator") {
          header.nextSibling.remove();
        }
        header.remove();
        return;
      }
      return;
    }

    if (!value) {
      return;
    }

    header = this.document.createXULElement("box");
    header.classList.add("panel-header");

    if (!isMainView) {
      let backButton = this.createHeaderBackButton();
      header.append(backButton);
    }

    let h1 = this.document.createElement("h1");
    let span = this.document.createElement("span");
    span.textContent = value;
    h1.appendChild(span);

    header.append(h1);
    this.node.prepend(header);

    ensureHeaderSeparator(header);
  }

  createHeaderBackButton() {
    let backButton = this.document.createXULElement("toolbarbutton");
    backButton.className =
      "subviewbutton subviewbutton-iconic subviewbutton-back";
    backButton.setAttribute("closemenu", "none");
    backButton.setAttribute("tabindex", "0");
    backButton.setAttribute(
      "aria-label",
      lazy.gBundle.GetStringFromName("panel.back")
    );
    backButton.addEventListener("command", () => {
      this.node.panelMultiView.goBack();
      backButton.blur();
    });
    return backButton;
  }

  dispatchCustomEvent(...args) {
    lazy.CustomizableUI.ensureSubviewListeners(this.node);
    return super.dispatchCustomEvent(...args);
  }

  captureKnownSize() {
    let rect = this._getBoundsWithoutFlushing(this.node);
    this.knownWidth = rect.width;
    this.knownHeight = rect.height;
  }

  #isNavigableWithTabOnly(element) {
    let tag = element.localName;
    return (
      tag == "menulist" ||
      tag == "select" ||
      tag == "radiogroup" ||
      tag == "input" ||
      tag == "textarea" ||
      tag == "browser" ||
      tag == "iframe" ||
      element.dataset?.navigableWithTabOnly === "true"
    );
  }

  #makeNavigableTreeWalker(arrowKey) {
    let filter = node => {
      if (node.disabled) {
        return NodeFilter.FILTER_REJECT;
      }
      let visible = node.checkVisibility({
        checkVisibilityCSS: true,
        flush: false,
      });
      if (!visible) {
        return NodeFilter.FILTER_REJECT;
      }
      let bounds = this._getBoundsWithoutFlushing(node);
      if (bounds.width == 0 || bounds.height == 0) {
        return NodeFilter.FILTER_REJECT;
      }
      let isNavigableWithTabOnly = this.#isNavigableWithTabOnly(node);
      if (arrowKey && isNavigableWithTabOnly) {
        return NodeFilter.FILTER_REJECT;
      }
      let localName = node.localName.toLowerCase();
      if (
        localName == "button" ||
        localName == "toolbarbutton" ||
        localName == "checkbox" ||
        localName == "a" ||
        localName == "moz-button" ||
        localName == "moz-box-button" ||
        localName == "moz-toggle" ||
        localName == "summary" ||
        node.classList.contains("text-link") ||
        (!arrowKey && isNavigableWithTabOnly) ||
        node.dataset?.capturesFocus === "true"
      ) {
        if (
          localName != "browser" &&
          localName != "iframe" &&
          localName != "input" &&
          !node.hasAttribute("tabindex") &&
          node.dataset?.capturesFocus !== "true"
        ) {
          node.setAttribute("tabindex", "-1");
        }
        return NodeFilter.FILTER_ACCEPT;
      }
      return NodeFilter.FILTER_SKIP;
    };
    return new DocumentWalker(this.node, {
      filter,
    });
  }

  #_tabNavigableWalker = null;
  get _tabNavigableWalker() {
    if (!this.#_tabNavigableWalker) {
      this.#_tabNavigableWalker = this.#makeNavigableTreeWalker(false);
    }
    return this.#_tabNavigableWalker;
  }

  #_arrowNavigableWalker = null;
  get #arrowNavigableWalker() {
    if (!this.#_arrowNavigableWalker) {
      this.#_arrowNavigableWalker = this.#makeNavigableTreeWalker(true);
    }
    return this.#_arrowNavigableWalker;
  }

  get selectedElement() {
    return this._selectedElement && this._selectedElement.get();
  }

  set selectedElement(value) {
    if (!value) {
      delete this._selectedElement;
    } else {
      this._selectedElement = Cu.getWeakReference(value);
    }
  }

  focusFirstNavigableElement(homeKey = false, skipBack = false) {
    let walker = homeKey
      ? this.#arrowNavigableWalker
      : this._tabNavigableWalker;
    walker.currentNode = walker.root;
    this.selectedElement = walker.firstChild();
    if (
      skipBack &&
      walker.currentNode &&
      walker.currentNode.classList.contains("subviewbutton-back") &&
      walker.nextNode()
    ) {
      this.selectedElement = walker.currentNode;
    }
    this.focusSelectedElement( true);
  }

  focusLastNavigableElement(endKey = false) {
    let walker = endKey ? this.#arrowNavigableWalker : this._tabNavigableWalker;
    walker.currentNode = walker.root;
    this.selectedElement = walker.lastChild();
    this.focusSelectedElement( true);
  }

  moveSelection(isDown, arrowKey = false) {
    let walker = arrowKey
      ? this.#arrowNavigableWalker
      : this._tabNavigableWalker;
    let oldSel = this.selectedElement;

    if (!oldSel) {
      oldSel = this.document.activeElement;
      if (
        oldSel &&
        !(
          this.node.compareDocumentPosition(oldSel) &
          Node.DOCUMENT_POSITION_CONTAINED_BY
        )
      ) {
        oldSel = null;
      }
      while (oldSel?.shadowRoot?.activeElement) {
        oldSel = oldSel.shadowRoot.activeElement;
      }
    }

    let newSel;
    if (oldSel) {
      walker.currentNode = oldSel;
      newSel = isDown ? walker.nextNode() : walker.previousNode();
    }
    if (!newSel) {
      walker.currentNode = walker.root;
      newSel = isDown ? walker.firstChild() : walker.lastChild();
    }
    this.selectedElement = newSel;
    return newSel;
  }

  keyNavigation(event) {
    if (!this.active) {
      return;
    }

    let focus = this.document.activeElement;
    if (
      focus &&
      !(
        this.node.compareDocumentPosition(focus) &
        Node.DOCUMENT_POSITION_CONTAINED_BY
      )
    ) {
      focus = null;
    }

    if (
      focus &&
      (focus.tagName == "browser" ||
        focus.tagName == "iframe" ||
        focus.dataset?.capturesFocus === "true")
    ) {
      return;
    }

    if (focus?.shadowRoot?.activeElement) {
      focus = focus.shadowRoot.activeElement;
    }

    let stop = () => {
      event.stopPropagation();
      event.preventDefault();
    };

    let tabOnly = () => {
      return focus && this.#isNavigableWithTabOnly(focus);
    };

    let isContextMenuOpen = () => {
      if (!focus) {
        return false;
      }
      let contextNode = focus.closest("[context]");
      if (!contextNode) {
        return false;
      }
      let context = contextNode.getAttribute("context");
      if (!context) {
        return false;
      }
      let popup = this.document.getElementById(context);
      return popup && popup.state == "open";
    };

    this.ignoreMouseMove = false;

    let keyCode = event.code;
    switch (keyCode) {
      case "ArrowDown":
      case "ArrowUp":
        if (tabOnly()) {
          break;
        }
      case "Tab": {
        if (
          isContextMenuOpen() ||
          (focus && focus.localName == "menulist" && focus.open)
        ) {
          break;
        }
        stop();
        let isDown =
          keyCode == "ArrowDown" || (keyCode == "Tab" && !event.shiftKey);
        let button = this.moveSelection(isDown, keyCode != "Tab");
        button.focus();
        break;
      }
      case "Home":
        if (tabOnly() || isContextMenuOpen()) {
          break;
        }
        stop();
        this.focusFirstNavigableElement(true);
        break;
      case "End":
        if (tabOnly() || isContextMenuOpen()) {
          break;
        }
        stop();
        this.focusLastNavigableElement(true);
        break;
      case "ArrowLeft":
      case "ArrowRight": {
        if (tabOnly() || isContextMenuOpen()) {
          break;
        }
        stop();
        if (
          (!this.window.RTL_UI && keyCode == "ArrowLeft") ||
          (this.window.RTL_UI && keyCode == "ArrowRight")
        ) {
          this.node.panelMultiView.goBack();
          break;
        }
        let button = this.selectedElement;
        if (
          !button ||
          !(
            button.classList.contains("subviewbutton-nav") ||
            button.classList.contains("moz-button-subviewbutton-nav")
          )
        ) {
          break;
        }
      }
      case "Space":
      case "NumpadEnter":
      case "Enter": {
        if (tabOnly() || isContextMenuOpen()) {
          break;
        }
        let button = this.selectedElement;
        if (!button || button?.localName == "moz-toggle") {
          break;
        }
        stop();

        this._doingKeyboardActivation = true;
        const details = {
          bubbles: true,
          ctrlKey: event.ctrlKey,
          altKey: event.altKey,
          shiftKey: event.shiftKey,
          metaKey: event.metaKey,
        };
        let target = button;
        if (
          button.localName == "moz-button" ||
          button.localName == "moz-box-button"
        ) {
          target = button.buttonEl;
          details.composed = true;
        }
        let dispEvent = new event.target.documentGlobal.MouseEvent(
          "mousedown",
          details
        );
        target.dispatchEvent(dispEvent);
        dispEvent = new event.target.documentGlobal.PointerEvent(
          "click",
          details
        );
        target.dispatchEvent(dispEvent);
        this._doingKeyboardActivation = false;
        break;
      }
    }
  }

  focusSelectedElement(byKey = false) {
    let selected = this.selectedElement;
    if (selected) {
      let flag = byKey ? Services.focus.FLAG_BYKEY : 0;
      Services.focus.setFocus(selected, flag);
    }
  }

  clearNavigation() {
    let selected = this.selectedElement;
    if (selected) {
      selected.blur();
      this.selectedElement = null;
    }
  }
};
