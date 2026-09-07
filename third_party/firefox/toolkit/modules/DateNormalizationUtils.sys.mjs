/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export var DateNormalizationUtils = {
  normalizeMonth(value) {
    value = parseInt(value, 10);
    return isNaN(value) || value < 1 || value > 12 ? undefined : value;
  },

  normalizeDay(value) {
    value = parseInt(value, 10);
    return isNaN(value) || value < 1 || value > 31 ? undefined : value;
  },

  normalizeYear(value) {
    value = parseInt(value, 10);
    if (isNaN(value) || value < 1) {
      return undefined;
    }
    if (value < 100) {
      value += 2000;
    }
    return value;
  },

  parseISODate(dateString) {
    const match = /^(\d{4})-(\d{1,2})-(\d{1,2})$/.exec(
      String(dateString).trim()
    );
    if (!match) {
      return {};
    }
    return { year: match[1], month: match[2], day: match[3] };
  },

  parseMonthYearString(dateString) {
    let rules = [
      {
        regex: /(?:^|\D)(\d{2})(\d{2})(?!\d)/,
      },
      {
        regex: /(?:^|\D)(\d{4})[-/](\d{1,2})(?!\d)/,
        yearIndex: 0,
        monthIndex: 1,
      },
      {
        regex: /(?:^|\D)(\d{1,2})[-/](\d{4})(?!\d)/,
        yearIndex: 1,
        monthIndex: 0,
      },
      {
        regex: /(?:^|\D)(\d{1,2})[-/](\d{1,2})(?!\d)/,
      },
      {
        regex: /(?:^|\D)(\d{2})(\d{2})(?!\d)/,
      },
    ];

    dateString = dateString.replaceAll(" ", "");
    for (let rule of rules) {
      let result = rule.regex.exec(dateString);
      if (!result) {
        continue;
      }

      let year, month;
      const parsedResults = [parseInt(result[1], 10), parseInt(result[2], 10)];
      if (!rule.yearIndex || !rule.monthIndex) {
        month = parsedResults[0];
        if (month > 12) {
          year = parsedResults[0];
          month = parsedResults[1];
        } else {
          year = parsedResults[1];
        }
      } else {
        year = parsedResults[rule.yearIndex];
        month = parsedResults[rule.monthIndex];
      }

      if (month >= 1 && month <= 12 && (year < 100 || year > 2000)) {
        return { month, year };
      }
    }
    return { month: undefined, year: undefined };
  },

  formatISODate({ year, month, day }) {
    if (year && month && day) {
      return (
        String(year) +
        "-" +
        String(month).padStart(2, "0") +
        "-" +
        String(day).padStart(2, "0")
      );
    }
    return "";
  },

  normalizeComponents({ string, month, day, year, parts }) {
    const components = { month, day, year };

    const parse = parts.includes("day")
      ? this.parseISODate
      : this.parseMonthYearString;

    const missing = parts.some(part => !components[part]);
    const parsed = string && missing ? parse(string) : {};

    const result = {};
    for (const part of parts) {
      const value = parsed[part] || components[part];
      if (part == "month") {
        result.month = this.normalizeMonth(value);
      } else if (part == "day") {
        result.day = this.normalizeDay(value);
      } else {
        result.year = this.normalizeYear(value);
      }
    }
    return result;
  },
};
