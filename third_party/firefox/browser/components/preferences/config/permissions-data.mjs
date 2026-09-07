/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import { SettingGroupManager } from "chrome://browser/content/preferences/config/SettingGroupManager.mjs";
import { Preferences } from "chrome://global/content/preferences/Preferences.mjs";

const XPCOMUtils = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
).XPCOMUtils;

const lazy = XPCOMUtils.declareLazy({
  AppConstants: "resource://gre/modules/AppConstants.sys.mjs",
  SelectableProfileService:
    "resource:///modules/profiles/SelectableProfileService.sys.mjs",
  AlertsServiceDND: () => {
    try {
      let alertsService = Cc["@mozilla.org/alerts-service;1"]
        .getService(Ci.nsIAlertsService)
        .QueryInterface(Ci.nsIAlertsDoNotDisturb);
      alertsService.manualDoNotDisturb;
      return alertsService;
    } catch (ex) {
      return undefined;
    }
  },
});

export const PRIVACY_SEGMENTATION_PREF =
  "browser.privacySegmentation.preferences.show";
const BACKUP_ENABLED_ON_PROFILES_PREF_NAME =
  "browser.backup.enabled_on.profiles";
const PREF_UPLOAD_ENABLED = "datareporting.healthreport.uploadEnabled";
const PREF_ADDON_RECOMMENDATIONS_ENABLED = "browser.discovery.enabled";
const PREF_NORMANDY_ENABLED = "app.normandy.enabled";
const PREF_OPT_OUT_STUDIES_ENABLED = "app.shield.optoutstudies.enabled";

Preferences.addAll([
  { id: "network.lna.blocking", type: "bool" },

  { id: "media.setsinkid.enabled", type: "bool" },

  { id: "dom.disable_open_during_load", type: "bool" },
  { id: "dom.security.framebusting_intervention.enabled", type: "bool" },

  { id: "xpinstall.whitelist.required", type: "bool" },
  { id: PRIVACY_SEGMENTATION_PREF, type: "bool" },
  { id: BACKUP_ENABLED_ON_PROFILES_PREF_NAME, type: "string" },

  { id: PREF_NORMANDY_ENABLED, type: "bool" },
  { id: "nimbus.rollouts.enabled", type: "bool" },

  { id: PREF_OPT_OUT_STUDIES_ENABLED, type: "bool" },
  { id: PREF_ADDON_RECOMMENDATIONS_ENABLED, type: "bool" },
  { id: PREF_UPLOAD_ENABLED, type: "bool" },
  { id: "datareporting.usage.uploadEnabled", type: "bool" },
]);

export function showPermissionExceptions({
  permissionType,
  dialogType = "site",
}) {
  if (dialogType === "site") {
    window.gSubDialog.open(
      "chrome://browser/content/preferences/dialogs/sitePermissions.xhtml",
      { features: "resizable=yes" },
      { permissionType }
    );
  } else {
    window.gSubDialog.open(
      "chrome://browser/content/preferences/dialogs/permissions.xhtml",
      { features: "resizable=yes" },
      {
        blockVisible: false,
        sessionVisible: false,
        allowVisible: true,
        prefilledHost: "",
        permissionType,
      }
    );
  }
}

