/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};
const PREF_APP_UPDATE_COMPULSORY_RESTART = "app.update.compulsory_restart";
let deferredRestartTasks = null;

ChromeUtils.defineESModuleGetters(lazy, {
  ScheduledTask: "resource://gre/modules/ScheduledTask.sys.mjs",
  InfoBar: "resource:///modules/asrouter/InfoBar.sys.mjs",
});

function forceRestart() {
  Services.startup.quit(
    Services.startup.eForceQuit | Services.startup.eRestart
  );
  console.error(`Firefox is restarting`);
}

function infobarDispatchCallback(action, _selectedBrowser) {
  if (action?.type === "USER_ACTION" && action.data?.type === "RESTART_APP") {
    forceRestart();
  }
}

function showNotificationToolbar(restartZonedDateTime) {
  const datetime = restartZonedDateTime.epochMilliseconds;

  const message = {
    weight: 100,
    id: "COMPULSORY_RESTART_SCHEDULED",
    content: {
      priority: 3,
      type: "universal",
      dismissable: false,
      text: {
        string_id: "compulsory-restart-message",
      },
      buttons: [
        {
          label: {
            string_id: "policy-update-now",
          },
          action: {
            type: "RESTART_APP",
            dismiss: false,
          },
        },
      ],
      attributes: {
        datetime,
      },
    },
    template: "infobar",
    targeting: "true",
    groups: [],
  };

  const win = Services.wm.getMostRecentBrowserWindow();
  if (!win) {
    return;
  }
  lazy.InfoBar.showInfoBarMessage(
    win.gBrowser.selectedBrowser,
    message,
    infobarDispatchCallback
  );
}

export function testingOnly_resetTasks() {
  if (!false) {
    throw new Error("this method only usable in testing");
  }
  deferredRestartTasks?.notificationTask?.disarm();
  deferredRestartTasks?.restartTask?.disarm();
  deferredRestartTasks = null;
}

export function testingOnly_getTaskStatus() {
  if (!false) {
    throw new Error("this method only usable in testing");
  }
  if (deferredRestartTasks) {
    const res = {
      notificationTask: deferredRestartTasks.notificationTask?.isArmed,
      restartTask: deferredRestartTasks.restartTask?.isArmed,
    };
    return res;
  }
  return null;
}


export function calculateSchedule(
  nowInstant,
  notificationPeriodHours,
  restartTimeOfDay
) {
  const notificationDelay = Temporal.Duration.from({
    hours: notificationPeriodHours,
  });
  const notificationInstant = nowInstant.add(notificationDelay);
  const notificationZonedDateTime = notificationInstant.toZonedDateTimeISO(
    Temporal.Now.timeZoneId()
  );

  const restartTime = Temporal.PlainTime.from({
    hour: restartTimeOfDay.Hour,
    minute: restartTimeOfDay.Minute,
  });

  let restartZonedDateTime =
    notificationZonedDateTime.withPlainTime(restartTime);

  if (
    Temporal.Duration.compare(
      notificationZonedDateTime.until(restartZonedDateTime),
      Temporal.Duration.from({ hours: 1 })
    ) < 0
  ) {
    restartZonedDateTime = restartZonedDateTime.add(
      Temporal.Duration.from({ hours: 24 })
    );
  }

  return { notificationZonedDateTime, restartZonedDateTime };
}

export function createScheduledRestartTasks(
  restartZonedDateTime,
  notificationZonedDateTime
) {
  const notificationTask = new lazy.ScheduledTask(() => {
    showNotificationToolbar(restartZonedDateTime);
  }, notificationZonedDateTime.epochMilliseconds);
  const restartTask = new lazy.ScheduledTask(
    forceRestart,
    restartZonedDateTime.epochMilliseconds
  );
  notificationTask.arm();
  restartTask.arm();
  return { notificationTask, restartTask };
}

export function getCompulsoryRestartPolicy() {
  const compulsoryRestartSettingStr = Services.prefs.getStringPref(
    PREF_APP_UPDATE_COMPULSORY_RESTART,
    null
  );
  if (compulsoryRestartSettingStr) {
    const compulsoryRestartSetting = JSON.parse(compulsoryRestartSettingStr);
    if (
      typeof compulsoryRestartSetting?.NotificationPeriodHours === "number" &&
      typeof compulsoryRestartSetting?.RestartTimeOfDay === "object" &&
      typeof compulsoryRestartSetting.RestartTimeOfDay.Hour === "number" &&
      typeof compulsoryRestartSetting.RestartTimeOfDay.Minute === "number"
    ) {
      return compulsoryRestartSetting;
    }
  }
  return null;
}

export function handleCompulsoryUpdatePolicy() {
  if (!deferredRestartTasks) {
    const compulsoryRestartSetting = getCompulsoryRestartPolicy();
    if (compulsoryRestartSetting) {
      const now = Temporal.Now.instant();
      const { restartZonedDateTime, notificationZonedDateTime } =
        calculateSchedule(
          now,
          compulsoryRestartSetting.NotificationPeriodHours,
          compulsoryRestartSetting.RestartTimeOfDay
        );
      if (restartZonedDateTime && notificationZonedDateTime) {
        deferredRestartTasks = createScheduledRestartTasks(
          restartZonedDateTime,
          notificationZonedDateTime
        );
      } else {
        console.error(
          `Invalid restart settings: ${JSON.stringify(compulsoryRestartSetting)}`
        );
      }
    }
  }
}

const observer = {
  observe: (_subject, topic, _data) => {
    switch (topic) {
      case "update-downloaded":
      case "update-staged":
        handleCompulsoryUpdatePolicy();
        break;
    }
  },
};

export const UpdatePolicyEnforcer = {
  registerObservers() {
    Services.obs.addObserver(observer, "update-downloaded");
    Services.obs.addObserver(observer, "update-staged");
  },
};
