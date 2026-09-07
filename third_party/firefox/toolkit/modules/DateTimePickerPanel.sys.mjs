/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { InputPickerPanelCommon } from "./InputPickerPanelCommon.sys.mjs";


const TIME_PICKER_WIDTH = "12.8em";
const TIME_PICKER_HEIGHT = "22.8em";
const DATE_PICKER_WIDTH = "24em";
const DATE_PICKER_HEIGHT = "26.9em";
const DATETIME_PICKER_WIDTH = "39.8em";
const DATETIME_PICKER_HEIGHT = "26.9em";

const MS_PER_SECOND = 1000;
const MS_PER_MINUTE = 60 * MS_PER_SECOND;

export class DateTimePickerPanel extends InputPickerPanelCommon {
  constructor(element) {
    super(element, "chrome://global/content/datetimepicker.xhtml");
  }

  openPickerImpl(type) {
    if (
      type == "datetime-local" &&
      !Services.prefs.getBoolPref("dom.forms.datetime.timepicker")
    ) {
      type = "date";
    }
    switch (type) {
      case "time": {
        return {
          type,
          width: TIME_PICKER_WIDTH,
          height: TIME_PICKER_HEIGHT,
        };
      }
      case "date": {
        return {
          type,
          width: DATE_PICKER_WIDTH,
          height: DATE_PICKER_HEIGHT,
        };
      }
      case "datetime-local": {
        return {
          type,
          width: DATETIME_PICKER_WIDTH,
          height: DATETIME_PICKER_HEIGHT,
        };
      }
    }
    throw new Error(`Unexpected type ${type}`);
  }

  initPickerImpl(type, detail) {
    let locale = new Services.intl.Locale(
      Services.locale.webExposedLocales[0],
      {
        calendar: "gregory",
      }
    ).toString();

    locale = locale.replace(/^pt-PT/i, "pt");

    const dir = Services.locale.isAppLocaleRTL ? "rtl" : "ltr";
    const { hour12 } = new Services.intl.DateTimeFormat(locale, {
      hour: "numeric",
    }).resolvedOptions();

    const { year, month, day, hour, minute, second, millisecond } =
      detail.value;
    const flattenDetail = {
      type,
      year,
      month: month == undefined ? undefined : month - 1,
      day,
      hour,
      minute,
      second,
      millisecond,
      locale,
      dir,
      format: hour12 ? "12" : "24",
      min: detail.min,
      max: detail.max,
      step: detail.step,
      stepBase: detail.stepBase,
    };

    if (
      type !== "date" &&
      Services.prefs.getBoolPref("dom.forms.datetime.timepicker")
    ) {
      flattenDetail.showSeconds =
        second != undefined ||
        detail.stepBase % MS_PER_MINUTE != 0 ||
        detail.step % MS_PER_MINUTE != 0;
      flattenDetail.showMilliseconds =
        millisecond != undefined ||
        detail.stepBase % MS_PER_SECOND != 0 ||
        detail.step % MS_PER_SECOND != 0;
    }

    if (type !== "time") {
      const { firstDayOfWeek, weekends } = this.getCalendarInfo(locale);

      const monthDisplayNames = new Services.intl.DisplayNames(locale, {
        type: "month",
        style: "short",
        calendar: "gregory",
      });
      const monthStrings = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12].map(
        monthNumber => monthDisplayNames.of(monthNumber)
      );

      const weekdayDisplayNames = new Services.intl.DisplayNames(locale, {
        type: "weekday",
        style: "abbreviated",
        calendar: "gregory",
      });
      const weekdayStrings = [
        7, 1, 2, 3, 4, 5, 6,
      ].map(weekday => weekdayDisplayNames.of(weekday));
      Object.assign(flattenDetail, {
        firstDayOfWeek,
        weekends,
        monthStrings,
        weekdayStrings,
      });
    }
    return flattenDetail;
  }

  sendPickerValueChangedImpl(type, pickerState) {
    let { year, month, day, hour, minute, second, millisecond } = pickerState;
    if (month !== undefined) {
      month += 1;
    }
    switch (type) {
      case "time": {
        return { hour, minute, second, millisecond };
      }
      case "date": {
        return { year, month, day };
      }
      case "datetime-local": {
        return { year, month, day, hour, minute, second, millisecond };
      }
    }
    throw new Error(`Unexpected type ${type}`);
  }

  getCalendarInfo(locale) {
    const calendarInfo = Services.intl.getCalendarInfo(locale);

    function toDateWeekday(day) {
      return day === 7 ? 0 : day;
    }

    let firstDayOfWeek = toDateWeekday(calendarInfo.firstDayOfWeek),
      weekend = calendarInfo.weekend;

    let weekends = weekend.map(toDateWeekday);

    return {
      firstDayOfWeek,
      weekends,
    };
  }
}