Preferences.addSetting({
  id: "enabledLNA",
  pref: "network.lna.blocking",
});
Preferences.addSetting({
  id: "enabledSpeakerControl",
  pref: "media.setsinkid.enabled",
});
Preferences.addSetting({
  id: "permissionBox",
});
Preferences.addSetting({
  id: "locationSettingsButton",
  onUserClick: () => showPermissionExceptions({ permissionType: "geo" }),
});
Preferences.addSetting({
  id: "cameraSettingsButton",
  onUserClick: () => showPermissionExceptions({ permissionType: "camera" }),
});
Preferences.addSetting({
  id: "loopbackNetworkSettingsButton",
  onUserClick: () =>
    showPermissionExceptions({
      permissionType: "loopback-network",
    }),
  deps: ["enabledLNA"],
  visible: deps => {
    return deps.enabledLNA.value;
  },
});
Preferences.addSetting({
  id: "localNetworkSettingsButton",
  onUserClick: () =>
    showPermissionExceptions({ permissionType: "local-network" }),
  deps: ["enabledLNA"],
  visible: deps => {
    return deps.enabledLNA.value;
  },
});
Preferences.addSetting({
  id: "microphoneSettingsButton",
  onUserClick: () => showPermissionExceptions({ permissionType: "microphone" }),
});
Preferences.addSetting({
  id: "speakerSettingsButton",
  onUserClick: () => showPermissionExceptions({ permissionType: "speaker" }),
  deps: ["enabledSpeakerControl"],
  visible: ({ enabledSpeakerControl }) => {
    return enabledSpeakerControl.value;
  },
});
Preferences.addSetting({
  id: "notificationSettingsButton",
  onUserClick: () =>
    showPermissionExceptions({
      permissionType: "desktop-notification",
    }),
});
Preferences.addSetting({
  id: "autoplaySettingsButton",
  onUserClick: () =>
    showPermissionExceptions({ permissionType: "autoplay-media" }),
});
Preferences.addSetting({
  id: "popupPolicy",
  pref: "dom.disable_open_during_load",
});
Preferences.addSetting({
  id: "redirectPolicy",
  pref: "dom.security.framebusting_intervention.enabled",
});
Preferences.addSetting({
  id: "popupAndRedirectPolicy",
  deps: ["popupPolicy", "redirectPolicy"],
  get: (_val, deps) => {
    if (deps.popupPolicy.locked && !deps.redirectPolicy.locked) {
      return deps.redirectPolicy.value;
    }
    if (!deps.popupPolicy.locked && deps.redirectPolicy.locked) {
      return deps.popupPolicy.value;
    }
    return deps.popupPolicy.value && deps.redirectPolicy.value;
  },
  set: (val, deps) => {
    if (!deps.popupPolicy.locked) {
      deps.popupPolicy.value = val;
    }
    if (!deps.redirectPolicy.locked) {
      deps.redirectPolicy.value = val;
    }
  },
  disabled: ({ popupPolicy, redirectPolicy }) =>
    popupPolicy.locked && redirectPolicy.locked,
});
Preferences.addSetting({
  id: "popupAndRedirectPolicyButton",
  deps: ["popupPolicy", "redirectPolicy"],
  onUserClick: () =>
    showPermissionExceptions({
      permissionType: "popup",
      dialogType: "permission",
    }),
  disabled: ({ popupPolicy, redirectPolicy }) =>
    !popupPolicy.value ||
    !redirectPolicy.value ||
    (popupPolicy.locked && redirectPolicy.locked),
});
Preferences.addSetting({
  id: "warnAddonInstall",
  pref: "xpinstall.whitelist.required",
});
Preferences.addSetting({
  id: "addonExceptions",
  deps: ["warnAddonInstall"],
  onUserClick: () =>
    showPermissionExceptions({
      permissionType: "install",
      dialogType: "permission",
    }),
  disabled: ({ warnAddonInstall }) => {
    return !warnAddonInstall.value || warnAddonInstall.locked;
  },
});
Preferences.addSetting({
  id: "notificationsDoNotDisturb",
  get: () => {
    return lazy.AlertsServiceDND?.manualDoNotDisturb ?? false;
  },
  set: value => {
    if (lazy.AlertsServiceDND) {
      lazy.AlertsServiceDND.manualDoNotDisturb = value;
    }
  },
  visible: () => {
    return lazy.AlertsServiceDND != undefined;
  },
});

