/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { html } from "chrome://global/content/vendor/lit.all.mjs";
import { MozLitElement } from "chrome://global/content/lit-utils.mjs";
import { SettingPaneManager } from "chrome://browser/content/preferences/config/SettingPaneManager.mjs";
import { SettingGroupManager } from "chrome://browser/content/preferences/config/SettingGroupManager.mjs";


function shouldGoBackToParent(win, parentCategory) {
  if (win.history.state?.previousCategory !== parentCategory) {
    return false;
  }
  return win.navigation?.canGoBack ?? true;
}


export class SettingPane extends MozLitElement {
  static properties = {
    name: { type: String },
    isSubPane: { type: Boolean },
    config: { type: Object },
    showRedesignPromo: { type: Boolean, attribute: false },
    onSearchPane: { type: Boolean, reflect: true },
    initialized: { type: Boolean, state: true },
  };

  get pageHeaderEl() {
    return this.renderRoot.querySelector("moz-page-header");
  }

  get paneId() {
    return this.config.id;
  }

  constructor() {
    super();
    this.name = undefined;
    this.isSubPane = false;
    this.config = undefined;
    this.showRedesignPromo = false;
    this.onSearchPane = false;
    this.initialized = false;
  }

  createRenderRoot() {
    return this;
  }

  async getUpdateComplete() {
    let result = await super.getUpdateComplete();
    await this.pageHeaderEl.updateComplete;
    return result;
  }

  goBack() {
    if (shouldGoBackToParent(window, this.config.parent)) {
      window.history.back();
      return;
    }
    window.gotoPref(this.config.parent);
  }

  handleVisibility() {
    if (this.config.visible) {
      let visible = this.config.visible();
      let categoryButton =  (
        document.querySelector(
          `#categories moz-page-nav-button[view="${this.name}"]`
        )
      );
      if (!visible && !this.isSubPane) {
        if (categoryButton) {
          categoryButton.remove();
        }
        this.remove();
      } else if (visible && categoryButton) {
        categoryButton.hidden = false;
      }
    }
  }

  #onAnySettingsRedesignPromoDismissClick = () => {
    this.showRedesignPromo = false;
  };

  connectedCallback() {
    super.connectedCallback();

    this.handleVisibility();

    document.addEventListener("paneshown", this.handlePaneShown);

    document.addEventListener(
      "settings-redesign-promo-dismiss",
      this.#onAnySettingsRedesignPromoDismissClick
    );

    this.setAttribute("data-category", this.name);
    this.hidden = true;
    if (this.isSubPane) {
      this.setAttribute("data-hidden-from-search", "true");
      this.setAttribute("data-subpanel", "true");
      this._createCategoryButton();
    }
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    document.removeEventListener("paneshown", this.handlePaneShown);
    document.removeEventListener(
      "settings-redesign-promo-dismiss",
      this.#onAnySettingsRedesignPromoDismissClick
    );
  }

  handlePaneShown = e => {
    if (
      this.isSubPane &&
      e.detail.category === this.name &&
      !this.contains(document.activeElement)
    ) {
      this.pageHeaderEl.backButtonEl.focus({ preventScroll: true });
    }
  };

  init() {
    if (!this.initialized) {
      this.initialized = true;
      this.performUpdate();
    }

    SettingPaneManager.importPane(this.paneId);

    Services.obs.notifyObservers(
       (window),
      `${this.config.id}-pane-loaded`
    );

    for (let groupId of this.config.groupIds) {
      if (SettingGroupManager.has(groupId)) {
        window.initSettingGroup(groupId);
      }
    }
  }

  _createCategoryButton() {
    let categoryButton = document.createElement("moz-page-nav-button");
    if (this.isSubPane) {
      categoryButton.classList.add("hidden-category");
    }
    categoryButton.setAttribute("view", this.name);
    document.getElementById("categories").append(categoryButton);
  }

  groupTemplate(groupId) {
    return html`<setting-group
      groupid=${groupId}
      .inSubPane=${this.isSubPane}
    ></setting-group>`;
  }

  breadcrumbsTemplate() {
    if (!this.isSubPane) {
      return "";
    }
    return html`<moz-breadcrumb-group slot="breadcrumbs">
      ${SettingPaneManager.getWithParents(this.paneId).map(
        config =>
          html`<moz-breadcrumb
            data-l10n-id=${config.l10nId}
            .href=${"#" + config.id}
          ></moz-breadcrumb>`
      )}
    </moz-breadcrumb-group>`;
  }

  onDismiss() {
    const event = new CustomEvent("settings-redesign-promo-dismiss", {
      bubbles: true,
      composed: true,
    });
    this.dispatchEvent(event);
  }

  settingsRedesignPromoTemplate() {
    if (!this.showRedesignPromo || this.onSearchPane) {
      return "";
    }

    return html`<moz-promo
      data-l10n-id="settings-redesign-promo"
      class="settings-redesign-promo"
    >
      <moz-button
        slot="actions"
        data-l10n-id="settings-redesign-promo-dismiss-button"
        type="primary"
        @click=${this.onDismiss}
      ></moz-button>
    </moz-promo>`;
  }

  render() {
    if (!this.initialized) {
      return "";
    }
    return html`
      ${this.settingsRedesignPromoTemplate()}
      <section>
        <moz-page-header
          data-l10n-id=${this.config.l10nId}
          .iconSrc=${this.config.iconSrc}
          .supportPage=${this.config.supportPage}
          .badge=${this.config.badge}
          .backButton=${this.isSubPane}
          .headingLevel=${this.onSearchPane ? 3 : 2}
          @navigate-back=${this.goBack}
          >${this.breadcrumbsTemplate()}</moz-page-header
        >
        ${this.config.groupIds.map(groupId => this.groupTemplate(groupId))}
      </section>
    `;
  }
}
customElements.define("setting-pane", SettingPane);
