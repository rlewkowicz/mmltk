/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { html, ifDefined } from "chrome://global/content/vendor/lit.all.mjs";
import { MozLitElement } from "chrome://global/content/lit-utils.mjs";

// eslint-disable-next-line import/no-unassigned-import
import "chrome://global/content/elements/moz-message-bar.mjs";
// eslint-disable-next-line import/no-unassigned-import
import "chrome://browser/content/backup/password-validation-inputs.mjs";

import { ERRORS } from "chrome://browser/content/backup/backup-constants.mjs";

const VALID_TYPES = Object.freeze({
  SET_PASSWORD: "set-password",
  CHANGE_PASSWORD: "change-password",
});

const VALID_L10N_IDS = new Map([
  [VALID_TYPES.SET_PASSWORD, "enable-backup-encryption-header"],
  [VALID_TYPES.CHANGE_PASSWORD, "change-backup-encryption-header"],
]);

const ERROR_L10N_IDS = Object.freeze({
  [ERRORS.INVALID_PASSWORD]: "backup-error-password-requirements",
  [ERRORS.UNKNOWN]: "backup-error-retry",
});

function getErrorL10nId(errorCode) {
  return ERROR_L10N_IDS[errorCode] ?? ERROR_L10N_IDS[ERRORS.UNKNOWN];
}

export default class EnableBackupEncryption extends MozLitElement {
  static properties = {
    _inputPassValue: { type: String, state: true },
    _passwordsMatch: { type: Boolean, state: true },

    supportBaseLink: { type: String },
    type: { type: String, reflect: true },

    enableEncryptionErrorCode: { type: Number },
  };

  static get queries() {
    return {
      cancelButtonEl: "#backup-enable-encryption-cancel-button",
      confirmButtonEl: "#backup-enable-encryption-confirm-button",
      contentEl: "#backup-enable-encryption-content",
      textHeaderEl: "#backup-enable-encryption-header",
      textDescriptionEl: "#backup-enable-encryption-description",
      passwordInputsEl: "#backup-enable-encryption-password-inputs",
      errorEl: "#enable-backup-encryption-error",
    };
  }

  constructor() {
    super();
    this.supportBaseLink = "";
    this.type = VALID_TYPES.SET_PASSWORD;
    this._inputPassValue = "";
    this._passwordsMatch = false;
    this.enableEncryptionErrorCode = 0;
  }

  connectedCallback() {
    super.connectedCallback();
    this.addEventListener("ValidPasswordsDetected", this);
    this.addEventListener("InvalidPasswordsDetected", this);
  }

  handleEvent(event) {
    if (event.type == "ValidPasswordsDetected") {
      let { password } = event.detail;
      this._passwordsMatch = true;
      this._inputPassValue = password;
    } else if (event.type == "InvalidPasswordsDetected") {
      this._passwordsMatch = false;
      this._inputPassValue = "";
    }
  }

  close() {
    this.dispatchEvent(
      new CustomEvent("dialogCancel", {
        bubbles: true,
        composed: true,
      })
    );
  }

  reset() {
    this._inputPassValue = "";
    this._passwordsMatch = false;
    this.passwordInputsEl.reset();
    this.enableEncryptionErrorCode = 0;
  }

  handleConfirm() {
    this.dispatchEvent(
      new CustomEvent("BackupUI:EnableEncryption", {
        bubbles: true,
        detail: {
          password: this._inputPassValue,
        },
      })
    );
  }

  descriptionTemplate() {
    return html`
      <div id="backup-enable-encryption-description">
        <span
          id="backup-enable-encryption-description-span"
          data-l10n-id="settings-sensitive-data-encryption-description"
        >
        </span>
        <a
          id="backup-enable-encryption-learn-more-link"
          is="moz-support-link"
          support-page="firefox-backup"
          data-l10n-id="enable-backup-encryption-support-link"
          utm-content="add-password"
        ></a>
      </div>
    `;
  }

  buttonGroupTemplate() {
    return html`
      <moz-button-group id="backup-enable-encryption-button-group">
        <moz-button
          id="backup-enable-encryption-cancel-button"
          @click=${this.close}
          data-l10n-id="enable-backup-encryption-cancel-button"
        ></moz-button>
        <moz-button
          id="backup-enable-encryption-confirm-button"
          @click=${this.handleConfirm}
          type="primary"
          data-l10n-id="enable-backup-encryption-confirm-button"
          ?disabled=${!this._passwordsMatch}
        ></moz-button>
      </moz-button-group>
    `;
  }

  errorTemplate() {
    let messageId = getErrorL10nId(this.enableEncryptionErrorCode);
    return html`
      <moz-message-bar
        id="enable-backup-encryption-error"
        type="error"
        .messageL10nId=${messageId}
      ></moz-message-bar>
    `;
  }

  contentTemplate() {
    return html`
      <div
        id="backup-enable-encryption-wrapper"
        aria-labelledby="backup-enable-encryption-header"
        aria-describedby="backup-enable-encryption-description"
      >
        <h1
          id="backup-enable-encryption-header"
          class="heading-medium"
          data-l10n-id=${ifDefined(VALID_L10N_IDS.get(this.type))}
        ></h1>
        <div id="backup-enable-encryption-content">
          ${this.type === VALID_TYPES.SET_PASSWORD
            ? this.descriptionTemplate()
            : null}
          <password-validation-inputs
            id="backup-enable-encryption-password-inputs"
            .supportBaseLink=${this.supportBaseLink}
          >
          </password-validation-inputs>

          ${this.enableEncryptionErrorCode ? this.errorTemplate() : null}
        </div>
        ${this.buttonGroupTemplate()}
      </div>
    `;
  }

  render() {
    return html`
      <link
        rel="stylesheet"
        href="chrome://browser/content/backup/enable-backup-encryption.css"
      />
      ${this.contentTemplate()}
    `;
  }
}

customElements.define("enable-backup-encryption", EnableBackupEncryption);