Preferences.addSetting({
  id: "privacySegmentation",
  pref: PRIVACY_SEGMENTATION_PREF,
});
Preferences.addSetting({
  id: "dataCollectionCategory",
  deps: ["privacySegmentation"],
  visible: ({ privacySegmentation }) =>
    lazy.AppConstants.MOZ_DATA_REPORTING || privacySegmentation.value,
});
Preferences.addSetting({
  id: "dataCollectionLink",
  visible: () => {
    const url = Services.urlFormatter.formatURLPref(
      "toolkit.datacollection.infoURL"
    );
    if (url) {
      return true;
    }
    return false;
  },
  getControlConfig(config) {
    const url = Services.urlFormatter.formatURLPref(
      "toolkit.datacollection.infoURL"
    );
    return {
      ...config,
      controlAttrs: {
        ...config.controlAttrs,
        href: url,
      },
    };
  },
});
Preferences.addSetting({
  id: "preferencesPrivacyProfiles",
  visible: () => lazy.SelectableProfileService.isEnabled,
});
Preferences.addSetting({
  id: "privacyProfilesLink",
  onUserClick: () => window.gMainPane.manageProfiles(),
});
Preferences.addSetting({
  id: "telemetryContainer",
  deps: ["submitHealthReportBox"],
  visible: deps => {
    if (!lazy.AppConstants.MOZ_DATA_REPORTING) {
      return false;
    }
    return !deps.submitHealthReportBox.value;
  },
});
Preferences.addSetting({
  id: "profilesBackupEnabled",
  pref: BACKUP_ENABLED_ON_PROFILES_PREF_NAME,
});
Preferences.addSetting({
  id: "submitHealthReportBox",
  pref: PREF_UPLOAD_ENABLED,
  getControlConfig(config, _, setting) {
    if (!setting.value) {
      return {
        ...config,
        l10nId: "data-collection-health-report-disabled",
      };
    }
    return {
      ...config,
      l10nId: "data-collection-health-report",
    };
  },
});
Preferences.addSetting({
  id: "addonRecommendationEnabled",
  pref: PREF_ADDON_RECOMMENDATIONS_ENABLED,
  deps: ["submitHealthReportBox"],
  visible: () => lazy.AppConstants.MOZ_DATA_REPORTING,
  get: (value, deps) => {
    return value && deps.submitHealthReportBox.pref.value;
  },
});
Preferences.addSetting({
  id: "normandyEnabled",
  pref: PREF_NORMANDY_ENABLED,
});

Preferences.addSetting({
  id: "optOutStudiesEnabled",
  visible: () => lazy.AppConstants.MOZ_NORMANDY,
  pref: PREF_OPT_OUT_STUDIES_ENABLED,
  deps: ["submitHealthReportBox", "normandyEnabled"],
  disabled: ({ submitHealthReportBox, normandyEnabled }) => {
    const allowedByPolicy = Services.policies.isAllowed("Shield");
    return (
      !allowedByPolicy || !submitHealthReportBox.value || !normandyEnabled.value
    );
  },
  get: (value, { submitHealthReportBox, normandyEnabled }) => {
    const allowedByPolicy = Services.policies.isAllowed("Shield");

    if (
      !allowedByPolicy ||
      !submitHealthReportBox.value ||
      !normandyEnabled.value
    ) {
      return false;
    }
    return value;
  },
});
Preferences.addSetting({
  id: "viewShieldStudies",
});
Preferences.addSetting({
  id: "enableNimbusRollouts",
  pref: "nimbus.rollouts.enabled",
  visible: () =>
    lazy.AppConstants.MOZ_DATA_REPORTING && lazy.AppConstants.MOZ_NORMANDY,
  disabled: () => !Services.policies.isAllowed("NimbusRollouts"),
  get: value => {
    if (!Services.policies.isAllowed("NimbusRollouts")) {
      return false;
    }
    return value;
  },
});
Preferences.addSetting({
  id: "submitUsagePingBox",
  pref: "datareporting.usage.uploadEnabled",
  visible: () => lazy.AppConstants.MOZ_DATA_REPORTING,
});
Preferences.addSetting(
   ({
    id: "backup-multi-profile-warning-message-bar",
    _originalStateOfDataCollectionPrefs: new Map(),
    deps: [
      "addonRecommendationEnabled",
      "optOutStudiesEnabled",
      "submitHealthReportBox",
      "submitUsagePingBox",
      "profilesBackupEnabled",
    ],
    setup(emitChange, dataCollectionPrefDeps) {
      for (let pref in dataCollectionPrefDeps) {
        const value = dataCollectionPrefDeps[pref].value;
        this._originalStateOfDataCollectionPrefs.set(pref, value);
      }
      emitChange();
    },
    visible(dataCollectionPrefDeps) {
      const { currentProfile } = lazy.SelectableProfileService;
      if (!currentProfile) {
        return false;
      }
      let anyPrefChanged = false;
      for (let pref in dataCollectionPrefDeps) {
        if (pref === "profilesBackupEnabled") {
          continue;
        }
        const originalValue =
          this._originalStateOfDataCollectionPrefs.get(pref);
        const updatedValue = dataCollectionPrefDeps[pref].value;
        if (updatedValue !== originalValue) {
          anyPrefChanged = true;
          break;
        }
      }

      const profilesBackupEnabledValue =  (
        dataCollectionPrefDeps.profilesBackupEnabled.value
      );
      let profilesEnabledOn;
      try {
        let parsed = JSON.parse(profilesBackupEnabledValue || "[]");
        profilesEnabledOn = Array.isArray(parsed)
          ? parsed
          : Object.keys(parsed);
      } catch {
        profilesEnabledOn = [];
      }
      let currentId = currentProfile.id;
      let otherProfilesEnabled = profilesEnabledOn.some(id => id != currentId);
      return otherProfilesEnabled && anyPrefChanged;
    },
  })
);

