/**
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "IDNService",
  "@mozilla.org/network/idn-service;1",
  Ci.nsIIDNService
);

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "SELECT_FIRST_IN_UI_LISTS",
  "dom.security.credentialmanagement.identity.select_first_in_ui_lists",
  false
);

ChromeUtils.defineESModuleGetters(lazy, {
  GeckoViewIdentityCredential:
    "resource://gre/modules/GeckoViewIdentityCredential.sys.mjs",
});
const BEST_HEADER_ICON_SIZE = 16;
const BEST_ICON_SIZE = 32;

function fulfilledPromiseFromFirstListElement(list) {
  if (list.length) {
    return Promise.resolve(0);
  }
  return Promise.reject();
}

function blobToDataUrl(blob) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.addEventListener("loadend", function () {
      if (reader.error) {
        reject(reader.error);
      }
      resolve(reader.result);
    });
    reader.readAsDataURL(blob);
  });
}

async function fetchToDataUrl(url) {
  let result = await fetch(url);
  if (!result.ok) {
    throw result.status;
  }
  let blob = await result.blob();
  let data = blobToDataUrl(blob);
  return data;
}

export class IdentityCredentialPromptService {
  classID = Components.ID("{936007db-a957-4f1d-a23d-f7d9403223e6}");
  QueryInterface = ChromeUtils.generateQI([
    "nsIIdentityCredentialPromptService",
  ]);

  async loadIconFromManifest(
    providerManifest,
    bestIconSize = BEST_ICON_SIZE,
    defaultIcon = null
  ) {
    if (providerManifest?.branding?.icons?.length) {
      let iconsArray = providerManifest.branding.icons;
      let vectorIcon = iconsArray.find(icon => !icon.size);
      if (vectorIcon) {
        return fetchToDataUrl(vectorIcon.url);
      }
      let exactIcon = iconsArray.find(icon => icon.size == bestIconSize);
      if (exactIcon) {
        return fetchToDataUrl(exactIcon.url);
      }
      let biggestIcon = iconsArray.sort(
        (iconA, iconB) => iconB.size - iconA.size
      )[0];
      if (biggestIcon) {
        return fetchToDataUrl(biggestIcon.url);
      }
    }

    return defaultIcon;
  }

  async showProviderPrompt(
    browsingContext,
    identityProviders,
    identityManifests
  ) {
    if (lazy.SELECT_FIRST_IN_UI_LISTS) {
      return fulfilledPromiseFromFirstListElement(identityProviders);
    }
    let browser = browsingContext.top.embedderElement;
    if (!browser) {
      throw new Error("Null browser provided");
    }

    if (identityProviders.length != identityManifests.length) {
      throw new Error("Mismatch argument array length");
    }

    let promises = identityManifests.map(async providerManifest => {
      const iconResult = await this.loadIconFromManifest(providerManifest);
      return iconResult ? iconResult : Promise.reject();
    });

    const providerNames = identityManifests.map(
      providerManifest => providerManifest?.branding?.name
    );

    if (promises.length != identityManifests.length) {
      throw new Error("Mismatch promise array length");
    }

    let iconResults = await Promise.allSettled(promises);
    if (AppConstants.platform === "android") {
      const providers = [];
      for (const [providerIndex, provider] of identityProviders.entries()) {
        let providerURL = new URL(provider.configURL);
        let displayDomain = lazy.IDNService.convertToDisplayIDN(
          providerURL.host
        );

        let iconResult = iconResults[providerIndex];
        const data = {
          id: providerIndex,
          icon: iconResult.value,
          name: providerNames[providerIndex],
          domain: displayDomain,
        };
        providers.push(data);
      }

      return new Promise((resolve, reject) => {
        lazy.GeckoViewIdentityCredential.onShowProviderPrompt(
          browsingContext,
          providers,
          resolve,
          reject
        );
      });
    }

    let localization = new Localization(
      ["browser/identityCredentialNotification.ftl"],
      true
    );
    let headerMessage = localization.formatValueSync(
      "identity-credential-header-providers"
    );
    let [accept, cancel] = localization.formatMessagesSync([
      { id: "identity-credential-accept-button" },
      { id: "identity-credential-cancel-button" },
    ]);

    let cancelLabel = cancel.attributes.find(x => x.name == "label").value;
    let cancelKey = cancel.attributes.find(x => x.name == "accesskey").value;
    let acceptLabel = accept.attributes.find(x => x.name == "label").value;
    let acceptKey = accept.attributes.find(x => x.name == "accesskey").value;

    let listBox = browser.ownerDocument.getElementById(
      "identity-credential-provider-selector-container"
    );
    while (listBox.firstChild) {
      listBox.removeChild(listBox.lastChild);
    }
    let itemTemplate = browser.ownerDocument.getElementById(
      "template-credential-provider-list-item"
    );
    for (const [providerIndex, provider] of identityProviders.entries()) {
      let providerURL = new URL(provider.configURL);
      let displayDomain = lazy.IDNService.convertToDisplayIDN(providerURL.host);
      let newItem = itemTemplate.content.firstElementChild.cloneNode(true);

      let newRadio = newItem.getElementsByClassName(
        "identity-credential-list-item-radio"
      )[0];
      newRadio.value = providerIndex;
      newRadio.addEventListener("change", function (event) {
        for (let item of listBox.children) {
          item.classList.remove("checked");
        }
        if (event.target.checked) {
          event.target.parentElement.classList.add("checked");
        }
      });
      if (providerIndex == 0) {
        newRadio.checked = true;
        newItem.classList.add("checked");
      }

      let iconResult = iconResults[providerIndex];
      if (iconResult.status == "fulfilled") {
        let newIcon = newItem.getElementsByClassName(
          "identity-credential-list-item-icon"
        )[0];
        newIcon.setAttribute("src", iconResult.value);
      }

      newItem.getElementsByClassName(
        "identity-credential-list-item-label-primary"
      )[0].textContent = providerNames[providerIndex] || displayDomain;
      newItem.getElementsByClassName(
        "identity-credential-list-item-label-secondary"
      )[0].hidden = true;

      if (providerNames[providerIndex] && displayDomain) {
        newItem.getElementsByClassName(
          "identity-credential-list-item-label-secondary"
        )[0].hidden = false;
        newItem.getElementsByClassName(
          "identity-credential-list-item-label-secondary"
        )[0].textContent = displayDomain;
      }

      listBox.append(newItem);
    }

    return new Promise((resolve, reject) => {
      let options = {
        hideClose: true,
        eventCallback: (topic, nextRemovalReason, isCancel) => {
          if (topic == "removed" && isCancel) {
            reject();
          }
        },
      };
      let mainAction = {
        label: acceptLabel,
        accessKey: acceptKey,
        callback(_event) {
          let result = listBox.querySelector(
            ".identity-credential-list-item-radio:checked"
          ).value;
          resolve(parseInt(result));
        },
      };
      let secondaryActions = [
        {
          label: cancelLabel,
          accessKey: cancelKey,
          callback(_event) {
            reject();
          },
        },
      ];

      browser.ownerDocument.getElementById(
        "identity-credential-provider"
      ).hidden = false;
      browser.ownerDocument.getElementById(
        "identity-credential-policy"
      ).hidden = true;
      browser.ownerDocument.getElementById(
        "identity-credential-account"
      ).hidden = true;
      browser.ownerDocument.getElementById(
        "identity-credential-header"
      ).hidden = true;
      browser.documentGlobal.PopupNotifications.show(
        browser,
        "identity-credential",
        headerMessage,
        "identity-credential-notification-icon",
        mainAction,
        secondaryActions,
        options
      );
    });
  }

  async showAccountListPrompt(
    browsingContext,
    accountList,
    provider,
    providerManifest
  ) {
    if (lazy.SELECT_FIRST_IN_UI_LISTS) {
      return fulfilledPromiseFromFirstListElement(accountList.accounts);
    }

    let browser = browsingContext.top.embedderElement;
    if (!browser) {
      throw new Error("Null browser provided");
    }

    let promises = accountList.accounts.map(async account => {
      if (!account?.picture) {
        throw new Error("Missing picture");
      }
      return fetchToDataUrl(account.picture);
    });

    if (promises.length != accountList.accounts.length) {
      throw new Error("Incorrect number of promises obtained");
    }

    let pictureResults = await Promise.allSettled(promises);

    let localization = new Localization(
      ["browser/identityCredentialNotification.ftl"],
      true
    );
    const providerName = providerManifest?.branding?.name;
    let providerURL = new URL(provider.configURL);
    let displayDomain = lazy.IDNService.convertToDisplayIDN(providerURL.host);

    let headerIconResult = await this.loadIconFromManifest(
      providerManifest,
      BEST_HEADER_ICON_SIZE,
      "chrome://global/skin/icons/defaultFavicon.svg"
    );

    if (AppConstants.platform === "android") {
      const accounts = [];

      for (const [accountIndex, account] of accountList.accounts.entries()) {
        var picture = "";
        let pictureResult = pictureResults[accountIndex];
        if (pictureResult.status == "fulfilled") {
          picture = pictureResult.value;
        }
        account.name;
        account.email;
        const data = {
          id: accountIndex,
          icon: picture,
          name: account.name,
          email: account.email,
        };
        accounts.push(data);
        console.log(data);
      }

      const provider = {
        name: providerName || displayDomain,
        domain: displayDomain,
        icon: headerIconResult,
      };

      const result = {
        provider,
        accounts,
      };

      return new Promise((resolve, reject) => {
        lazy.GeckoViewIdentityCredential.onShowAccountsPrompt(
          browsingContext,
          result,
          resolve,
          reject
        );
      });
    }

    let headerMessage = localization.formatValueSync(
      "identity-credential-header-accounts",
      {
        provider: providerName || displayDomain,
      }
    );

    let [accept, cancel] = localization.formatMessagesSync([
      { id: "identity-credential-sign-in-button" },
      { id: "identity-credential-cancel-button" },
    ]);

    let cancelLabel = cancel.attributes.find(x => x.name == "label").value;
    let cancelKey = cancel.attributes.find(x => x.name == "accesskey").value;
    let acceptLabel = accept.attributes.find(x => x.name == "label").value;
    let acceptKey = accept.attributes.find(x => x.name == "accesskey").value;

    let listBox = browser.ownerDocument.getElementById(
      "identity-credential-account-selector-container"
    );
    while (listBox.firstChild) {
      listBox.removeChild(listBox.lastChild);
    }
    let itemTemplate = browser.ownerDocument.getElementById(
      "template-credential-account-list-item"
    );
    for (const [accountIndex, account] of accountList.accounts.entries()) {
      let newItem = itemTemplate.content.firstElementChild.cloneNode(true);

      let newRadio = newItem.getElementsByClassName(
        "identity-credential-list-item-radio"
      )[0];
      newRadio.value = accountIndex;
      newRadio.addEventListener("change", function (event) {
        for (let item of listBox.children) {
          item.classList.remove("checked");
        }
        if (event.target.checked) {
          event.target.parentElement.classList.add("checked");
        }
      });
      if (accountIndex == 0) {
        newRadio.checked = true;
        newItem.classList.add("checked");
      }

      let pictureResult = pictureResults[accountIndex];
      if (pictureResult.status == "fulfilled") {
        let newPicture = newItem.getElementsByClassName(
          "identity-credential-list-item-icon"
        )[0];
        newPicture.setAttribute("src", pictureResult.value);
      }

      const labels = [
        account.name,
        account.email,
        account.username,
        account.tel,
      ].filter(label => label != undefined && label != "");

      const primaryLabel = labels[0];
      if (primaryLabel) {
        newItem.getElementsByClassName(
          "identity-credential-list-item-label-primary"
        )[0].textContent = primaryLabel;
      }

      const secondaryLabel = labels[1];
      if (secondaryLabel) {
        newItem.getElementsByClassName(
          "identity-credential-list-item-label-secondary"
        )[0].textContent = secondaryLabel;
      }

      listBox.append(newItem);
    }

    return new Promise(function (resolve, reject) {
      let options = {
        hideClose: true,
        eventCallback: (topic, nextRemovalReason, isCancel) => {
          if (topic == "removed" && isCancel) {
            reject();
          }
        },
      };
      let mainAction = {
        label: acceptLabel,
        accessKey: acceptKey,
        callback(_event) {
          let result = listBox.querySelector(
            ".identity-credential-list-item-radio:checked"
          ).value;
          resolve(parseInt(result));
        },
      };
      let secondaryActions = [
        {
          label: cancelLabel,
          accessKey: cancelKey,
          callback(_event) {
            reject();
          },
        },
      ];

      if (headerIconResult) {
        let headerIcon = browser.ownerDocument.getElementsByClassName(
          "identity-credential-header-icon"
        )[0];
        headerIcon.setAttribute("src", headerIconResult);
      }

      const headerText = browser.ownerDocument.getElementById(
        "identity-credential-header-text"
      );
      headerText.textContent = headerMessage;

      browser.ownerDocument.getElementById(
        "identity-credential-provider"
      ).hidden = true;
      browser.ownerDocument.getElementById(
        "identity-credential-policy"
      ).hidden = true;
      browser.ownerDocument.getElementById(
        "identity-credential-account"
      ).hidden = false;
      browser.ownerDocument.getElementById(
        "identity-credential-header"
      ).hidden = false;
      browser.documentGlobal.PopupNotifications.show(
        browser,
        "identity-credential",
        "",
        "identity-credential-notification-icon",
        mainAction,
        secondaryActions,
        options
      );
    });
  }

  close(browsingContext) {
    let browser = browsingContext.top.embedderElement;
    if (!browser || AppConstants.platform === "android") {
      return;
    }
    let notification =
      browser.documentGlobal.PopupNotifications.getNotification(
        "identity-credential",
        browser
      );
    if (notification) {
      browser.documentGlobal.PopupNotifications.remove(
        notification,
         true
      );
    }
  }
}
