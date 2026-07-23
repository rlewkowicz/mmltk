/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  DeferredTask: "resource://gre/modules/DeferredTask.sys.mjs",
});

const kStateActive = 0x00000001; 
const kStateHover = 0x00000004; 

const SUPPORTED_OPTION_OPTGROUP_PROPERTIES = [
  "direction",
  "color",
  "background-color",
  "text-shadow",
  "text-transform",
  "font-family",
  "font-weight",
  "font-size",
  "font-style",
];

const SUPPORTED_SELECT_PROPERTIES = [
  ...SUPPORTED_OPTION_OPTGROUP_PROPERTIES,
  "scrollbar-width",
  "scrollbar-color",
];

var gOpen = false;

export var SelectContentHelper = function (aElement, aOptions, aActor) {
  this.element = aElement;
  this.initialSelection = aElement[aElement.selectedIndex] || null;
  this.actor = aActor;
  this.closedWithClickOn = false;
  this.isOpenedViaTouch = aOptions.isOpenedViaTouch;
  this._closeAfterBlur = true;
  this._pseudoStylesSetup = false;
  this.init();
  this.showDropDown();
  this._updateTimer = new lazy.DeferredTask(this._update.bind(this), 0);
};

Object.defineProperty(SelectContentHelper, "open", {
  get() {
    return gOpen;
  },
});