SettingGroupManager.registerGroups({
  permissions: {
    id: "permissions",
    subcategory: "permissions",
    l10nId: "permissions-header3",
    headingLevel: 2,
    items: [
      {
        id: "permissionBox",
        control: "moz-box-group",
        controlAttrs: {
          type: "list",
        },
        items: [
          {
            id: "locationSettingsButton",
            control: "moz-box-button",
            l10nId: "permissions-location2",
            controlAttrs: {
              ".iconSrc": "chrome://browser/skin/notification-icons/geo.svg",
              "search-l10n-ids":
                "permissions-remove.label,permissions-remove-all.label,permissions-site-location-window2.title,permissions-site-location-desc,permissions-site-location-disable-label,permissions-site-location-disable-desc",
            },
          },
          {
            id: "cameraSettingsButton",
            control: "moz-box-button",
            l10nId: "permissions-camera2",
            controlAttrs: {
              ".iconSrc": "chrome://browser/skin/notification-icons/camera.svg",
              "search-l10n-ids":
                "permissions-remove.label,permissions-remove-all.label,permissions-site-camera-window2.title,permissions-site-camera-desc,permissions-site-camera-disable-label,permissions-site-camera-disable-desc,",
            },
          },
          {
            id: "loopbackNetworkSettingsButton",
            control: "moz-box-button",
            l10nId: "permissions-localhost2",
            controlAttrs: {
              ".iconSrc":
                "chrome://browser/skin/notification-icons/local-host.svg",
              "search-l10n-ids":
                "permissions-remove.label,permissions-remove-all.label,permissions-site-localhost-window.title,permissions-site-localhost-desc,permissions-site-localhost-disable-label,permissions-site-localhost-disable-desc,",
            },
          },
          {
            id: "localNetworkSettingsButton",
            control: "moz-box-button",
            l10nId: "permissions-local-network2",
            controlAttrs: {
              ".iconSrc":
                "chrome://browser/skin/notification-icons/local-network.svg",
              "search-l10n-ids":
                "permissions-remove.label,permissions-remove-all.label,permissions-site-local-network-window.title,permissions-site-local-network-desc,permissions-site-local-network-disable-label,permissions-site-local-network-disable-desc,",
            },
          },
          {
            id: "microphoneSettingsButton",
            control: "moz-box-button",
            l10nId: "permissions-microphone2",
            controlAttrs: {
              ".iconSrc":
                "chrome://browser/skin/notification-icons/microphone.svg",
              "search-l10n-ids":
                "permissions-remove.label,permissions-remove-all.label,permissions-site-microphone-window2.title,permissions-site-microphone-desc,permissions-site-microphone-disable-label,permissions-site-microphone-disable-desc,",
            },
          },
          {
            id: "speakerSettingsButton",
            control: "moz-box-button",
            l10nId: "permissions-speaker2",
            controlAttrs: {
              ".iconSrc":
                "chrome://browser/skin/notification-icons/speaker.svg",
              "search-l10n-ids":
                "permissions-remove.label,permissions-remove-all.label,permissions-site-speaker-window.title,permissions-site-speaker-desc,",
            },
          },
          {
            id: "notificationSettingsButton",
            control: "moz-box-button",
            l10nId: "permissions-notification2",
            controlAttrs: {
              ".iconSrc":
                "chrome://browser/skin/notification-icons/desktop-notification.svg",
              "search-l10n-ids":
                "permissions-remove.label,permissions-remove-all.label,permissions-site-notification-window2.title,permissions-site-notification-desc,permissions-site-notification-disable-label,permissions-site-notification-disable-desc,",
            },
          },
          {
            id: "autoplaySettingsButton",
            control: "moz-box-button",
            l10nId: "permissions-autoplay2",
            controlAttrs: {
              ".iconSrc":
                "chrome://browser/skin/notification-icons/autoplay-media.svg",
              "search-l10n-ids":
                "permissions-remove.label,permissions-remove-all.label,permissions-site-autoplay-window2.title,permissions-site-autoplay-desc,",
            },
          },
        ],
      },
      {
        id: "popupAndRedirectPolicy",
        l10nId: "permissions-block-popups2",
        subcategory: "permissions-block-popups",
        items: [
          {
            id: "popupAndRedirectPolicyButton",
            l10nId: "permissions-block-popups-exceptions-button4",
            control: "moz-box-button",
            controlAttrs: {
              "search-l10n-ids":
                "permissions-address,permissions-exceptions-popup-window3.title,permissions-exceptions-popup-desc2,permissions-block-popups-exceptions-button4.searchkeywords",
            },
          },
        ],
      },
      {
        id: "warnAddonInstall",
        l10nId: "permissions-addon-install-warning3",
        items: [
          {
            id: "addonExceptions",
            l10nId: "permissions-addon-exceptions2",
            control: "moz-box-button",
            controlAttrs: {
              "search-l10n-ids":
                "permissions-address,permissions-allow.label,permissions-remove.label,permissions-remove-all.label,permissions-exceptions-addons-window2.title,permissions-exceptions-addons-desc",
            },
          },
        ],
      },
      {
        id: "notificationsDoNotDisturb",
        l10nId: "permissions-notification-pause",
      },
    ],
  },
  dataCollection: {
    items: [
      {
        id: "dataCollectionCategory",
        l10nId: "data-collection",
        control: "moz-fieldset",
        iconSrc: "chrome://global/skin/icons/trending.svg",
        controlAttrs: {
          headinglevel: 2,
          "data-l10n-attrs": "searchkeywords",
        },
        items: [
          {
            id: "dataCollectionLink",
            control: "a",
            l10nId: "data-collection-link",
            slot: "support-link",
            controlAttrs: {
              id: "dataCollectionPrivacyNoticeLink",
              target: "_blank",
            },
          },
          {
            id: "preferencesPrivacyProfiles",
            control: "moz-message-bar",
            l10nId: "data-collection-preferences-across-profiles",
            controlAttrs: {
              role: "status",
            },
            items: [
              {
                id: "privacyProfilesLink",
                control: "a",
                l10nId: "data-collection-profiles-link",
                slot: "support-link",
                controlAttrs: {
                  id: "dataCollectionViewProfiles",
                  target: "_blank",
                  href: "",
                },
              },
            ],
          },
          {
            id: "telemetryContainer",
            control: "moz-message-bar",
            l10nId: "data-collection-health-report-telemetry-disabled",
            supportPage: "telemetry-clientid",
            controlAttrs: {
              role: "status",
            },
          },
          {
            id: "backup-multi-profile-warning-message-bar",
            control: "moz-message-bar",
            l10nId: "backup-multi-profile-warning-message",
            controlAttrs: {
              dismissable: true,
            },
          },
          {
            id: "submitHealthReportBox",
            supportPage: "technical-and-interaction-data",
            subcategory: "reports",
            items: [
              {
                id: "addonRecommendationEnabled",
                l10nId: "addon-recommendations3",
                supportPage: "personalized-addons",
              },
              {
                id: "optOutStudiesEnabled",
                l10nId: "data-collection-run-studies",
                items: [
                  {
                    id: "viewShieldStudies",
                    control: "moz-box-link",
                    l10nId: "data-collection-studies-link",
                    controlAttrs: {
                      href: "about:studies",
                    },
                  },
                ],
              },
            ],
          },

          {
            id: "enableNimbusRollouts",
            l10nId: "nimbus-rollouts",
            supportPage: "remote-improvements",
          },
          {
            id: "submitUsagePingBox",
            l10nId: "data-collection-usage-ping",
            subcategory: "reports",
            supportPage: "usage-ping-settings",
          },
        ],
      },
    ],
  },
});
