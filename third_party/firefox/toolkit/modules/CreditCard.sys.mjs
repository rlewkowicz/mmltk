/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { DateNormalizationUtils } from "resource://gre/modules/DateNormalizationUtils.sys.mjs";

const SUPPORTED_NETWORKS = Object.freeze([
  "amex",
  "cartebancaire",
  "diners",
  "discover",
  "jcb",
  "mastercard",
  "mir",
  "unionpay",
  "visa",
]);

export const NETWORK_NAMES = {
  "american express": "amex",
  "master card": "mastercard",
  "union pay": "unionpay",
};

const CREDIT_CARD_IIN = [
  { type: "amex", start: 34, end: 34, len: 15 },
  { type: "amex", start: 37, end: 37, len: 15 },
  { type: "cartebancaire", start: 4035, end: 4035, len: 16 },
  { type: "cartebancaire", start: 4360, end: 4360, len: 16 },
  { type: "diners", start: 300, end: 305, len: [14, 19] },
  { type: "diners", start: 3095, end: 3095, len: [14, 19] },
  { type: "diners", start: 36, end: 36, len: [14, 19] },
  { type: "diners", start: 38, end: 39, len: [14, 19] },
  { type: "discover", start: 6011, end: 6011, len: [16, 19] },
  { type: "discover", start: 644, end: 649, len: [16, 19] },
  { type: "discover", start: 65, end: 65, len: [16, 19] },
  { type: "jcb", start: 3528, end: 3589, len: [16, 19] },
  { type: "mastercard", start: 2221, end: 2720, len: 16 },
  { type: "mastercard", start: 51, end: 55, len: 16 },
  { type: "mir", start: 2200, end: 2204, len: [16, 19] },
  { type: "unionpay", start: 62, end: 62, len: [16, 19] },
  { type: "unionpay", start: 81, end: 81, len: [16, 19] },
  { type: "visa", start: 4, end: 4, len: [13, 19] },
].sort((a, b) => b.start - a.start);

export class CreditCard {
  constructor({
    name,
    number,
    expirationString,
    expirationMonth,
    expirationYear,
    network,
    ccv,
    encryptedNumber,
  }) {
    this._name = name;
    this._unmodifiedNumber = number;
    this._encryptedNumber = encryptedNumber;
    this._ccv = ccv;
    this.number = number;
    let { month, year } = CreditCard.normalizeExpiration({
      expirationString,
      expirationMonth,
      expirationYear,
    });
    this._expirationMonth = month;
    this._expirationYear = year;
    this.network = network;
  }

  set name(value) {
    this._name = value;
  }

  set expirationMonth(value) {
    if (typeof value == "undefined") {
      this._expirationMonth = undefined;
      return;
    }
    this._expirationMonth = CreditCard.normalizeExpirationMonth(value);
  }

  get expirationMonth() {
    return this._expirationMonth;
  }

  set expirationYear(value) {
    if (typeof value == "undefined") {
      this._expirationYear = undefined;
      return;
    }
    this._expirationYear = CreditCard.normalizeExpirationYear(value);
  }

  get expirationYear() {
    return this._expirationYear;
  }

  set expirationString(value) {
    let { month, year } = DateNormalizationUtils.parseMonthYearString(value);
    this.expirationMonth = month;
    this.expirationYear = year;
  }

  set ccv(value) {
    this._ccv = value;
  }

  get number() {
    return this._number;
  }

  set number(value) {
    if (value) {
      let normalizedNumber = CreditCard.normalizeCardNumber(value);
      normalizedNumber = normalizedNumber.match(/^\d{12,}$/)
        ? normalizedNumber
        : "";
      this._number = normalizedNumber;
    } else {
      this._number = "";
    }

    if (value && !this.isValidNumber()) {
      this._number = "";
      throw new Error("Invalid credit card number");
    }
  }

  get network() {
    return this._network;
  }

  set network(value) {
    this._network = value || undefined;
  }

  isValidNumber() {
    if (!this._number) {
      return false;
    }

    const number = CreditCard.normalizeCardNumber(this._number);

    const len = number.length;
    if (len < 12 || len > 19) {
      return false;
    }

    if (!/^\d+$/.test(number)) {
      return false;
    }

    let total = 0;
    for (let i = 0; i < len; i++) {
      let ch = parseInt(number[len - i - 1], 10);
      if (i % 2 == 1) {
        ch *= 2;
        if (ch > 9) {
          ch -= 9;
        }
      }
      total += ch;
    }
    return total % 10 == 0;
  }

  static normalizeCardNumber(number) {
    if (!number) {
      return null;
    }
    return number.replace(/[\-\s]/g, "");
  }