SelectContentHelper.prototype = {
  init() {
    let win = this.element.documentGlobal;
    win.addEventListener("pagehide", this, { mozSystemGroup: true });
    this.element.addEventListener("blur", this, { mozSystemGroup: true });
    this.element.addEventListener("transitionend", this, {
      mozSystemGroup: true,
    });
    this.mut = new win.MutationObserver(() => {
      this._updateTimer.arm();
    });
    this.mut.observe(this.element, {
      childList: true,
      subtree: true,
      attributes: true,
    });

    XPCOMUtils.defineLazyPreferenceGetter(
      this,
      "disablePopupAutohide",
      "ui.popup.disable_autohide",
      false
    );
  },

  uninit() {
    this.element.openInParentProcess = false;
    let win = this.element.documentGlobal;
    win.removeEventListener("pagehide", this, { mozSystemGroup: true });
    this.element.removeEventListener("blur", this, { mozSystemGroup: true });
    this.element.removeEventListener("transitionend", this, {
      mozSystemGroup: true,
    });
    this.element = null;
    this.actor = null;
    this.mut.disconnect();
    this._updateTimer.disarm();
    this._updateTimer = null;
    gOpen = false;
  },

  showDropDown() {
    this.element.openInParentProcess = true;
    this._setupPseudoClassStyles();
    let rect = this._getBoundingContentRect();
    let computedStyles = getComputedStyles(this.element);
    let options = this._buildOptionList();
    let defaultStyles = this.element.documentGlobal.getDefaultComputedStyle(
      this.element
    );
    this.actor.sendAsyncMessage("Forms:ShowDropDown", {
      isOpenedViaTouch: this.isOpenedViaTouch,
      options,
      rect,
      custom: this._allowCustomStyling(computedStyles),
      selectedIndex: this.element.selectedIndex,
      isDarkBackground: ChromeUtils.isDarkBackground(this.element),
      style: supportedStyles(computedStyles, SUPPORTED_SELECT_PROPERTIES),
      defaultStyle: supportedStyles(defaultStyles, SUPPORTED_SELECT_PROPERTIES),
    });
    this._clearPseudoClassStyles();
    gOpen = true;
  },

  _setupPseudoClassStyles() {
    if (this._pseudoStylesSetup) {
      throw new Error("pseudo styles must not be set up yet");
    }
    this._pseudoStylesSetup = true;
    InspectorUtils.addPseudoClassLock(this.element, ":focus");
  },

  _clearPseudoClassStyles() {
    if (!this._pseudoStylesSetup) {
      throw new Error("pseudo styles must be set up already");
    }
    InspectorUtils.clearPseudoClassLocks(this.element);
    this._pseudoStylesSetup = false;
  },

  _getBoundingContentRect() {
    let win = this.element.documentGlobal;
    return win.windowUtils.getElementBoundingScreenRect(this.element);
  },

  _buildOptionList() {
    if (!this._pseudoStylesSetup) {
      throw new Error("pseudo styles must be set up");
    }
    let uniqueStyles = [];
    let options = buildOptionListForChildren(this.element, uniqueStyles);
    return { options, uniqueStyles };
  },

  _allowCustomStyling(styles) {
    if (this.element.nodePrincipal.isSystemPrincipal) {
      return false;
    }
    if (styles.backgroundImage !== "none") {
      return false;
    }
    return true;
  },

  _update() {
    this._setupPseudoClassStyles();
    let computedStyles = getComputedStyles(this.element);
    let defaultStyles = this.element.documentGlobal.getDefaultComputedStyle(
      this.element
    );
    this.actor.sendAsyncMessage("Forms:UpdateDropDown", {
      options: this._buildOptionList(),
      custom: this._allowCustomStyling(computedStyles),
      selectedIndex: this.element.selectedIndex,
      isDarkBackground: ChromeUtils.isDarkBackground(this.element),
      style: supportedStyles(computedStyles, SUPPORTED_SELECT_PROPERTIES),
      defaultStyle: supportedStyles(defaultStyles, SUPPORTED_SELECT_PROPERTIES),
    });
    this._clearPseudoClassStyles();
  },

  dispatchMouseEvent(win, target, eventName) {
    let dict = {
      view: win,
      bubbles: true,
      cancelable: true,
      composed: true,
    };
    let mouseEvent =
      eventName == "click"
        ? new win.PointerEvent(eventName, dict)
        : new win.MouseEvent(eventName, dict);
    target.dispatchEvent(mouseEvent);
  },

  receiveMessage(message) {
    switch (message.name) {
      case "Forms:SelectDropDownItem":
        this.element.selectedIndex = message.data.value;
        this.closedWithClickOn = !message.data.closedWithEnter;
        break;

      case "Forms:DismissedDropDown": {
        if (!this.element) {
          return;
        }

        let element = this.element;
        let win = element.documentGlobal;
        let selectedOption = element.item(element.selectedIndex);

        if (this.closedWithClickOn) {
          this.dispatchMouseEvent(win, selectedOption, "mousedown");
          this.dispatchMouseEvent(win, selectedOption, "mouseup");
        }

        InspectorUtils.removeContentState(
          element,
          kStateActive,
           true
        );

        {
          let changed = this.initialSelection !== selectedOption;
          let handlingUserInput = win.windowUtils.setHandlingUserInput(changed);
          try {
            element.userFinishedInteracting(changed);
          } finally {
            handlingUserInput.destruct();
          }
        }

        if (this.closedWithClickOn) {
          this.dispatchMouseEvent(win, selectedOption, "click");
        }

        this.uninit();
        break;
      }

      case "Forms:MouseOver":
        InspectorUtils.setContentState(this.element, kStateHover);
        break;

      case "Forms:MouseOut":
        InspectorUtils.removeContentState(this.element, kStateHover);
        break;

      case "Forms:MouseUp": {
        let win = this.element.documentGlobal;
        if (message.data.onAnchor) {
          this.dispatchMouseEvent(win, this.element, "mouseup");
        }
        InspectorUtils.removeContentState(this.element, kStateActive);
        if (message.data.onAnchor) {
          this.dispatchMouseEvent(win, this.element, "click");
        }
        break;
      }

      case "Forms:SearchFocused":
        this._closeAfterBlur = false;
        break;

      case "Forms:BlurDropDown-Pong":
        if (!this._closeAfterBlur || !gOpen) {
          return;
        }
        this.actor.sendAsyncMessage("Forms:HideDropDown", {});
        this.uninit();
        break;
    }
  },

  handleEvent(event) {
    switch (event.type) {
      case "pagehide":
        if (this.element.ownerDocument === event.target) {
          this.actor.sendAsyncMessage("Forms:HideDropDown", {});
          this.uninit();
        }
        break;
      case "blur": {
        if (this.element !== event.target || this.disablePopupAutohide) {
          break;
        }
        this._closeAfterBlur = true;
        this.actor.sendAsyncMessage("Forms:BlurDropDown-Ping", {});
        break;
      }
      case "mozhidedropdown":
        if (this.element === event.target) {
          this.actor.sendAsyncMessage("Forms:HideDropDown", {});
          this.uninit();
        }
        break;
      case "transitionend":
        if (
          this.element === event.target &&
          SUPPORTED_SELECT_PROPERTIES.includes(event.propertyName)
        ) {
          this._updateTimer.arm();
        }
        break;
    }
  },
};

