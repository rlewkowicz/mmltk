/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { html } from "../vendor/lit.all.mjs";
import { MozLitElement } from "../lit-utils.mjs";

const REORDER_EVENT = "reorder";
const DRAGSTART_EVENT = "dragstarted";
const DRAGEND_EVENT = "dragended";
const DRAG_DATA_TYPE_PREFIX = "text/reorderable-item/";
const REORDER_PROP = "__mozReorderableIndex";

export default class MozReorderableList extends MozLitElement {
  static queries = {
    slotEl: "slot",
    indicatorEl: ".indicator",
  };

  static properties = {
    itemSelector: { type: String },
    dragSelector: { type: String },
  };

  #draggedElement = null;
  #dropTargetInfo = null;
  #mutationObserver = null;
  #items = [];

  isXULElement(element) {
    return window.XULElement?.isInstance?.(element);
  }

  getBounds(element) {
    return (
      window.windowUtils?.getBoundsWithoutFlushing?.(element) ||
      element.getBoundingClientRect()
    );
  }

  constructor() {
    super();
    this.itemSelector = "li";
    this.addEventListener("dragstart", this.onDragStart);
    this.addEventListener("dragover", this.onDragOver);
    this.addEventListener("dragleave", this.onDragLeave);
    this.addEventListener("dragend", this.onDragEnd);
    this.addEventListener("drop", this.onDrop);
    this.#mutationObserver = new MutationObserver((...args) =>
      this.onMutation(...args)
    );
  }

  firstUpdated() {
    super.firstUpdated();
    this.getItems();
  }

  connectedCallback() {
    super.connectedCallback();
    this.#mutationObserver.observe(this, {
      childList: true,
      subtree: true,
    });
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    this.#mutationObserver.disconnect();
  }

  onMutation(mutationList) {
    let needsUpdate = false;

    for (const mutation of mutationList) {
      if (mutation.addedNodes.length || mutation.removedNodes.length) {
        needsUpdate = true;
        break;
      }
    }

    if (needsUpdate) {
      requestAnimationFrame(() => {
        this.getItems();
      });
    }
  }

  addDraggableAttribute(items) {
    let draggableItems = items;
    if (this.dragSelector) {
      draggableItems = this.getAssignedElementsBySelector(
        this.dragSelector,
        items
      );
    }
    for (const item of draggableItems) {
      if (!this.isXULElement(item)) {
        item.draggable = true;
      }
    }
  }

  onDragStart(event) {
    let draggedElement = this.getTargetItemFromEvent(event);
    if (!draggedElement) {
      return;
    }

    const dragIndex = this.getItemIndex(draggedElement);
    if (dragIndex === -1) {
      return;
    }

    event.stopPropagation();

    this.emitEvent(DRAGSTART_EVENT, {
      draggedElement,
    });

    if (window.document.nodePrincipal?.isSystemPrincipal) {
      let rect = this.getBounds(draggedElement);
      let scale = window.devicePixelRatio || 1;

      let canvas = document.createElement("canvas");
      canvas.width = rect.width * scale;
      canvas.height = rect.height * scale;

      let context = canvas.getContext("2d");
      context.scale(scale, scale);
      context.drawWindow(
        window,
        rect.left,
        rect.top,
        rect.width,
        rect.height,
        "rgb(255,255,255)"
      );
      event.dataTransfer.setDragImage(canvas, 0, 0);
    }

    if (this.isXULElement(draggedElement)) {
      let documentId = draggedElement.ownerDocument.documentElement.id;
      event.dataTransfer.mozSetDataAt(
        `${DRAG_DATA_TYPE_PREFIX}${documentId}`,
        draggedElement.id,
        0
      );
      event.dataTransfer.addElement(draggedElement);
      event.dataTransfer.effectAllowed = "move";
    }

    this.#draggedElement = draggedElement;
  }

  onDragOver(event) {
    this.#dropTargetInfo = this.getDropTargetInfo(event);
    if (!this.#dropTargetInfo) {
      this.indicatorEl.hidden = true;
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    const { targetIndex, position } = this.#dropTargetInfo;
    const items = this.#items;
    const item = items[targetIndex];

    if (!item) {
      this.indicatorEl.hidden = true;
      return;
    }

    const containerRect = this.getBounds(this);
    const itemRect = this.getBounds(item);

    this.indicatorEl.hidden = false;
    if (position < 0) {
      const top = itemRect.top - containerRect.top;
      this.indicatorEl.style.top = `${Math.max(this.indicatorEl.offsetHeight, top)}px`;
    } else {
      this.indicatorEl.style.top = `${itemRect.bottom - containerRect.top}px`;
    }
  }

  onDragLeave(event) {
    let path = event.composedPath();
    let draggedEl = path.find(el => el.matches?.(this.itemSelector));
    if (!draggedEl) {
      return;
    }
    let target = event.relatedTarget;
    while (target && target !== this) {
      target = target.parentNode;
    }
    if (target !== this) {
      this.indicatorEl.hidden = true;
    }
  }

  onDrop(event) {
    this.#dropTargetInfo = this.getDropTargetInfo(event);
    if (!this.#draggedElement || !this.#dropTargetInfo) {
      return;
    }

    if (this.#draggedElement === this.#dropTargetInfo.targetElement) {
      this.onDragEnd();
      return;
    }

    const draggedIndex = this.getItemIndex(this.#draggedElement);
    const targetIndex = this.#dropTargetInfo.targetIndex;
    const position = this.#dropTargetInfo.position;

    if (
      (position === 0 && targetIndex === draggedIndex - 1) || 
      (position === -1 && targetIndex === draggedIndex + 1) 
    ) {
      this.onDragEnd();
      return;
    }

    event.preventDefault();
    event.stopPropagation();
    this.emitEvent(REORDER_EVENT, {
      draggedElement: this.#draggedElement,
      targetElement: this.#dropTargetInfo.targetElement,
      position,
      draggedIndex,
      targetIndex,
      insertAt:
        (position < 0 ? targetIndex : targetIndex + 1) -
        (draggedIndex < targetIndex ? 1 : 0),
    });
    this.onDragEnd();
  }

  onDragEnd() {
    if (this.#draggedElement == null) {
      return;
    }
    this.emitEvent(DRAGEND_EVENT, {
      draggedElement: this.#draggedElement,
    });
    this.indicatorEl.hidden = true;
    this.#draggedElement = null;
  }

  evaluateKeyDownEvent(event) {
    const direction = isReorderKeyboardEvent(event);
    if (direction == 0) {
      return undefined;
    }
    const fromEl = this.getTargetItemFromEvent(event);
    if (!fromEl) {
      return undefined;
    }
    const fromIndex = this.getItemIndex(fromEl);
    if (fromIndex === -1) {
      return undefined;
    }

    const items = this.#items;
    if (
      (fromIndex === 0 && direction === -1) ||
      (fromIndex === items.length - 1 && direction === 1)
    ) {
      return undefined;
    }

    let targetIndex = fromIndex + direction;
    return {
      draggedElement: fromEl,
      targetElement: items[fromIndex + direction],
      position: Math.min(direction, 0),
      draggedIndex: fromIndex,
      targetIndex,
      insertAt: targetIndex,
    };
  }

  emitEvent(eventName, detail) {
    const customEvent = new CustomEvent(eventName, {
      detail,
    });
    this.dispatchEvent(customEvent);
  }

  getItems() {
    let items = this.getAssignedElementsBySelector(this.itemSelector);
    this.addDraggableAttribute(items);
    items.forEach((item, i) => {
      item[REORDER_PROP] = i;
    });
    this.#items = items;
  }

  getAssignedElementsBySelector(selector, root) {
    if (!root) {
      root = this.slotEl.assignedElements();
    } else if (!Array.isArray(root)) {
      root = [root];
    }

    const collectEls = items => {
      return items.flatMap(item => {
        if (item.matches(selector)) {
          return item;
        }

        let nestedEls =
          item.shadowRoot?.querySelectorAll(selector) ??
          item.querySelectorAll(selector);
        if (nestedEls.length) {
          return [...nestedEls];
        }

        let nextEls =
          item.localName == "slot" ? item.assignedElements() : item.children;
        return collectEls([...(nextEls ?? [])]);
      });
    };

    return collectEls(root);
  }

  getDropTargetInfo(event) {
    const targetItem = this.getTargetItemFromEvent(event);
    if (!targetItem) {
      return null;
    }

    const targetIndex = this.getItemIndex(targetItem);
    if (targetIndex === -1) {
      return null;
    }

    const rect = targetItem.getBoundingClientRect();

    const threshold = rect.height * 0.5;
    const position = event.clientY < rect.top + threshold ? -1 : 0;
    return {
      targetElement: targetItem,
      targetIndex,
      position,
    };
  }

  getItemIndex(item) {
    return item[REORDER_PROP] ?? -1;
  }

  getTargetItemFromEvent(event) {
    const targetItem =
      event.target.closest(this.itemSelector) ||
      event.originalTarget.closest(this.itemSelector);
    return targetItem;
  }

  render() {
    return html`
      <link
        rel="stylesheet"
        href="chrome://global/content/elements/moz-reorderable-list.css"
      />
      <div class="indicator" hidden="" aria-hidden="true"></div>
      <slot @slotchange=${this.getItems}></slot>
    `;
  }
}

export function isReorderKeyboardEvent(event) {
  if (event.code != "ArrowUp" && event.code != "ArrowDown") {
    return 0;
  }
  if (!event.ctrlKey || !event.shiftKey || event.altKey || event.metaKey) {
    return 0;
  }
  return event.code == "ArrowUp" ? -1 : 1;
}

customElements.define("moz-reorderable-list", MozReorderableList);