  static getType(ccNumber) {
    if (!ccNumber) {
      return null;
    }

    for (let i = 0; i < CREDIT_CARD_IIN.length; i++) {
      const range = CREDIT_CARD_IIN[i];
      if (typeof range.len == "number") {
        if (range.len != ccNumber.length) {
          continue;
        }
      } else if (
        ccNumber.length < range.len[0] ||
        ccNumber.length > range.len[1]
      ) {
        continue;
      }

      const prefixLength = Math.floor(Math.log10(range.start)) + 1;
      const prefix = parseInt(ccNumber.substring(0, prefixLength), 10);
      if (prefix >= range.start && prefix <= range.end) {
        return range.type;
      }
    }
    return null;
  }

  static getNetworkFromName(name) {
    if (!name) {
      return null;
    }
    let lcName = name.trim().toLowerCase().normalize("NFKC");
    if (SUPPORTED_NETWORKS.includes(lcName)) {
      return lcName;
    }
    for (let term in NETWORK_NAMES) {
      if (lcName.includes(term)) {
        return NETWORK_NAMES[term];
      }
    }
    return null;
  }

  isValid() {
    if (!this.isValidNumber()) {
      return false;
    }

    let currentDate = new Date();
    let currentYear = currentDate.getFullYear();
    if (this._expirationYear > currentYear) {
      return true;
    }

    let currentMonth = currentDate.getMonth() + 1;
    return (
      this._expirationYear == currentYear &&
      this._expirationMonth >= currentMonth
    );
  }

  get maskedNumber() {
    return CreditCard.getMaskedNumber(this._number);
  }

  get longMaskedNumber() {
    return CreditCard.getLongMaskedNumber(this._number);
  }

  static getLabelInfo({ number, name, month, year, type }) {
    let formatSelector = ["number"];
    if (name) {
      formatSelector.push("name");
    }
    if (month && year) {
      formatSelector.push("expiration");
    }
    let stringId = `credit-card-label-${formatSelector.join("-")}-2`;
    return {
      id: stringId,
      args: {
        number: CreditCard.getMaskedNumber(number),
        name,
        month: month?.toString(),
        year: year?.toString(),
        type,
      },
    };
  }

  static getLabel({ number, name }) {
    let parts = [];

    if (number) {
      parts.push(CreditCard.getMaskedNumber(number));
    }
    if (name) {
      parts.push(name);
    }
    return parts.join(", ");
  }

  static normalizeExpirationMonth(month) {
    return DateNormalizationUtils.normalizeMonth(month);
  }

  static normalizeExpirationYear(year) {
    return DateNormalizationUtils.normalizeYear(year);
  }

  static normalizeExpiration({
    expirationString,
    expirationMonth,
    expirationYear,
  }) {
    return DateNormalizationUtils.normalizeComponents({
      string: expirationString,
      month: expirationMonth,
      year: expirationYear,
      parts: ["month", "year"],
    });
  }

  static formatMaskedNumber(maskedNumber) {
    return "•".repeat(4) + maskedNumber.substr(-4);
  }

  static getMaskedNumber(number) {
    return "•".repeat(4) + " " + number.substr(-4);
  }

  static getLongMaskedNumber(number) {
    return "•".repeat(number.length - 4) + number.substr(-4);
  }

  static getCreditCardLogo(network) {
    const PATH = "chrome://formautofill/content/";
    const THIRD_PARTY_PATH = PATH + "third-party/";
    switch (network) {
      case "amex":
        return THIRD_PARTY_PATH + "cc-logo-amex.png";
      case "cartebancaire":
        return THIRD_PARTY_PATH + "cc-logo-cartebancaire.png";
      case "diners":
        return THIRD_PARTY_PATH + "cc-logo-diners.svg";
      case "discover":
        return THIRD_PARTY_PATH + "cc-logo-discover.png";
      case "jcb":
        return THIRD_PARTY_PATH + "cc-logo-jcb.svg";
      case "mastercard":
        return THIRD_PARTY_PATH + "cc-logo-mastercard.svg";
      case "mir":
        return THIRD_PARTY_PATH + "cc-logo-mir.svg";
      case "unionpay":
        return THIRD_PARTY_PATH + "cc-logo-unionpay.svg";
      case "visa":
        return THIRD_PARTY_PATH + "cc-logo-visa.svg";
      default:
        return PATH + "icon-credit-card-generic.svg";
    }
  }

  static isValidNumber(number) {
    try {
      new CreditCard({ number });
    } catch (ex) {
      return false;
    }
    return true;
  }

  static isValidNetwork(network) {
    return SUPPORTED_NETWORKS.includes(network);
  }

  static getSupportedNetworks() {
    return SUPPORTED_NETWORKS;
  }

  static getNetworkL10nId(network) {
    return this.isValidNetwork(network)
      ? `autofill-card-network-${network}`
      : null;
  }
}
