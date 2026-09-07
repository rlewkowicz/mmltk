/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { html, ifDefined, staticHtml, literal } from "../vendor/lit.all.mjs";
import { MozLitElement } from "../lit-utils.mjs";

export const GROUP_TYPES = {
  list: "list",
  reorderable: "reorderable-list",
};


export default class MozBoxGroup extends MozLitElement {
  #tabbable = true;

  static properties = {
    type: { type: String },
    listItems: { type: Array, state: true },
    staticItems: { type: Array, state: true },
  };

  static queries = {
    reorderableList: "moz-reorderable-list",
    headerSlot: "slot[name='header']",
    footerSlot: "slot[name='footer']",
  };

  constructor() {
    super();
    this.listItems = [];
    this.staticItems = [];
    this.listMutationObserver = new MutationObserver(
      this.updateItems.bind(this)
    );
  }

  firstUpdated(changedProperties) {
    super.firstUpdated(changedProperties);
    this.listMutationObserver.observe(this, {
      attributeFilter: ["hidden"],
      subtree: true,
      childList: true,
    });
    this.renderRoot.addEventListener("scroll", this.#forwardScroll, {
      capture: true,
    });
    this.updateItems();
  }

  #forwardScroll = () => {
    this.dispatchEvent(new Event("scroll", { composed: true }));
  };

  get isListType() {
    return (
      this.type == GROUP_TYPES.list || this.type == GROUP_TYPES.reorderable
    );
  }

  contentTemplate() {
    if (this.type == GROUP_TYPES.reorderable) {
      return html`<moz-reorderable-list
        class="scroll-container"
        itemselector="moz-box-item:not([static])"
        dragselector=".handle"
        @reorder=${this.handleReorder}
      >
        ${this.slotTemplate()}
      </moz-reorderable-list>`;
    }
    return this.slotTemplate();
  }

  slotTemplate() {
    let isReorderable = this.type == GROUP_TYPES.reorderable;
    if (this.isListType) {
      let listTag = isReorderable ? literal`ol` : literal`ul`;
      return staticHtml`<${listTag}
          tabindex="-1"
          role=${ifDefined(isReorderable ? "listbox" : undefined)}
          class="list scroll-container"
          aria-orientation="vertical"
          @keydown=${this.handleKeydown}
          @focusin=${this.handleFocus}
          @focusout=${this.handleBlur}
        >
          ${this.listItems.map((_, i) => {
            return html`<li
              role=${ifDefined(isReorderable ? "presentation" : undefined)}
            >
              <slot name=${i}></slot>
            </li> `;
          })}
          ${this.staticItems?.map((_, i) => {
            return html`<li
              role=${ifDefined(isReorderable ? "presentation" : undefined)}
            >
              <slot name=${`static-${i}`}></slot>
            </li> `;
          })}
        </${listTag}>
        <slot hidden></slot>
        ${isReorderable ? html`<slot name="static" hidden></slot>` : ""}`;
    }
    return html`<div class="scroll-container" tabindex="-1">
      <slot></slot>
    </div>`;
  }

  getMozBoxElement(listItem) {
    let selector = "moz-box-item, moz-box-link, moz-box-button";
    if (listItem.matches(selector)) {
      return listItem;
    }
    return listItem.querySelector(selector);
  }

  restoreTabindex(item) {
    let element = this.getMozBoxElement(item);
    if (element?.localName === "moz-box-item") {
      if (this.isListType) {
        element.setAttribute("tabindex", "0");
      } else {
        element.removeAttribute("tabindex");
      }
    } else {
      item.removeAttribute("tabindex");
    }
  }

  updateOptionRole(item) {
    let option = this.getMozBoxElement(item);
    if (option && this.type == GROUP_TYPES.reorderable) {
      option.setAttribute("role", "option");
    } else {
      option?.removeAttribute("role");
    }
  }

  reorderArrayFromEvent(array, event) {
    let { draggedIndex, insertAt } = event.detail;
    array = Array.from(array);
    let [moved] = array.splice(draggedIndex, 1);
    array.splice(insertAt, 0, moved);
    return array;
  }

  handleReorder(event) {
    let { targetIndex } = event.detail;

    this.dispatchEvent(
      new CustomEvent("reorder", {
        bubbles: true,
        detail: event.detail,
      })
    );

    requestAnimationFrame(() => {
      this.listItems[targetIndex]?.focus();
    });
  }

  handleKeydown(event) {
    let item = event.originalTarget;
    if (item.localName === "moz-box-item" && item.isDraggable) {
      let detail = this.reorderableList.evaluateKeyDownEvent(event);
      if (detail) {
        event.preventDefault();
        event.stopPropagation();
        this.handleReorder({ detail });
        return;
      }
    }

    if (event.ctrlKey || event.shiftKey || event.altKey || event.metaKey) {
      return;
    }

    let positionElement = event.target.closest("[position]");
    if (!positionElement) {
      return;
    }
    let positionAttr = positionElement.getAttribute("position");
    let currentPosition = parseInt(positionAttr);

    let allItems = [...this.listItems, ...this.staticItems];

    switch (event.key) {
      case "Down":
      case "ArrowDown": {
        event.preventDefault();
        let nextItem = allItems[currentPosition + 1];
        nextItem?.focus(event);
        break;
      }
      case "Up":
      case "ArrowUp": {
        event.preventDefault();
        let prevItem = allItems[currentPosition - 1];
        prevItem?.focus(event);
        break;
      }
    }
  }

  handleFocus(event) {
    if (this.#tabbable) {
      let activeElement = event.target.closest("[position]");
      if (!activeElement) {
        return;
      }
      this.#tabbable = false;
      let activeMozBox = this.getMozBoxElement(activeElement);
      let allItems = [...this.listItems, ...this.staticItems];
      allItems.forEach(item => {
        let element = this.getMozBoxElement(item);
        if (element?.localName === "moz-box-item") {
          element.setAttribute(
            "tabindex",
            element === activeMozBox ? "0" : "-1"
          );
        } else {
          item.setAttribute("tabindex", "-1");
        }
      });
    }
  }

  handleBlur() {
    if (!this.#tabbable) {
      this.#tabbable = true;
      let allItems = [...this.listItems, ...this.staticItems];
      allItems.forEach(item => {
        this.restoreTabindex(item);
      });
    }
  }

  updateItems() {
    let listItems = [];
    let staticItems = [];
    [...this.children].forEach(child => {
      if (child.slot === "header" || child.slot === "footer" || child.hidden) {
        return;
      }
      if (child.slot.includes("static")) {
        staticItems.push(child);
      } else {
        listItems.push(child);
      }
    });
    this.listItems = listItems;
    this.staticItems = staticItems;
  }

  render() {
    return html`
      <link
        rel="stylesheet"
        href="chrome://global/content/elements/moz-box-group.css"
      />
      <slot name="header"></slot>
      ${this.contentTemplate()}
      <slot name="footer"></slot>
    `;
  }

  updated(changedProperties) {
    let headerNode = this.headerSlot.assignedNodes()[0];
    let footerNode = this.footerSlot.assignedNodes().at(-1);
    headerNode?.classList.add("first");
    footerNode?.classList.add("last");

    if (changedProperties.has("listItems") && this.listItems.length) {
      this.listItems.forEach((item, i) => {
        if (this.isListType) {
          item.slot = i;
        }
        item.setAttribute("position", i);
        item.classList.toggle("first", i == 0 && !headerNode);
        item.classList.toggle(
          "last",
          i == this.listItems.length - 1 &&
            !this.staticItems.length &&
            !footerNode
        );
        this.restoreTabindex(item);
        this.updateOptionRole(item);
      });
      if (!this.#tabbable) {
        this.#tabbable = true;
      }
    }

    if (changedProperties.has("staticItems") && this.staticItems.length) {
      this.staticItems.forEach((item, i) => {
        item.slot = `static-${i}`;
        item.setAttribute("position", this.listItems.length + i);
        let staticEl = item.querySelector("moz-box-item") ?? item;
        staticEl.setAttribute("static", "");
        item.classList.toggle(
          "first",
          i == 0 && !this.listItems.length && !headerNode
        );
        item.classList.toggle(
          "last",
          i == this.staticItems.length - 1 && !footerNode
        );
        this.restoreTabindex(item);
        this.updateOptionRole(item);
      });
    }

    if (changedProperties.has("type") && this.isListType) {
      this.updateItems();
    }
  }
}
customElements.define("moz-box-group", MozBoxGroup);
