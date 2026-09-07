/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

var gManagers = new WeakMap();

const kPaletteId = "customization-palette";

class AreaPositionManager {
  #rtl = false;

  #containerInfo = null;

  #horizontalDistance = 0;

  #heightToWidthFactor = 0;

  constructor(aContainer) {
    this.#rtl = aContainer.documentGlobal.RTL_UI;
    this.#containerInfo = DOMRectReadOnly.fromRect(
      aContainer.getBoundingClientRect()
    );
    this.update(aContainer);
  }

  #nodePositionStore = new WeakMap();

  #lastPlaceholderInsertion = null;

  update(aContainer) {
    let last = null;
    let singleItemHeight;
    for (let child of aContainer.children) {
      if (child.hidden) {
        continue;
      }
      let coordinates = this.#lazyStoreGet(child);
      if (!this.#horizontalDistance && last) {
        this.#horizontalDistance = coordinates.left - last.left;
      }
      if (!singleItemHeight) {
        singleItemHeight = coordinates.height;
      }
      last = coordinates;
    }
    this.#heightToWidthFactor = this.#containerInfo.width / singleItemHeight;
  }

  find(aContainer, aX, aY) {
    let closest = null;
    let minCartesian = Number.MAX_VALUE;
    let containerX = this.#containerInfo.left;
    let containerY = this.#containerInfo.top;

    for (let node of aContainer.children) {
      let coordinates = this.#lazyStoreGet(node);
      let offsetX = coordinates.x - containerX;
      let offsetY = coordinates.y - containerY;
      let hDiff = offsetX - aX;
      let vDiff = offsetY - aY;
      hDiff /= this.#heightToWidthFactor;

      let cartesianDiff = hDiff * hDiff + vDiff * vDiff;
      if (cartesianDiff < minCartesian) {
        minCartesian = cartesianDiff;
        closest = node;
      }
    }

    if (closest) {
      let targetBounds = this.#lazyStoreGet(closest);
      let farSide = this.#rtl ? "left" : "right";
      let outsideX = targetBounds[farSide];
      if (aY > targetBounds.top && aY < targetBounds.bottom) {
        if ((!this.#rtl && aX > outsideX) || (this.#rtl && aX < outsideX)) {
          return closest.nextElementSibling || aContainer;
        }
      }
    }
    return closest;
  }

  insertPlaceholder(aContainer, aBefore, aSize, aIsFromThisArea) {
    let isShifted = false;
    for (let child of aContainer.children) {
      if (child.hidden) {
        continue;
      }
      if (child == aBefore) {
        isShifted = true;
      }
      if (isShifted) {
        if (aIsFromThisArea && !this.#lastPlaceholderInsertion) {
          child.setAttribute("notransition", "true");
        }
        child.style.transform = this.#diffWithNext(child, aSize);
      } else {
        child.style.transform = "";
      }
    }

    if (
      aContainer.lastElementChild &&
      aIsFromThisArea &&
      !this.#lastPlaceholderInsertion
    ) {
      aContainer.lastElementChild.getBoundingClientRect();
      for (let child of aContainer.children) {
        child.removeAttribute("notransition");
      }
    }
    this.#lastPlaceholderInsertion = aBefore;
  }

  clearPlaceholders(aContainer, aNoTransition) {
    for (let child of aContainer.children) {
      if (aNoTransition) {
        child.setAttribute("notransition", true);
      }
      child.style.transform = "";
      if (aNoTransition) {
        child.getBoundingClientRect();
        child.removeAttribute("notransition");
      }
    }
    if (aNoTransition) {
      this.#lastPlaceholderInsertion = null;
    }
  }

  #diffWithNext(aNode, aSize) {
    let xDiff;
    let yDiff = null;
    let nodeBounds = this.#lazyStoreGet(aNode);
    let side = this.#rtl ? "right" : "left";
    let next = this.#getVisibleSiblingForDirection(aNode, "next");
    if (next) {
      let otherBounds = this.#lazyStoreGet(next);
      xDiff = otherBounds[side] - nodeBounds[side];
      yDiff = otherBounds.top - nodeBounds.top;
    } else {
      let firstNode = this.#firstInRow(aNode);
      if (aNode == firstNode) {
        xDiff = this.#horizontalDistance || (this.#rtl ? -1 : 1) * aSize.width;
      } else {
        xDiff = this.#moveNextBasedOnPrevious(aNode, nodeBounds, firstNode);
      }
    }

    if (yDiff === null) {
      if ((xDiff > 0 && this.#rtl) || (xDiff < 0 && !this.#rtl)) {
        yDiff = aSize.height;
      } else {
        yDiff = 0;
      }
    }
    return "translate(" + xDiff + "px, " + yDiff + "px)";
  }

  #moveNextBasedOnPrevious(aNode, aNodeBounds, aFirstNodeInRow) {
    let next = this.#getVisibleSiblingForDirection(aNode, "previous");
    let otherBounds = this.#lazyStoreGet(next);
    let side = this.#rtl ? "right" : "left";
    let xDiff = aNodeBounds[side] - otherBounds[side];
    let bound = this.#containerInfo[this.#rtl ? "left" : "right"];
    if (
      (!this.#rtl && xDiff + aNodeBounds.right > bound) ||
      (this.#rtl && xDiff + aNodeBounds.left < bound)
    ) {
      xDiff = this.#lazyStoreGet(aFirstNodeInRow)[side] - aNodeBounds[side];
    }
    return xDiff;
  }

  #lazyStoreGet(aNode) {
    let rect = this.#nodePositionStore.get(aNode);
    if (!rect) {
      rect = DOMRectReadOnly.fromRect(aNode.getBoundingClientRect());
      this.#nodePositionStore.set(aNode, rect);
    }
    return rect;
  }

  #firstInRow(aNode) {
    let bound = Math.floor(this.#lazyStoreGet(aNode).top);
    let rv = aNode;
    let prev;
    while (rv && (prev = this.#getVisibleSiblingForDirection(rv, "previous"))) {
      if (Math.floor(this.#lazyStoreGet(prev).bottom) <= bound) {
        return rv;
      }
      rv = prev;
    }
    return rv;
  }

  #getVisibleSiblingForDirection(aNode, aDirection) {
    let rv = aNode;
    do {
      rv = rv[aDirection + "ElementSibling"];
    } while (rv && rv.hidden);
    return rv;
  }
}

export var DragPositionManager = {
  start(aWindow) {
    let paletteArea = aWindow.document.getElementById(kPaletteId);
    let positionManager = gManagers.get(paletteArea);
    if (positionManager) {
      positionManager.update(paletteArea);
    } else {
      gManagers.set(paletteArea, new AreaPositionManager(paletteArea));
    }
  },

  stop() {
    gManagers = new WeakMap();
  },

  getManagerForArea(aArea) {
    return gManagers.get(aArea);
  },
};

Object.freeze(DragPositionManager);
