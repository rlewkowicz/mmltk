/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */




class _TabGroupState {
  collect(tabGroup) {
    return {
      id: tabGroup.id,
      name: tabGroup.label,
      color: tabGroup.color,
      collapsed: tabGroup.collapsed,
      saveOnWindowClose: tabGroup.saveOnWindowClose,
    };
  }

  closed(tabGroup, sourceWindowId) {
    let closedData = this.collect(tabGroup);
    closedData.closedAt = Date.now();
    closedData.sourceWindowId = sourceWindowId;
    closedData.tabs = [];
    closedData.splitViews = [];
    return closedData;
  }

  savedInOpenWindow(tabGroup, sourceWindowId) {
    let savedData = this.closed(tabGroup, sourceWindowId);
    savedData.saved = true;
    return savedData;
  }

  savedInClosedWindow(tabGroupState, windowClosedId) {
    let savedData = tabGroupState;
    savedData.saved = true;
    savedData.closedAt = Date.now();
    savedData.windowClosedId = windowClosedId;
    savedData.tabs = [];
    savedData.splitViews = [];
    return savedData;
  }

  abbreviated(tabGroupState) {
    let abbreviatedData = {
      id: tabGroupState.id,
      name: tabGroupState.name,
      color: tabGroupState.color,
      collapsed: tabGroupState.collapsed,
    };
    return abbreviatedData;
  }
}

export const TabGroupState = new _TabGroupState();
