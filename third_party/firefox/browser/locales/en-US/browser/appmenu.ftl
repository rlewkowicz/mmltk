# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

## App Menu

appmenuitem-banner-update-downloading =
    .label = Downloading { -brand-shorter-name } update

appmenuitem-banner-update-available =
    .label = Update available — download now

appmenuitem-banner-update-manual =
    .label = Update available — download now

appmenuitem-banner-update-unsupported =
    .label = Unable to update — system incompatible

appmenuitem-banner-update-restart =
    .label = Update available — restart now

# Fresh Firefox refers to the new updated UI
appmenu-nova-update-promo =
    .message = Get a fresh { -brand-short-name }. Keep all your tabs.

appmenu-nova-update-link = Restart to update

appmenu-nova-fxa-sign-in = Sign in

appmenu-nova-switch-device-promo =
    .message = Getting a new device soon? Take { -brand-short-name } with you!

appmenu-nova-switch-device-link = How to migrate your data

appmenuitem-new-tab =
    .label = New Tab
appmenuitem-new-window =
    .label = New Window
appmenuitem-new-private-window =
    .label = New Private Window
appmenuitem-history =
  .label = History
appmenuitem-tab-groups =
  .label = Tab groups
appmenuitem-downloads =
  .label = Downloads
appmenuitem-passwords =
    .label = Passwords
appmenuitem-extensions-and-themes =
    .label = Extensions and Themes
appmenuitem-extensions =
    .label = Extensions
appmenuitem-print =
  .label = Print…
appmenuitem-find-in-page =
    .label = Find in Page…
appmenuitem-translate =
    .label = Translate Page…
appmenuitem-zoom =
    .value = Zoom
appmenuitem-more-tools =
    .label = More Tools
appmenuitem-help =
    .label = Help
appmenuitem-exit2 =
    .label =
        { PLATFORM() ->
            [linux] Quit
           *[other] Exit
        }
appmenu-menu-button-closed2 =
    .tooltiptext = Open application menu
    .label = { -brand-short-name }
appmenu-menu-button-opened2 =
    .tooltiptext = Close application menu
    .label = { -brand-short-name }

# Settings is now used to access the browser settings across all platforms,
# instead of Options or Preferences.
appmenuitem-settings =
    .label = Settings

## Zoom and Fullscreen Controls

appmenuitem-zoom-enlarge =
  .label = Zoom In
appmenuitem-zoom-reduce =
  .label = Zoom Out
appmenuitem-fullscreen =
  .label = Full screen

## Firefox Account toolbar button and Sync panel in App menu.

appmenu-remote-tabs-sign-into-sync =
  .label = Sign in to sync…
appmenu-remote-tabs-turn-on-sync =
  .label = Turn on sync…

# This is shown after the tabs list if we can display more tabs by clicking on the button
appmenu-remote-tabs-showmore =
  .label = Show more tabs
  .tooltiptext = Show more tabs from this device

# This is shown as the label for an element to show inactive tabs from this device.
appmenu-remote-tabs-show-inactive-tabs =
  .label = Inactive tabs
  .tooltiptext = See inactive tabs on this device

# This is shown beneath the name of a device when that device has no open tabs
appmenu-remote-tabs-notabs = No open tabs

# This is shown when Sync is configured but syncing tabs is disabled.
appmenu-remote-tabs-tabsnotsyncing = Turn on tab syncing to view a list of tabs from your other devices.

appmenu-remote-tabs-opensettings =
  .label = Settings

# This is shown when Sync is configured but this appears to be the only device attached to
# the account. We also show links to download Firefox for android/ios.
appmenu-remote-tabs-noclients = Want to see your tabs from other devices here?

appmenu-remote-tabs-connectdevice =
  .label = Connect Another Device
appmenu-remote-tabs-welcome = View a list of tabs from your other devices.
appmenu-remote-tabs-unverified = Your account needs to be verified.

appmenuitem-fxa-toolbar-sync-now2 = Sync now
appmenuitem-fxa-sign-in = Sign in to { -brand-product-name }
appmenuitem-fxa-manage-account = Manage account
appmenu-account-header = Account
# Variables
# $time (string) - Localized relative time since last sync (e.g. 1 second ago,
# 3 hours ago, etc.)
appmenu-fxa-last-sync = Last synced { $time }
    .label = Last synced { $time }
appmenu-fxa-sync-and-save-data2 = Sync and save data
appmenu-fxa-signed-in-label = Sign In
appmenu-fxa-setup-sync =
    .label = Turn On Syncing…
appmenu-fxa-setup-sync-new = Turn On
appmenuitem-save-page =
    .label = Save Page As…

appmenuitem-fxa-sync-off-title = Sync is off
appmenu-manage-history =
    .label = Manage history
appmenu-restore-session =
    .label = Restore previous session
appmenu-clear-history =
    .label = Clear recent history…
appmenu-recent-history-subheader = Recent history
appmenu-recently-closed-tabs =
    .label = Recently closed tabs
appmenu-recently-closed-windows =
    .label = Recently closed windows
# This allows to search through the browser's history.
appmenu-search-history =
    .label = Search history

## Help panel

appmenu-help-header =
    .title = { -brand-shorter-name } help
appmenu-about =
    .label = About { -brand-shorter-name }
    .accesskey = A
appmenu-get-help =
    .label = Get help
    .accesskey = h
appmenu-help-more-troubleshooting-info =
    .label = More troubleshooting information
    .accesskey = t
appmenu-help-share-ideas =
    .label = Share ideas and feedback…
    .accesskey = S
appmenu-help-switch-device =
    .label = Switching to a new device

## appmenu-help-enter-troubleshoot-mode and appmenu-help-exit-troubleshoot-mode
## are mutually exclusive, so it's possible to use the same accesskey for both.

appmenu-help-enter-troubleshoot-mode2 =
    .label = Troubleshoot Mode…
    .accesskey = M
appmenu-help-exit-troubleshoot-mode =
    .label = Turn Troubleshoot Mode off
    .accesskey = M

## More Tools

appmenu-customizetoolbar =
    .label = Customize toolbar…
appmenu-edit-pdf =
    .label = Edit PDF…

appmenuitem-report-broken-site =
  .label = Report Broken Site

## Panel for privacy and security products

appmenuitem-sign-in-account = Sign in to your account

appmenuitem-monitor-title2 = Stay Ahead of Identity Theft
appmenuitem-monitor-description2 = Get alerts about data breaches
appmenuitem-relay-title = { -relay-brand-short-name }
appmenuitem-relay-title2 = Keep Your Email Private
appmenuitem-relay-description2 = Helps prevent spam in your inbox
appmenuitem-services-relay-description = Launch email masks dashboard
appmenuitem-vpn-title2 = Hide Your Location with { -mozilla-vpn-brand-name }
appmenuitem-vpn-description3 = Make your browsing harder to trace

appmenu-services-header = My services
# "Mozilla" is intentionally hardcoded to prevent forks from replacing it
# with their own vendor name, since these tools are created and maintained by
# Mozilla.
appmenu-other-protection-header3 = Privacy tools

## Profiles panel

appmenu-profiles-2 =
    .label = Profiles
appmenu-other-profiles = Other profiles
appmenu-manage-profiles =
    .label = Manage profiles
appmenu-copy-profile =
    .label = Copy this profile
appmenu-create-profile =
    .label = New profile
appmenu-edit-profile =
    .aria-label = Edit profile