function getComputedStyles(element) {
  return element.documentGlobal.getComputedStyle(element);
}

function supportedStyles(cs, supportedProps) {
  let styles = {};
  for (let property of supportedProps) {
    if (property == "font-size") {
      let usedSize = cs.usedFontSize;
      if (usedSize >= 0.0) {
        styles[property] = usedSize + "px";
        continue;
      }
    }
    styles[property] = cs.getPropertyValue(property);
  }
  return styles;
}

function supportedStylesEqual(styles, otherStyles) {
  for (let property in styles) {
    if (styles[property] !== otherStyles[property]) {
      return false;
    }
  }
  return true;
}

function uniqueStylesIndex(cs, uniqueStyles) {
  let styles = supportedStyles(cs, SUPPORTED_OPTION_OPTGROUP_PROPERTIES);
  for (let i = uniqueStyles.length; i--; ) {
    if (supportedStylesEqual(uniqueStyles[i], styles)) {
      return i;
    }
  }
  uniqueStyles.push(styles);
  return uniqueStyles.length - 1;
}

function buildOptionListForChildren(node, uniqueStyles) {
  let result = [];

  let lastWasHR = false;
  for (let child of node.children) {
    let className = ChromeUtils.getClassName(child);
    let isOption = className == "HTMLOptionElement";
    let isOptGroup = className == "HTMLOptGroupElement";
    let isHR = className == "HTMLHRElement";
    if (!isOption && !isOptGroup && !isHR) {
      continue;
    }
    if (child.hidden) {
      continue;
    }

    let cs = getComputedStyles(child);

    if (isHR) {
      if (lastWasHR) {
        continue;
      }

      let info = {
        index: child.index,
        display: cs.display,
        isHR,
      };

      const defaultHRStyle = node.documentGlobal.getDefaultComputedStyle(child);
      if (cs.color != defaultHRStyle.color) {
        info.color = cs.color;
      }

      result.push(info);

      lastWasHR = true;
      continue;
    }
    lastWasHR = false;

    let textContent = isOptGroup
      ? child.getAttribute("label")
      : child.label || child.text;
    if (textContent == null) {
      textContent = "";
    }

    let info = {
      index: child.index,
      isOptGroup,
      textContent,
      disabled: child.disabled,
      display: cs.display,
      tooltip: child.title,
      children: isOptGroup
        ? buildOptionListForChildren(child, uniqueStyles)
        : [],
      styleIndex: uniqueStylesIndex(cs, uniqueStyles),
    };
    result.push(info);
  }
  return result;
}

let currentSelectContentHelper = new WeakMap();

export class SelectChild extends JSWindowActorChild {
  handleEvent(event) {
    if (SelectContentHelper.open) {
      let contentHelper = currentSelectContentHelper.get(this);
      if (contentHelper) {
        contentHelper.handleEvent(event);
      }
      return;
    }

    switch (event.type) {
      case "mozshowdropdown": {
        let contentHelper = new SelectContentHelper(
          event.target,
          { isOpenedViaTouch: false },
          this
        );
        currentSelectContentHelper.set(this, contentHelper);
        break;
      }

      case "mozshowdropdown-sourcetouch": {
        let contentHelper = new SelectContentHelper(
          event.target,
          { isOpenedViaTouch: true },
          this
        );
        currentSelectContentHelper.set(this, contentHelper);
        break;
      }
    }
  }

  receiveMessage(message) {
    let contentHelper = currentSelectContentHelper.get(this);
    if (contentHelper) {
      contentHelper.receiveMessage(message);
    }
  }
}
