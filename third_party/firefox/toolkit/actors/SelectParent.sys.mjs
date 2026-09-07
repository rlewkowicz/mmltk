/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";
import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

let prefsChanged = false;

const onPrefsChanged = () => (prefsChanged = true);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "DOM_FORMS_SELECTSEARCH",
  "dom.forms.selectSearch",
  false,
  onPrefsChanged
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "CUSTOM_STYLING_ENABLED",
  "dom.forms.select.customstyling",
  false,
  onPrefsChanged
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "MAC_NATIVE_SELECT_ENABLED",
  "widget.macos.allow-native-select",
  false,
  onPrefsChanged
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "MAC_NATIVE_ANCHORED_MENUS_ENABLED",
  "widget.macos.native-anchored-menus",
  false
);

const SEARCH_MINIMUM_ELEMENTS = 40;

const PROPERTIES_RESET_WHEN_ACTIVE = [
  "color",
  "background-color",
  "text-shadow",
];

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

export var SelectParentHelper = {
  populate(
    menulist,
    items,
    uniqueItemStyles,
    selectedIndex,
    zoom,
    custom,
    isDarkBackground,
    uaStyle,
    selectStyle
  ) {
    let doc = menulist.ownerDocument;

    let menupopup = menulist.menupopup;
    menupopup.textContent = "";

    let stylesheet = menulist.querySelector("#ContentSelectDropdownStylesheet");
    if (stylesheet) {
      stylesheet.remove();
    }

    menupopup.setAttribute("style", "");
    menupopup.style.colorScheme = isDarkBackground ? "dark" : "light";
    menupopup.style.direction = selectStyle.direction;

    if (
      AppConstants.platform == "macosx" &&
      lazy.MAC_NATIVE_ANCHORED_MENUS_ENABLED &&
      !this.disableMacNativeMenu()
    ) {
      menupopup.style.fontSize =
        zoom * parseFloat(selectStyle["font-size"], 10) + "px";
    }

    stylesheet = doc.createElementNS("http://www.w3.org/1999/xhtml", "style");
    stylesheet.setAttribute("id", "ContentSelectDropdownStylesheet");
    stylesheet.hidden = true;
    stylesheet = menulist.appendChild(stylesheet);

    let sheet = stylesheet.sheet;

    if (!custom) {
      selectStyle = uaStyle;
    }

    if (selectStyle.color == selectStyle["background-color"]) {
      selectStyle.color = uaStyle.color;
    }

    let selectBackgroundSet =
      selectStyle["background-color"] != uaStyle["background-color"] ||
      selectStyle.color != uaStyle.color;

    if (custom) {
      if (selectStyle["text-shadow"] != "none") {
        sheet.insertRule(
          `#ContentSelectDropdown > menupopup > :is(menuitem, menucaption)[_moz-menuactive="true"] {
          text-shadow: none;
        }`,
          0
        );
      }

      for (let property of SUPPORTED_SELECT_PROPERTIES) {
        let shouldSkip = (function () {
          if (property == "direction") {
            return true;
          }
          if (!selectStyle[property]) {
            return true;
          }
          if (property == "background-color") {
            return !selectBackgroundSet;
          }
          return selectStyle[property] == uaStyle[property];
        })();

        if (shouldSkip) {
          continue;
        }
        let value = selectStyle[property];
        if (property == "scrollbar-width") {
          property = "--content-select-scrollbar-width";
        }
        if (property == "color") {
          property = "--panel-text-color";
        }
        menupopup.style.setProperty(property, value);
      }
      if (selectBackgroundSet) {
        let parsedColor = menupopup.style.backgroundColor;
        menupopup.style.setProperty(
          "--content-select-background-image",
          `linear-gradient(${parsedColor}, ${parsedColor})`
        );
        menupopup.style.backgroundColor = "";
        menupopup.style.setProperty("--panel-text-color", selectStyle.color);

        sheet.insertRule(
          `#ContentSelectDropdown > menupopup > :is(menuitem, menucaption):not([_moz-menuactive="true"]) {
            color: inherit;
        }`,
          0
        );
      }
    }

    for (let i = 0, len = uniqueItemStyles.length; i < len; ++i) {
      sheet.insertRule(
        `#ContentSelectDropdown .ContentSelectDropdown-item-${i} {}`,
        0
      );
      let style = uniqueItemStyles[i];
      let rule = sheet.cssRules[0].style;
      rule.direction = style.direction;
      rule.fontSize = zoom * parseFloat(style["font-size"], 10) + "px";

      if (!custom) {
        continue;
      }
      let optionBackgroundIsTransparent =
        style["background-color"] == "rgba(0, 0, 0, 0)";
      let optionBackgroundSet =
        !optionBackgroundIsTransparent || style.color != selectStyle.color;

      if (optionBackgroundIsTransparent && style.color != selectStyle.color) {
        style["background-color"] = selectStyle["background-color"];
      }

      if (style.color == style["background-color"]) {
        style.color = selectStyle.color;
      }

      let inactiveRule = null;
      for (const property of SUPPORTED_OPTION_OPTGROUP_PROPERTIES) {
        let shouldSkip = (function () {
          if (property == "direction" || property == "font-size") {
            return true;
          }
          if (!style[property]) {
            return true;
          }
          if (property == "background-color" || property == "color") {
            return !optionBackgroundSet;
          }
          return style[property] == selectStyle[property];
        })();
        if (shouldSkip) {
          continue;
        }
        if (PROPERTIES_RESET_WHEN_ACTIVE.includes(property)) {
          if (!inactiveRule) {
            sheet.insertRule(
              `#ContentSelectDropdown .ContentSelectDropdown-item-${i}:not([_moz-menuactive="true"]) {}`,
              0
            );
            inactiveRule = sheet.cssRules[0].style;
          }
          inactiveRule[property] = style[property];
        } else {
          rule[property] = style[property];
        }
      }
      style.customStyling = selectBackgroundSet || optionBackgroundSet;
    }

    if (custom && selectBackgroundSet) {
      menulist.menupopup.setAttribute("customoptionstyling", "true");
    } else {
      menulist.menupopup.removeAttribute("customoptionstyling");
    }

    this._currentZoom = zoom;
    this._currentMenulist = menulist;
    this.populateChildren(
      menulist,
      custom,
      items,
      uniqueItemStyles,
      selectedIndex
    );
  },

  open(browser, menulist, rect, isOpenedViaTouch, selectParentActor) {
    const canOpen = (() => {
      if (!selectParentActor.browsingContext.canOpenModalPicker) {
        return false;
      }
      if (browser) {
        let tabbrowser = browser.getTabBrowser();
        if (tabbrowser && tabbrowser.selectedBrowser != browser) {
          return false;
        }
      }
      return true;
    })();

    if (!canOpen) {
      selectParentActor.sendAsyncMessage("Forms:DismissedDropDown", {});
      return;
    }

    this._actor = selectParentActor;
    menulist.hidden = false;
    this._currentBrowser = browser;
    this._closedWithEnter = false;
    this._selectRect = rect;
    this._registerListeners(menulist.menupopup);

    let menupopup = menulist.menupopup;
    menupopup.classList.toggle("isOpenedViaTouch", isOpenedViaTouch);

    let win = menulist.documentGlobal;
    if (browser) {
      browser.constrainPopup(menupopup);
      browser.style.pointerEvents = "none";
    } else {
      menupopup.setConstraintRect(new win.DOMRect(0, 0, 0, 0));
    }
    menupopup.openPopupAtScreenRect(
      AppConstants.platform == "macosx" ? "selection" : "after_start",
      rect.left,
      rect.top,
      rect.width,
      rect.height,
      false,
      false
    );
  },

  hide(menulist, browser) {
    if (this._currentBrowser == browser) {
      menulist.menupopup.hidePopup();
    }
  },

  handleEvent(event) {
    switch (event.type) {
      case "mouseup": {
        function inRect(rect, x, y) {
          return (
            x >= rect.left &&
            x <= rect.left + rect.width &&
            y >= rect.top &&
            y <= rect.top + rect.height
          );
        }

        let x = event.screenX,
          y = event.screenY;
        let onAnchor =
          !inRect(this._currentMenulist.menupopup.getOuterScreenRect(), x, y) &&
          inRect(this._selectRect, x, y) &&
          this._currentMenulist.menupopup.state == "open";
        this._actor.sendAsyncMessage("Forms:MouseUp", { onAnchor });
        break;
      }

      case "mouseover":
        if (
          !event.relatedTarget ||
          !this._currentMenulist.contains(event.relatedTarget)
        ) {
          this._actor.sendAsyncMessage("Forms:MouseOver", {});
        }
        break;

      case "mouseout":
        if (
          !event.relatedTarget ||
          !this._currentMenulist.contains(event.relatedTarget)
        ) {
          this._actor.sendAsyncMessage("Forms:MouseOut", {});
        }
        break;

      case "keydown":
        if (event.keyCode == event.DOM_VK_RETURN) {
          this._closedWithEnter = true;
        }
        break;

      case "command":
        if (event.target.hasAttribute("value")) {
          this._actor.sendAsyncMessage("Forms:SelectDropDownItem", {
            value: event.target.value,
            closedWithEnter: this._closedWithEnter,
          });
        }
        break;

      case "fullscreen":
      case "FullscreenWarningOnScreen":
        if (this._currentMenulist) {
          this._currentMenulist.menupopup.hidePopup();
        }
        break;

      case "popuphidden": {
        let popup = event.target;
        this._unregisterListeners(popup);
        popup.parentNode.hidden = true;
        if (this._currentBrowser) {
          this._currentBrowser.style.pointerEvents = "";
          this._currentBrowser = null;
        }
        this._currentMenulist = null;
        this._selectRect = null;
        this._currentZoom = 1;
        try {
          this._actor.sendAsyncMessage("Forms:DismissedDropDown", {});
        } finally {
          this._actor = null;
        }
        break;
      }
    }
  },

  receiveMessage(browser, msg) {
    if (!this._currentMenulist || this._currentBrowser != browser) {
      return;
    }

    if (msg.name == "Forms:UpdateDropDown") {
      let scrollBox = this._currentMenulist.menupopup.scrollBox.scrollbox;
      let scrollTop = scrollBox.scrollTop;

      let options = msg.data.options;
      let selectedIndex = msg.data.selectedIndex;
      this.populate(
        this._currentMenulist,
        options.options,
        options.uniqueStyles,
        selectedIndex,
        this._currentZoom,
        msg.data.custom && lazy.CUSTOM_STYLING_ENABLED,
        msg.data.isDarkBackground,
        msg.data.defaultStyle,
        msg.data.style
      );

      scrollBox.scrollTop = scrollTop;
    } else if (msg.name == "Forms:BlurDropDown-Ping") {
      this._actor.sendAsyncMessage("Forms:BlurDropDown-Pong", {});
    }
  },

  _registerListeners(popup) {
    popup.addEventListener("command", this);
    popup.addEventListener("popuphidden", this);
    popup.addEventListener("mouseover", this);
    popup.addEventListener("mouseout", this);
    popup.documentGlobal.addEventListener("mouseup", this, true);
    popup.documentGlobal.addEventListener("keydown", this, true);
    popup.documentGlobal.addEventListener("fullscreen", this, true);
    popup.documentGlobal.addEventListener(
      "FullscreenWarningOnScreen",
      this,
      true
    );
  },

  _unregisterListeners(popup) {
    popup.removeEventListener("command", this);
    popup.removeEventListener("popuphidden", this);
    popup.removeEventListener("mouseover", this);
    popup.removeEventListener("mouseout", this);
    popup.documentGlobal.removeEventListener("mouseup", this, true);
    popup.documentGlobal.removeEventListener("keydown", this, true);
    popup.documentGlobal.removeEventListener("fullscreen", this, true);
    popup.documentGlobal.removeEventListener(
      "FullscreenWarningOnScreen",
      this,
      true
    );
  },

  populateChildren(
    menulist,
    custom,
    options,
    uniqueOptionStyles,
    selectedIndex,
    parentElement = null,
    isGroupDisabled = false,
    addSearch = true,
    nthChildIndex = 1
  ) {
    let element = menulist.menupopup;

    let ariaOwns = "";
    for (let option of options) {
      let isOptGroup = option.isOptGroup;
      let isHR = option.isHR;

      let xulElement = "menuitem";
      if (isOptGroup) {
        xulElement = "menucaption";
      }
      if (isHR) {
        xulElement = "menuseparator";
      }

      let item = element.ownerDocument.createXULElement(xulElement);
      item.hidden =
        option.display == "none" || (parentElement && parentElement.hidden);

      if (parentElement) {
        item.id = "ContentSelectDropdownOption" + nthChildIndex;
        item.setAttribute("aria-level", "2");
        ariaOwns += item.id + " ";
      }

      element.appendChild(item);
      nthChildIndex++;

      if (isHR) {
        item.style.color = (custom && option.color) || "";

        continue;
      }

      item.className = `ContentSelectDropdown-item-${option.styleIndex}`;

      if (isOptGroup) {
        item.setAttribute("role", "group");
      }
      item.setAttribute("label", option.textContent);
      item.hiddenByContent = item.hidden;
      item.setAttribute("tooltiptext", option.tooltip);

      if (uniqueOptionStyles[option.styleIndex].customStyling) {
        item.setAttribute("customoptionstyling", "true");
      } else {
        item.removeAttribute("customoptionstyling");
      }

      let isDisabled = isGroupDisabled || option.disabled;
      if (isDisabled) {
        item.setAttribute("disabled", "true");
      }

      if (isOptGroup) {
        nthChildIndex = this.populateChildren(
          menulist,
          custom,
          option.children,
          uniqueOptionStyles,
          selectedIndex,
          item,
          isDisabled,
          false,
          nthChildIndex
        );
      } else {
        if (option.index == selectedIndex) {
          menulist.selectedItem = item;

          menulist.activeChild = item;
        }

        item.setAttribute("value", option.index);

        if (parentElement) {
          item.setAttribute("indented", true);
        }
      }
    }

    if (parentElement && ariaOwns) {
      parentElement.setAttribute("aria-owns", ariaOwns);
    }

    if (
      lazy.DOM_FORMS_SELECTSEARCH &&
      addSearch &&
      element.childElementCount > SEARCH_MINIMUM_ELEMENTS
    ) {
      let searchbox = element.ownerDocument.createElement("moz-input-search");
      searchbox.className = "contentSelectDropdown-searchbox";
      searchbox.addEventListener("input", this.onSearchInput);
      searchbox.addEventListener("focus", this.onSearchFocus.bind(this));
      searchbox.addEventListener("blur", this.onSearchBlur);
      searchbox.addEventListener("MozInputSearch:search", this.onSearchInput);

      searchbox.addEventListener(
        "keydown",
        event => {
          this.onSearchKeydown(event, menulist);
        },
        true
      );

      element.insertBefore(searchbox, element.children[0]);
    }

    return nthChildIndex;
  },

  disableMacNativeMenu() {
    return (
      AppConstants.platform == "macosx" &&
      (lazy.CUSTOM_STYLING_ENABLED ||
        lazy.DOM_FORMS_SELECTSEARCH ||
        !lazy.MAC_NATIVE_SELECT_ENABLED)
    );
  },

  onSearchKeydown(event, menulist) {
    if (event.defaultPrevented) {
      return;
    }

    let searchbox = event.currentTarget;
    switch (event.key) {
      case "Escape":
        searchbox.parentElement.hidePopup();
        break;
      case "ArrowDown":
      case "Enter":
      case "Tab":
        searchbox.blur();
        if (
          searchbox.nextElementSibling.localName == "menuitem" &&
          !searchbox.nextElementSibling.hidden
        ) {
          menulist.activeChild = searchbox.nextElementSibling;
        } else {
          let currentOption = searchbox.nextElementSibling;
          while (
            currentOption &&
            (currentOption.localName != "menuitem" || currentOption.hidden)
          ) {
            currentOption = currentOption.nextElementSibling;
          }
          if (currentOption) {
            menulist.activeChild = currentOption;
          } else {
            searchbox.focus();
          }
        }
        break;
      default:
        return;
    }
    event.preventDefault();
  },

  onSearchInput(event) {
    let searchObj = event.currentTarget;

    let input = searchObj.value.toLowerCase();
    let menupopup = searchObj.parentElement;
    let menuItems = menupopup.querySelectorAll("menuitem, menucaption");

    let allHidden = true;
    let prevCaption = null;

    for (let currentItem of menuItems) {
      if (!currentItem.hiddenByContent) {
        let itemLabel = currentItem.getAttribute("label")?.toLowerCase() || "";
        let itemTooltip =
          currentItem.getAttribute("title")?.toLowerCase() || "";

        if (!input) {
          currentItem.hidden = false;
        } else if (currentItem.localName == "menucaption") {
          if (prevCaption != null) {
            prevCaption.hidden = allHidden;
          }
          prevCaption = currentItem;
          allHidden = true;
        } else {
          if (
            !currentItem.hasAttribute("indented") &&
            currentItem.previousElementSibling.hasAttribute("indented")
          ) {
            if (prevCaption != null) {
              prevCaption.hidden = allHidden;
            }
            prevCaption = null;
            allHidden = true;
          }
          if (itemLabel.includes(input) || itemTooltip.includes(input)) {
            currentItem.hidden = false;
            allHidden = false;
          } else {
            currentItem.hidden = true;
          }
        }
        if (prevCaption != null) {
          prevCaption.hidden = allHidden;
        }
      }
    }
  },

  onSearchFocus(event) {
    let menupopup = event.target.closest("menupopup");
    menupopup.parentElement.activeChild = null;
    menupopup.setAttribute("ignorekeys", "true");
    this._actor.sendAsyncMessage("Forms:SearchFocused", {});
  },

  onSearchBlur(event) {
    let menupopup = event.target.closest("menupopup");
    menupopup.setAttribute(
      "ignorekeys",
      AppConstants.platform == "win" ? "shortcuts" : "false"
    );
  },
};

