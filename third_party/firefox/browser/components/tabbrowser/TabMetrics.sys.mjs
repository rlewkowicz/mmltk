/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const METRIC_SOURCE = Object.freeze({
  TAB_OVERFLOW_MENU: "tab_overflow",
  TAB_GROUP_MENU: "tab_group",
  CANCEL_TAB_GROUP_CREATION: "cancel_create",
  TAB_MENU: "tab_menu",
  TAB_STRIP: "tab_strip",
  DRAG_AND_DROP: "drag",
  KEYBOARD: "keyboard",
  MIDDLE_CLICK: "middle_click",
  MOUSE_WHEEL: "mouse_wheel",
  GESTURE: "gesture",
  SUGGEST: "suggest",
  RECENT_TABS: "recent",
  MESSAGING: "messaging",
  CTRL_TAB: "ctrl_tab",
  UNKNOWN: "unknown",
});

const METRIC_ACTION = Object.freeze({
  ACTIVATE: "activate",
  ADOPT: "adopt",
  DETACH: "detach",
  CLOSE: "close",
  MOVE: "move",
  PIN: "pin",
  UNPIN: "unpin",
});

const METRIC_TABS_LAYOUT = Object.freeze({
  HORIZONTAL: "horizontal",
  VERTICAL: "vertical",
});

const METRIC_REOPEN_TYPE = Object.freeze({
  SAVED: "saved",
  DELETED: "deleted",
});

const METRIC_GROUP_TYPE = Object.freeze({
  EXPANDED: "expanded",
  COLLAPSED: "collapsed",
  SAVED: "saved",
});


const UNKNOWN_CONTEXT = Object.freeze({
  isUserTriggered: false,
  telemetrySource: METRIC_SOURCE.UNKNOWN,
});

function userTriggeredContext(telemetrySource) {
  telemetrySource = telemetrySource || METRIC_SOURCE.UNKNOWN;
  return {
    isUserTriggered: true,
    telemetrySource,
  };
}

function decomposedContext(metricsContext) {
  return {
    ...metricsContext,
    isDecomposed: true,
  };
}

function sourceForEvent(event) {
  if (!event) {
    return METRIC_SOURCE.UNKNOWN;
  }

  if (event.type == "DOMMouseScroll") {
    return METRIC_SOURCE.MOUSE_WHEEL;
  }

  if (SimpleGestureEvent.isInstance(event)) {
    return METRIC_SOURCE.GESTURE;
  }

  if (KeyboardEvent.isInstance(event)) {
    return METRIC_SOURCE.KEYBOARD;
  }

  return METRIC_SOURCE.UNKNOWN;
}

export const TabMetrics = {
  METRIC_SOURCE,
  METRIC_ACTION,
  METRIC_TABS_LAYOUT,
  METRIC_REOPEN_TYPE,
  METRIC_GROUP_TYPE,
  UNKNOWN_CONTEXT,
  sourceForEvent,
  userTriggeredContext,
  decomposedContext,
};
