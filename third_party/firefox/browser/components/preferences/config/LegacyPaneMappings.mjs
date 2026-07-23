/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

export const LEGACY_PANE_MAPPINGS = new Map([
  ["privacy-permissions", { category: "permissionsData" }],

  ["privacy-sitedata", { category: "privacy", subcategory: "sitedata" }],
  ["privacy-vpn", { category: "privacy", subcategory: "vpn" }],
  [
    "privacy-trackingprotection",
    { category: "privacy", subcategory: "etpStatus" },
  ],
  ["privacy-doh", { category: "privacy", subcategory: "dnsOverHttps" }],
  ["search-firefoxSuggest", { category: "search", subcategory: "locationBar" }],

  ["general", { category: "sync" }],
  ["general-layout", { category: "appearance", subcategory: "layout" }],
  [
    "general-update-box-group",
    { category: "about", subcategory: "update-box-group" },
  ],
  ["general-update-state", { category: "about", subcategory: "update-state" }],

  ["general-cfraddons", { category: "tabsBrowsing", subcategory: "cfraddons" }],
  [
    "general-cfrfeatures",
    { category: "tabsBrowsing", subcategory: "cfrfeatures" },
  ],
  ["general-layout", { category: "tabsBrowsing", subcategory: "layout" }],

  ["general-migrate", { category: "sync", subcategory: "migrate" }],
  [
    "general-migrate-autoclose",
    { category: "sync", subcategory: "migrate-autoclose" },
  ],

  ["general-drm", { category: "tabsBrowsing", subcategory: "drm" }],

  ["general-netsettings", { category: "privacy", subcategory: "netsettings" }],

  [
    "privacy-permissions-block-popups",
    { category: "permissionsData", subcategory: "permissions-block-popups" },
  ],
  ["privacy-reports", { category: "permissionsData", subcategory: "reports" }],
  ["privacy-privacy-segmentation", { category: "privacy" }],

  ["privacy-logins", { category: "passwordsAutofill", subcategory: "logins" }],
  [
    "privacy-payment-methods-autofill",
    {
      category: "passwordsAutofill",
      subcategory: "payment-methods-autofill",
    },
  ],
  [
    "privacy-credit-card-autofill",
    { category: "passwordsAutofill", subcategory: "credit-card-autofill" },
  ],
  [
    "privacy-addresses-autofill",
    { category: "passwordsAutofill", subcategory: "addresses-autofill" },
  ],
  [
    "privacy-address-autofill",
    { category: "passwordsAutofill", subcategory: "address-autofill" },
  ],
  ["privacy-logins", { category: "passwordsAutofill", subcategory: "logins" }],
]);

export function resolveLegacyCategory(category, subcategory) {
  if (/^pane[A-Z]/.test(category)) {
    category = category[4].toLowerCase() + category.slice(5);
  }
  let key = subcategory ? `${category}-${subcategory}` : category;
  let dest =
    LEGACY_PANE_MAPPINGS.get(key) ?? LEGACY_PANE_MAPPINGS.get(category);
  if (!dest) {
    return { category, subcategory: subcategory ?? null };
  }
  return { category: dest.category, subcategory: dest.subcategory ?? null };
}