export class SelectParent extends JSWindowActorParent {
  get relevantBrowser() {
    return this.browsingContext.top.embedderElement;
  }

  get _document() {
    return this.browsingContext.topChromeWindow.document;
  }

  get _menulist() {
    return this._document.getElementById("ContentSelectDropdown");
  }

  _createMenulist() {
    let document = this._document;
    let menulist = document.createXULElement("menulist");
    menulist.setAttribute("id", "ContentSelectDropdown");
    menulist.setAttribute("popuponly", "true");
    menulist.setAttribute("hidden", "true");

    let popup = menulist.appendChild(document.createXULElement("menupopup"));
    popup.setAttribute("id", "ContentSelectDropdownPopup");
    popup.setAttribute("activateontab", "true");
    popup.setAttribute("position", "after_start");
    popup.setAttribute("tabspecific", "true");
    popup.setAttribute("level", "parent");
    if (AppConstants.platform == "win") {
      popup.setAttribute("consumeoutsideclicks", "false");
      popup.setAttribute("ignorekeys", "shortcuts");
    } else if (SelectParentHelper.disableMacNativeMenu()) {
      popup.toggleAttribute("nonnative", true);
    }

    let container =
      document.getElementById("mainPopupSet") ||
      document.querySelector("popupset") ||
      document.documentElement.appendChild(
        document.createXULElement("popupset")
      );

    container.appendChild(menulist);
    return menulist;
  }

  receiveMessage(message) {
    switch (message.name) {
      case "Forms:ShowDropDown": {
        let menulist = this._menulist || this._createMenulist();

        if (prefsChanged) {
          if (AppConstants.platform == "macosx") {
            menulist.menupopup.toggleAttribute(
              "nonnative",
              SelectParentHelper.disableMacNativeMenu()
            );
          }
          prefsChanged = false;
        }

        let data = message.data;

        SelectParentHelper.populate(
          menulist,
          data.options.options,
          data.options.uniqueStyles,
          data.selectedIndex,
          this.browsingContext.fullZoom,
          data.custom && lazy.CUSTOM_STYLING_ENABLED,
          data.isDarkBackground,
          data.defaultStyle,
          data.style
        );
        SelectParentHelper.open(
          this.relevantBrowser,
          menulist,
          data.rect,
          data.isOpenedViaTouch,
          this
        );
        break;
      }

      case "Forms:HideDropDown": {
        SelectParentHelper.hide(this._menulist, this.relevantBrowser);
        break;
      }

      default:
        SelectParentHelper.receiveMessage(this.relevantBrowser, message);
    }
  }
}
