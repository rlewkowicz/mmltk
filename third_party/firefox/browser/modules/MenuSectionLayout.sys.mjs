/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export class MenuSectionLayout {
  constructor(layout, { dynamicItemSelectors = [] } = {}) {
    this.layout = layout;
    this.dynamicItemSelectors = dynamicItemSelectors;
  }

  static placementsFor(sections) {
    let result = [];
    for (let section of sections) {
      for (let entry of section.items) {
        if (typeof entry === "string") {
          result.push({ selector: entry, optional: false });
        } else if (Array.isArray(entry)) {
          for (let selector of entry) {
            result.push({ selector, optional: false });
          }
        } else {
          result.push({
            selector: entry.selector,
            optional: entry.optional === true,
          });
        }
      }
    }
    return result;
  }

  arrange(rootPopup) {
    if (!rootPopup) {
      throw new Error("MenuSectionLayout: rootPopup is required");
    }

    let popups = new Map();
    for (let popupId of Object.keys(this.layout)) {
      let popup =
        rootPopup.id === popupId
          ? rootPopup
          : rootPopup.querySelector(`#${popupId}`);
      if (!popup) {
        throw new Error(
          `MenuSectionLayout: no popup "#${popupId}" under #${rootPopup.id}`
        );
      }
      popups.set(popupId, popup);
    }

    let claimed = new Set();
    let orderedByPopup = new Map();

    for (let [popupId, sections] of Object.entries(this.layout)) {
      let ordered = [];
      for (let { selector, optional } of MenuSectionLayout.placementsFor(
        sections
      )) {
        let matches = [...rootPopup.querySelectorAll(selector)].filter(
          node => !claimed.has(node)
        );
        if (!matches.length) {
          if (optional) {
            continue;
          }
          throw new Error(
            `MenuSectionLayout: no unclaimed node matching "${selector}" under #${rootPopup.id}`
          );
        }
        if (matches.length > 1) {
          throw new Error(
            `MenuSectionLayout: "${selector}" matches ${matches.length} nodes under #${rootPopup.id}; selectors must be unique`
          );
        }
        claimed.add(matches[0]);
        ordered.push(matches[0]);
      }
      orderedByPopup.set(popupId, ordered);
    }

    let openPopups = new Set(
      Object.entries(this.layout)
        .filter(([, sections]) => sections.some(section => section.open))
        .map(([popupId]) => popupId)
    );

    for (let [popupId, popup] of popups) {
      let children = [...popup.children];
      let lastPlaced = children.reduce(
        (last, node, index) => (claimed.has(node) ? index : last),
        -1
      );
      let unplaced = children.filter(
        (node, index) =>
          !claimed.has(node) &&
          !this.dynamicItemSelectors.some(sel => node.matches(sel)) &&
          !(openPopups.has(popupId) && index > lastPlaced)
      );
      if (unplaced.length) {
        let names = unplaced
          .map(node => (node.id ? "#" + node.id : node.localName))
          .join(", ");
        throw new Error(
          `MenuSectionLayout: ${unplaced.length} child(ren) of #${popupId} not placed by any section: ${names}`
        );
      }
    }

    for (let [popupId, ordered] of orderedByPopup) {
      let popup = popups.get(popupId);
      let fragment = popup.ownerDocument.createDocumentFragment();
      for (let node of ordered) {
        fragment.appendChild(node);
      }
      popup.insertBefore(fragment, popup.firstChild);
    }
  }
}
