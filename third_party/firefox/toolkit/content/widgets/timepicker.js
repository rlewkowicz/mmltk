/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


"use strict";

function TimePicker(context) {
  this.context = context;
  this._attachEventListeners();
}

{
  const DAY_IN_MS = 86400000,
    MIN_DATE = -62135596800000,
    MAX_DATE = 8640000000000000;

  TimePicker.prototype = {
    init(props) {
      if (props.type == "date") {
        return;
      }
      if (props.type == "datetime-local") {
        const timepicker = this.context;
        const datetimepicker = timepicker.parentNode;
        const datepicker = datetimepicker.children.namedItem("date-picker");
        timepicker.setAttribute("role", "group");
        timepicker.removeAttribute("aria-modal");
        datepicker.setAttribute("role", "group");
        datepicker.removeAttribute("aria-modal");
        datetimepicker.setAttribute("role", "dialog");
        datetimepicker.setAttribute("aria-modal", "true");
        datetimepicker.setAttribute("data-l10n-id", "datetime-picker-label");
      }
      this.context.hidden = false;
      this.props = props || {};
      this._setDefaultState();
      this._createComponents();
      this._setComponentStates();
      window.PICKER_READY = true;
      document.dispatchEvent(new CustomEvent("PickerReady"));
      if (props.type == "time") {
        this.components.hour.elements.spinner.focus();
      }
    },

    _setDefaultState() {
      const {
        type,
        year,
        month,
        day,
        hour,
        minute,
        second,
        millisecond,
        min,
        max,
        step,
        format,
        showSeconds,
        showMilliseconds,
      } = this.props;
      const now = new Date();

      let defaultMin = 0;
      let defaultMax = DAY_IN_MS - 1;
      if (type == "datetime-local") {
        defaultMin = MIN_DATE;
        defaultMax = MAX_DATE;
      }
      let timeKeeper = new TimeKeeper({
        type,
        year,
        month,
        day,
        min: new Date(Number.isNaN(min) ? defaultMin : min),
        max: new Date(Number.isNaN(max) ? defaultMax : max),
        step,
        format: format || "12",
      });
      const newState = {
        hour: hour == undefined ? now.getHours() : hour,
        minute: minute == undefined ? now.getMinutes() : minute,
      };
      if (showSeconds) {
        newState.second = second == undefined ? now.getSeconds() : second;
      }
      if (showMilliseconds) {
        newState.millisecond =
          millisecond == undefined ? now.getMilliseconds() : millisecond;
      }
      timeKeeper.setState(newState);
      if (timeKeeper.state.isInvalid) {
        const validPeriods = timeKeeper.ranges.dayPeriod.filter(m => m.enabled);
        if (validPeriods.length) {
          timeKeeper.setDayPeriod(validPeriods[0].value);
        }
        const validHours = timeKeeper.ranges.hours.filter(h => h.enabled);
        if (validHours.length) {
          timeKeeper.setHour(validHours[0].value);
        }
        const validMinutes = timeKeeper.ranges.minutes.filter(m => m.enabled);
        if (validMinutes.length) {
          timeKeeper.setMinute(validMinutes[0].value);
        }
        const validSeconds = timeKeeper.ranges.seconds.filter(s => s.enabled);
        if (validSeconds.length) {
          timeKeeper.setSecond(validSeconds[0].value);
        }
        const validMilliseconds = timeKeeper.ranges.milliseconds.filter(
          ms => ms.enabled
        );
        if (validMilliseconds.length) {
          timeKeeper.setMillisecond(validMilliseconds[0].value);
        }
      }

      this.state = { timeKeeper };
    },

    _createComponents() {
      const { locale, format, showSeconds, showMilliseconds } = this.props;
      const { timeKeeper } = this.state;

      const wrapSetValueFn = setTimeFunction => {
        return value => {
          setTimeFunction(value);
          this._setComponentStates();
          this._dispatchState();
        };
      };
      const options = {
        hour: "numeric",
        minute: "numeric",
        hour12: format == "12",
      };
      if (showSeconds) {
        options.second = "numeric";
      }
      if (showMilliseconds) {
        options.fractionalSecondDigits = 3;
      }
      const dateTimeFormat = new Intl.DateTimeFormat(locale, options);
      const getPartValue = (date, type) =>
        dateTimeFormat.formatToParts(date).find(f => f.type == type).value;

      this.components = {};

      for (const timePart of dateTimeFormat.formatToParts(new Date(0))) {
        switch (timePart.type) {
          case "second":
            this.components.second = new Spinner(
              {
                setValue: wrapSetValueFn(value => {
                  timeKeeper.setSecond(value);
                  this.state.isSecondSet = true;
                }),
                getDisplayString: second =>
                  getPartValue(new Date(0).setSeconds(second), "second"),
              },
              this.context
            );
            break;
          case "fractionalSecond":
            this.components.millisecond = new Spinner(
              {
                setValue: wrapSetValueFn(value => {
                  timeKeeper.setMillisecond(value);
                  this.state.isMillisecondSet = true;
                }),
                getDisplayString: millisecond =>
                  getPartValue(
                    new Date(0).setMilliseconds(millisecond),
                    "fractionalSecond"
                  ),
              },
              this.context
            );
            break;
          case "minute":
            this.components.minute = new Spinner(
              {
                setValue: wrapSetValueFn(value => {
                  timeKeeper.setMinute(value);
                  this.state.isMinuteSet = true;
                }),
                getDisplayString: minute =>
                  getPartValue(new Date(0).setMinutes(minute), "minute"),
              },
              this.context
            );
            break;
          case "hour":
            this.components.hour = new Spinner(
              {
                setValue: wrapSetValueFn(value => {
                  timeKeeper.setHour(value);
                  this.state.isHourSet = true;
                }),
                getDisplayString: hour =>
                  getPartValue(new Date(0).setHours(hour), "hour"),
              },
              this.context
            );
            break;
          case "dayPeriod":
            this.components.dayPeriod = new Spinner(
              {
                setValue: wrapSetValueFn(value => {
                  timeKeeper.setDayPeriod(value);
                  this.state.isDayPeriodSet = true;
                }),
                getDisplayString: dayPeriod =>
                  getPartValue(new Date(0).setHours(dayPeriod), "dayPeriod"),
                hideButtons: true,
              },
              this.context
            );
            break;
          case "literal":
            if (timePart.value == " ") {
              this._insertLayoutElement({
                tag: "div",
                className: "spacer",
              });
              break;
            }
            this._insertLayoutElement({
              tag: "div",
              textContent: timePart.value,
              className: "colon",
            });
            break;
        }
      }
      this._updateButtonIds();
    },

    _insertLayoutElement({ tag, className, textContent }) {
      let el = document.createElement(tag);
      el.textContent = textContent;
      el.className = className;
      this.context.appendChild(el);
    },

    _setComponentStates() {
      const {
        timeKeeper,
        isHourSet,
        isMinuteSet,
        isSecondSet,
        isMillisecondSet,
        isDayPeriodSet,
      } = this.state;
      const isInvalid = timeKeeper.state.isInvalid;

      this.components.hour.setState({
        value: timeKeeper.hour,
        items: timeKeeper.ranges.hours,
        isInfiniteScroll: true,
        isValueSet: isHourSet,
        isInvalid,
      });

      this.components.minute.setState({
        value: timeKeeper.minute,
        items: timeKeeper.ranges.minutes,
        isInfiniteScroll: true,
        isValueSet: isMinuteSet,
        isInvalid,
      });

      this.components.second?.setState({
        value: timeKeeper.second,
        items: timeKeeper.ranges.seconds,
        isInfiniteScroll: true,
        isValueSet: isSecondSet,
        isInvalid,
      });

      this.components.millisecond?.setState({
        value: timeKeeper.millisecond,
        items: timeKeeper.ranges.milliseconds,
        isInfiniteScroll: true,
        isValueSet: isMillisecondSet,
        isInvalid,
      });

      this.components.dayPeriod?.setState({
        value: timeKeeper.dayPeriod,
        items: timeKeeper.ranges.dayPeriod,
        isInfiniteScroll: false,
        isValueSet: isDayPeriodSet,
        isInvalid,
      });
    },

    _dispatchState() {
      const { hour, minute, second, millisecond } = this.state.timeKeeper;
      const {
        isHourSet,
        isMinuteSet,
        isSecondSet,
        isMillisecondSet,
        isDayPeriodSet,
      } = this.state;
      window.postMessage(
        {
          name: "PickerPopupChanged",
          detail: {
            hour,
            minute,
            second,
            millisecond,
            isHourSet,
            isMinuteSet,
            isSecondSet,
            isMillisecondSet,
            isDayPeriodSet,
          },
        },
        "*"
      );
    },

    _closePopup() {
      window.postMessage(
        {
          name: "ClosePopup",
        },
        "*"
      );
    },
    _attachEventListeners() {
      window.addEventListener("message", this);
      document.addEventListener("mousedown", this);
      document.addEventListener("keydown", this);
    },

    focusNextSpinner(isReverse) {
      let focusedSpinner = document.activeElement;
      let spinners =
        focusedSpinner.parentNode.parentNode.querySelectorAll(".spinner");
      spinners = [...spinners];

      let next = isReverse
        ? spinners[spinners.indexOf(focusedSpinner) - 1]
        : spinners[spinners.indexOf(focusedSpinner) + 1];

      next?.focus();
    },

    handleEvent(event) {
      switch (event.type) {
        case "message": {
          this.handleMessage(event);
          break;
        }
        case "mousedown": {
          event.preventDefault();
          event.target.setPointerCapture(event.pointerId);
          break;
        }
        case "keydown": {
          if (
            this.context.parentNode.id == "datetime-picker" &&
            !event.target.closest("#time-picker")
          ) {
            break;
          }
          switch (event.key) {
            case "Enter":
            case " ": {
              event.stopPropagation();
              event.preventDefault();
              this._dispatchState();
              this._closePopup();
              break;
            }
            case "Escape": {
              event.stopPropagation();
              event.preventDefault();
              this._closePopup();
              break;
            }
            case "ArrowLeft":
            case "ArrowRight": {
              const isReverse = event.key == "ArrowLeft";
              this.focusNextSpinner(isReverse);
              break;
            }
          }
          break;
        }
      }
    },

    handleMessage(event) {
      switch (event.data.name) {
        case "PickerInit": {
          this.init(event.data.detail);
          break;
        }
        case "PickerPopupChanged": {
          if (this.props?.type != "datetime-local") {
            break;
          }
          if (
            event.data.detail?.year === undefined ||
            event.data.detail?.month === undefined ||
            event.data.detail?.day === undefined
          ) {
            break;
          }
          this.state.timeKeeper?.setState({
            year: event.data.detail.year,
            month: event.data.detail.month,
            day: event.data.detail.day,
          });
          this._setComponentStates();
          break;
        }
      }
    },

    _updateButtonIds() {
      let buttons = [
        [
          this.components.hour.elements.prev,
          "spinner-hour-previous",
          "time-spinner-hour-previous",
        ],
        [
          this.components.hour.elements.spinner,
          "spinner-hour",
          "time-spinner-hour-label",
        ],
        [
          this.components.hour.elements.next,
          "spinner-hour-next",
          "time-spinner-hour-next",
        ],
        [
          this.components.minute.elements.prev,
          "spinner-minute-previous",
          "time-spinner-minute-previous",
        ],
        [
          this.components.minute.elements.spinner,
          "spinner-minute",
          "time-spinner-minute-label",
        ],
        [
          this.components.minute.elements.next,
          "spinner-minute-next",
          "time-spinner-minute-next",
        ],
      ];

      if (this.components.second) {
        buttons = [
          ...buttons,
          [
            this.components.second.elements.prev,
            "spinner-second-previous",
            "time-spinner-second-previous",
          ],
          [
            this.components.second.elements.spinner,
            "spinner-second",
            "time-spinner-second-label",
          ],
          [
            this.components.second.elements.next,
            "spinner-second-next",
            "time-spinner-second-next",
          ],
        ];
      }

      if (this.components.millisecond) {
        buttons = [
          ...buttons,
          [
            this.components.millisecond.elements.prev,
            "spinner-millisecond-previous",
            "time-spinner-millisecond-previous",
          ],
          [
            this.components.millisecond.elements.spinner,
            "spinner-millisecond",
            "time-spinner-millisecond-label",
          ],
          [
            this.components.millisecond.elements.next,
            "spinner-millisecond-next",
            "time-spinner-millisecond-next",
          ],
        ];
      }

      if (this.components.dayPeriod) {
        buttons = [
          ...buttons,
          [
            this.components.dayPeriod.elements.prev,
            "spinner-time-previous",
            "time-spinner-day-period-previous",
          ],
          [
            this.components.dayPeriod.elements.spinner,
            "spinner-time",
            "time-spinner-day-period-label",
          ],
          [
            this.components.dayPeriod.elements.next,
            "spinner-time-next",
            "time-spinner-day-period-next",
          ],
        ];
      }

      for (const [btn, id, l10nId] of buttons) {
        btn.setAttribute("id", id);
        document.l10n.setAttributes(btn, l10nId);
      }
    },
  };
}

document.addEventListener("DOMContentLoaded", () => {
  new TimePicker(document.getElementById("time-picker"));
});
