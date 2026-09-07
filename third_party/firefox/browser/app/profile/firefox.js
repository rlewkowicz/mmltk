#filter dumbComments emptyLines substitution

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.


#if defined(XP_UNIX)
    #define UNIX_BUT_NOT_MAC
#endif


#if defined(UNIX_BUT_NOT_MAC)
  pref("general.autoScroll", false);
#else
  pref("general.autoScroll", true);
#endif

sticky_pref("browser.uidensity", 0);
pref("browser.compactmode.show", false);

pref("browser.startup.page",                1);
pref("browser.startup.homepage",            "about:home");
pref("browser.startup.homepage.abouthome_cache.enabled", true);
pref("browser.startup.homepage.abouthome_cache.loglevel", "Warn");

pref("browser.startup.firstrunSkipsHomepage", true);

pref("browser.startup.couldRestoreSession.count", 0);

pref("browser.chrome.site_icons", true);
pref("browser.warnOnQuit", true);

  pref("browser.warnOnQuitShortcut", true);

  pref("browser.fullscreen.autohide", true);

pref("browser.overlink-delay", 80);

  pref("browser.taskbarTabs.enabled", false);

pref("browser.urlbar.ctrlCanonizesURLs", true);

pref("browser.urlbar.autoFill", true);

#if defined(NIGHTLY_BUILD)
pref("browser.urlbar.autoFill.adaptiveHistory.enabled", true);
#else
pref("browser.urlbar.autoFill.adaptiveHistory.enabled", false);
#endif

pref("browser.urlbar.autoFill.backspaceBlockDurationMs", 172800000);

pref("browser.urlbar.autoFill.backspaceThreshold", 3);

pref("browser.urlbar.autoFill.dismissalBlockDurationMs", 604800000);

pref("browser.urlbar.autoFill.adaptiveHistory.minCharsThreshold", 0);

pref("browser.urlbar.speculativeConnect.enabled", true);

pref("browser.urlbar.filter.javascript", true);

pref("browser.urlbar.focusContentDocumentOnEsc", true);

pref("browser.urlbar.ipc.chromeMessagePassing", false);

pref("browser.urlbar.loglevel", "Error");

pref("browser.urlbar.maxRichResults", 10);

pref("browser.urlbar.suggest.bookmark",             true);
pref("browser.urlbar.suggest.history",              true);
pref("browser.urlbar.suggest.openpage",             true);
pref("browser.urlbar.suggest.engines",              true);

pref("browser.urlbar.deduplication.enabled", true);
pref("browser.urlbar.deduplication.thresholdDays", 0);

pref("browser.urlbar.unifiedSearchButton.always", false);

pref("browser.urlbar.contextMenu.featureGate", false);

pref("browser.urlbar.unitConversion.enabled", true);

pref("browser.urlbar.showSearchSuggestionsFirst", true);


pref("browser.urlbar.trimURLs", true);
pref("browser.urlbar.trimHttps", false);
pref("browser.urlbar.untrimOnUserInteraction.featureGate", false);

pref("browser.urlbar.decodeURLsOnCopy", false);

pref("browser.urlbar.switchTabs.adoptIntoActiveWindow", false);

pref("browser.urlbar.openintab", false);

pref("browser.urlbar.resultMenu.keyboardAccessible", true);

pref("browser.urlbar.showSearchTerms.featureGate", false);

pref("browser.urlbar.showSearchTerms.enabled", true);

pref("browser.urlbar.shortcuts.bookmarks", true);
pref("browser.urlbar.shortcuts.tabs", true);
pref("browser.urlbar.shortcuts.history", true);

pref("browser.urlbar.dnsResolveSingleWordsAfterSearch", 0);

pref("browser.urlbar.keepPanelOpenDuringImeComposition", false);

pref("browser.altClickSave", false);

pref("browser.download.saveLinkAsFilenameTimeout", 4000);

pref("browser.download.useDownloadDir", true);
pref("browser.download.folderList", 1);
pref("browser.download.manager.addToRecentDocs", true);
pref("browser.download.manager.resumeOnWakeDelay", 10000);

pref("browser.download.panel.shown", false);

pref("browser.download.openInSystemViewerContextMenuItem", true);

pref("browser.download.alwaysOpenInSystemViewerContextMenuItem", true);

pref("browser.download.viewableInternally.enabledTypes", "xml,svg,webp,avif,jxl");


pref("browser.download.autohideButton", true);

pref("browser.download.alwaysOpenPanel", true);

pref("browser.download.clearHistoryOnDelete", 0);

  pref("browser.helperApps.deleteTempFileOnExit", true);

pref("browser.helperApps.showOpenOptionForViewableInternally", true);

pref("browser.search.openintab", false);

pref("browser.search.context.loadInBackground", false);

pref("browser.search.widget.removeAfterDaysUnused", 120);

pref("browser.search.widget.new", true);

pref("browser.spin_cursor_while_busy", false);

pref("browser.privateWindowSeparation.enabled", true);

pref("browser.privacySegmentation.preferences.show", false);

pref("browser.sessionhistory.max_entries", 50);

pref("permissions.manager.defaultsUrl", "resource://app/defaults/permissions");

pref("permissions.default.loopback-network", 0);
pref("permissions.default.local-network", 0);
pref("permissions.default.shortcuts", 0);

pref("permissions.fullscreen.allowed", false);

pref("browser.link.force_default_user_context_id_for_external_opens", false);

pref("browser.link.open_newwindow", 3);

pref("browser.link.open_newwindow.override.external", -1);

pref("browser.link.open_newwindow.restriction", 2);

  pref("browser.link.open_newwindow.disabled_in_fullscreen", false);

pref("browser.link.alternative_click.block_javascript", true);

pref("browser.tabs.closeTabByDblclick", false);
pref("browser.tabs.closeWindowWithLastTab", true);
pref("browser.tabs.allowTabDetach", true);
pref("browser.tabs.insertRelatedAfterCurrent", true);
pref("browser.tabs.insertAfterCurrent", false);
pref("browser.tabs.insertAfterCurrentExceptPinned", false);
pref("browser.tabs.warnOnClose", false);
pref("browser.tabs.warnOnCloseOtherTabs", true);
pref("browser.tabs.warnOnOpen", true);
pref("browser.tabs.maxOpenBeforeWarn", 15);
pref("browser.tabs.loadDivertedInBackground", false);
pref("browser.tabs.loadBookmarksInBackground", false);
pref("browser.tabs.loadBookmarksInTabs", false);
pref("browser.tabs.tabClipWidth", 140);
pref("browser.tabs.tabMinWidth", 76);

pref("browser.tabs.selectOwnerOnClose", true);

pref("browser.tabs.selectMRUOnClose", false);

pref("browser.tabs.delayHidingAudioPlayingIconMS", 3000);

pref("browser.tabs.remote.separatePrivilegedContentProcess", true);

#if defined(NIGHTLY_BUILD) && !defined(MOZ_ASAN)
  pref("browser.tabs.remote.enforceRemoteTypeRestrictions", true);
#endif

pref("browser.tabs.remote.separatePrivilegedMozillaWebContentProcess", true);

#if defined(NIGHTLY_BUILD)
pref("browser.tabs.tooltipsShowPidAndActiveness", true);
#else
pref("browser.tabs.tooltipsShowPidAndActiveness", false);
#endif

pref("browser.tabs.hoverPreview.enabled", true);
pref("browser.tabs.hoverPreview.showThumbnails", true);

pref("browser.tabs.groups.enabled", true);
pref("browser.tabs.groups.hoverPreview.enabled", true);
pref("browser.tabs.groups.alternateMenu", false);


pref("browser.tabs.dragDrop.createGroup.enabled", true);
pref("browser.tabs.dragDrop.createGroup.delayMS", 240);
pref("browser.tabs.dragDrop.expandGroup.delayMS", 350);
pref("browser.tabs.dragDrop.selectTab.delayMS", 350);
pref("browser.tabs.dragDrop.pinInteractionCue.delayMS", 500);
pref("browser.tabs.dragDrop.moveOverThresholdPercent", 80);

pref("security.allow_parent_unrestricted_js_loads", false);

    pref("browser.tabs.unloadOnLowMemory", false);

pref("browser.tabs.min_inactive_duration_before_unload", 600000);

#if defined(UNIX_BUT_NOT_MAC)
pref("browser.tabs.searchclipboardfor.middleclick", true);
#else
pref("browser.tabs.searchclipboardfor.middleclick", false);
#endif



pref("browser.ctrlTab.sortByRecentlyUsed", false);

pref("browser.bookmarks.autoExportHTML",          false);

pref("browser.bookmarks.max_backups",             15);

pref("browser.bookmarks.openInTabClosesMenu", true);

pref("browser.bookmarks.defaultLocation", "toolbar");

pref("browser.tabs.allow_transparent_browser", false);

pref("dom.disable_open_during_load",              true);

pref("dom.disable_window_move_resize",            false);
pref("dom.disable_window_flip",                   true);

pref("privacy.popups.showBrowserMessage",   true);

pref("privacy.clearOnShutdown.history",     true);
pref("privacy.clearOnShutdown.formdata",    true);
pref("privacy.clearOnShutdown.downloads",   true);
pref("privacy.clearOnShutdown.cookies",     true);
pref("privacy.clearOnShutdown.cache",       true);
pref("privacy.clearOnShutdown.sessions",    true);
pref("privacy.clearOnShutdown.offlineApps", false);
pref("privacy.clearOnShutdown.siteSettings", false);
pref("privacy.clearOnShutdown.openWindows", false);

pref("privacy.clearOnShutdown_v2.historyFormDataAndDownloads", true);

pref("privacy.clearOnShutdown_v2.browsingHistoryAndDownloads", true);
pref("privacy.clearOnShutdown_v2.cookiesAndStorage", true);
pref("privacy.clearOnShutdown_v2.cache", true);
pref("privacy.clearOnShutdown_v2.siteSettings", false);
pref("privacy.clearOnShutdown_v2.formdata", false);

pref("privacy.cpd.history",                 true);
pref("privacy.cpd.formdata",                true);
pref("privacy.cpd.downloads",               true);
pref("privacy.cpd.cookies",                 true);
pref("privacy.cpd.cache",                   true);
pref("privacy.cpd.sessions",                true);
pref("privacy.cpd.offlineApps",             false);
pref("privacy.cpd.siteSettings",            false);
pref("privacy.cpd.openWindows",             false);

pref("privacy.clearHistory.historyFormDataAndDownloads", true);
pref("privacy.clearHistory.browsingHistoryAndDownloads", true);
pref("privacy.clearHistory.cookiesAndStorage", true);
pref("privacy.clearHistory.cache", true);
pref("privacy.clearHistory.siteSettings", false);
pref("privacy.clearHistory.formdata", false);
pref("privacy.clearSiteData.historyFormDataAndDownloads", false);
pref("privacy.clearSiteData.browsingHistoryAndDownloads", false);
pref("privacy.clearSiteData.cookiesAndStorage", true);
pref("privacy.clearSiteData.cache", true);
pref("privacy.clearSiteData.siteSettings", false);
pref("privacy.clearSiteData.formdata", false);

pref("privacy.history.custom",              false);

pref("privacy.sanitize.timeSpan", 1);

pref("privacy.sanitize.clearOnShutdown.hasMigratedToNewPrefs2", false);
pref("privacy.sanitize.clearOnShutdown.hasMigratedToNewPrefs3", false);
pref("privacy.sanitize.cpd.hasMigratedToNewPrefs2", false);
pref("privacy.sanitize.cpd.hasMigratedToNewPrefs3", false);

pref("privacy.panicButton.enabled",         true);

pref("privacy.temporary_permission_expire_time_ms",  3600000);

pref("privacy.authPromptSpoofingProtection",         true);

pref("privacy.globalprivacycontrol.functionality.enabled",  true);

pref("privacy.globalprivacycontrol.pbmode.enabled", true);

pref("network.proxy.share_proxy_settings",  false); 

pref("browser.gesture.swipe.left", "Browser:BackOrBackDuplicate");
pref("browser.gesture.swipe.right", "Browser:ForwardOrForwardDuplicate");
pref("browser.gesture.swipe.up", "cmd_scrollTop");
pref("browser.gesture.swipe.down", "cmd_scrollBottom");
pref("browser.gesture.pinch.latched", false);
pref("browser.gesture.pinch.threshold", 25);
#if 0 || defined(MOZ_WIDGET_GTK)
  pref("browser.gesture.pinch.out", "cmd_fullZoomEnlarge");
  pref("browser.gesture.pinch.in", "cmd_fullZoomReduce");
  pref("browser.gesture.pinch.out.shift", "cmd_fullZoomReset");
  pref("browser.gesture.pinch.in.shift", "cmd_fullZoomReset");
#else
  pref("browser.gesture.pinch.out", "");
  pref("browser.gesture.pinch.in", "");
  pref("browser.gesture.pinch.out.shift", "");
  pref("browser.gesture.pinch.in.shift", "");
#endif
pref("browser.gesture.twist.latched", false);
pref("browser.gesture.twist.threshold", 0);
pref("browser.gesture.twist.right", "cmd_gestureRotateRight");
pref("browser.gesture.twist.left", "cmd_gestureRotateLeft");
pref("browser.gesture.twist.end", "cmd_gestureRotateEnd");
#if 0 || defined(MOZ_WIDGET_GTK)
  pref("browser.gesture.tap", "cmd_fullZoomReset");
#else
  pref("browser.gesture.tap", "");
#endif

pref("browser.history_swipe_animation.disabled", false);

  pref("mousewheel.with_shift.action", 4);
  pref("mousewheel.with_alt.action", 2);

pref("mousewheel.with_meta.action", 1);

pref("browser.xul.error_pages.expert_bad_cert", false);

pref("network.manage-offline-status", true);

pref("network.lna.prompt.timeout", 300000); 

pref("network.lna.temporary_permission_expire_time_ms", 86400000); 

pref("network.protocol-handler.external.mailto", true); 

pref("network.protocol-handler.warn-external.mailto", false);

pref("network.protocol-handler.expose-all", true);
pref("network.protocol-handler.expose.mailto", false);
pref("network.protocol-handler.expose.news", false);
pref("network.protocol-handler.expose.snews", false);
pref("network.protocol-handler.expose.nntp", false);


pref("browser.preferences.experimental.hidden", false);
pref("browser.preferences.moreFromMozilla", true);
pref("browser.preferences.defaultPerformanceSettings.enabled", true);

pref("browser.proton.toolbar.version", 0);

pref("browser.backspace_action", 2);

pref("intl.regional_prefs.use_os_locales", false);

pref("browser.send_pings", false);

pref("browser.lna.warning.infoURL", "https://support.mozilla.org/%LOCALE%/kb/control-personal-device-local-network-permissions-firefox");

pref("browser.sessionstore.resume_from_crash", true);
pref("browser.sessionstore.resume_session_once", false);
pref("browser.sessionstore.resuming_after_os_restart", false);

pref("browser.sessionstore.closedTabsFromAllWindows", true);
pref("browser.sessionstore.closedTabsFromClosedWindows", true);

pref("browser.sessionstore.interval.idle", 3600000); 

pref("browser.sessionstore.idleDelay", 180); 

pref("browser.sessionstore.log.appender.console", "Fatal");
pref("browser.sessionstore.log.appender.dump", "Error");
pref("browser.sessionstore.log.appender.file.level", "Trace");
pref("browser.sessionstore.log.appender.file.logOnError", true);

pref("browser.sessionstore.loglevel", "Warn");

#if defined(NIGHTLY_BUILD)
  pref("browser.sessionstore.loglevel", "Debug");
  pref("browser.sessionstore.log.appender.file.logOnSuccess", true);
#else
  pref("browser.sessionstore.log.appender.file.logOnSuccess", false);
#endif
pref("browser.sessionstore.log.appender.file.maxErrorAge", 864000); 

pref("browser.sessionstore.logFlushIntervalSeconds", 3600);

pref("browser.sessionstore.privacy_level", 0);
pref("browser.sessionstore.max_tabs_undo", 25);
pref("browser.sessionstore.max_windows_undo", 5);
pref("browser.sessionstore.max_resumed_crashes", 1);
pref("browser.sessionstore.max_serialize_back", 10);
pref("browser.sessionstore.max_serialize_forward", -1);
pref("browser.sessionstore.restore_on_demand", true);
pref("browser.sessionstore.restore_hidden_tabs", false);
pref("browser.sessionstore.restore_pinned_tabs_on_demand", false);
pref("browser.sessionstore.upgradeBackup.latestBuildID", "");
pref("browser.sessionstore.upgradeBackup.maxUpgradeBackups", 3);
pref("browser.sessionstore.debug", false);
pref("browser.sessionstore.cleanup.forget_closed_after", 1209600000);

pref("browser.sessionstore.persist_closed_tabs_between_sessions", true);

pref("browser.quitShortcut.disabled", false);


pref("places.history.enabled", true);

pref("places.loglevel", "Error");

pref("places.search.matchDiacritics", false);


pref("places.frecency.origins.alternative.featureGate", false);

pref("browser.places.speculativeConnect.enabled", true);

pref("browser.zoom.full", true);

pref("browser.zoom.updateBackgroundTabs", true);

pref("app.support.baseURL", "https://support.mozilla.org/1/firefox/%VERSION%/%OS%/%LOCALE%/");

pref("app.feedback.baseURL", "https://ideas.mozilla.org/");

pref("security.certerrors.permanentOverride", true);
pref("security.certerrors.mitm.priming.enabled", true);
pref("security.certerrors.mitm.priming.endpoint", "https://mitmdetection.services.mozilla.com/");
pref("security.certerrors.mitm.auto_enable_enterprise_roots", true);

pref("browser.bookmarks.editDialog.showForNewBookmarks", true);

pref("browser.bookmarks.editDialog.firstEditField", "namePicker");

pref("browser.bookmarks.editDialog.maxRecentFolders", 7);






pref("prompts.defaultModalType", 3);

pref("security.mixed_content.block_active_content", true);

pref("security.insecure_connection_text.enabled", true);
pref("security.insecure_connection_text.pbmode.enabled", true);

pref("dom.debug.propagate_gesture_events_through_content", false);

pref("browser.uiCustomization.debug", false);

pref("browser.uiCustomization.state", "");

pref("media.contextmenu.video-overlay-detection", true);
pref("network.cookie.cookieBehavior", 5);

pref("network.cookie.cookieBehavior.pbmode", 5);

pref("privacy.trackingprotection.fingerprinting.enabled", true);

pref("privacy.trackingprotection.cryptomining.enabled", true);

pref("privacy.resistFingerprinting.skipEarlyBlankFirstPaint", true);

pref("browser.contentblocking.database.enabled", true);

pref("browser.contentblocking.cryptomining.preferences.ui.enabled", true);
pref("browser.contentblocking.fingerprinting.preferences.ui.enabled", true);
pref("browser.contentblocking.reject-and-isolate-cookies.preferences.ui.enabled", true);

pref("browser.contentblocking.features.strict", "tp,tpPrivate,cookieBehavior5,cookieBehaviorPBM5,cryptoTP,fp,stp,emailTP,emailTPPrivate,-consentmanagerSkip,-consentmanagerSkipPrivate,lvl2,rp,rpTop,qps,qpsPBM,fpp,fppPrivate,btp,lna");


pref("browser.contentblocking.report.monitor.enabled", false);

#if defined(NIGHTLY_BUILD)
  pref("browser.contentblocking.report.privacy_metrics.enabled", true);
#else
  pref("browser.contentblocking.report.privacy_metrics.enabled", false);
#endif

pref("browser.contentblocking.report.show_mobile_app", true);

pref("browser.send_to_device_locales", "de,en-GB,en-US,es-AR,es-CL,es-ES,es-MX,fr,id,pl,pt-BR,ru,zh-TW");

pref("browser.vpn_promo.disallowed_regions", "ae,by,cn,cu,iq,ir,kp,om,ru,sd,sy,tm,tr");

pref("browser.vpn_promo.enabled", true);
pref("browser.contentblocking.report.vpn_regions", "as,at,au,bd,be,bg,br,ca,ch,cl,co,cy,cz,de,dk,ee,eg,es,fi,fr,gb,gg,gr,hr,hu,id,ie,im,in,io,it,je,ke,kr,lt,lu,lv,ma,mp,mt,mx,my,ng,nl,no,nz,pl,pr,pt,ro,sa,se,sg,si,sk,sn,th,tr,tw,ua,ug,uk,um,us,vg,vi,vn,za");

pref("browser.promo.focus.disallowed_regions", "cn");

pref("browser.promo.focus.enabled", true);

pref("browser.promo.pin.enabled", true);

pref("browser.contentblocking.report.hide_vpn_banner", false);
pref("browser.contentblocking.report.vpn_sub_id", "sub_HrfCZF7VPHzZkA");

pref("browser.contentblocking.report.monitor.url", "https://monitor.firefox.com/?entrypoint=protection_report_monitor&utm_source=about-protections");
pref("browser.contentblocking.report.monitor.how_it_works.url", "https://monitor.firefox.com/about");
pref("browser.contentblocking.report.monitor.sign_in_url", "https://monitor.firefox.com/oauth/init?entrypoint=protection_report_monitor&utm_source=about-protections&email=");
pref("browser.contentblocking.report.monitor.preferences_url", "https://monitor.firefox.com/user/preferences");
pref("browser.contentblocking.report.monitor.home_page_url", "https://monitor.firefox.com/user/dashboard");
pref("browser.contentblocking.report.manage_devices.url", "https://accounts.firefox.com/settings/clients");
pref("browser.contentblocking.report.mobile-android.url", "https://play.google.com/store/apps/details?id=org.mozilla.firefox&referrer=utm_source%3Dprotection_report%26utm_content%3Dmobile_promotion");
pref("browser.contentblocking.report.vpn.url", "https://vpn.mozilla.org/?utm_source=firefox-browser&utm_medium=firefox-browser&utm_campaign=about-protections-card");
pref("browser.contentblocking.report.vpn-promo.url", "https://vpn.mozilla.org/?utm_source=firefox-browser&utm_medium=firefox-browser&utm_campaign=about-protections-top-promo");
pref("browser.contentblocking.report.vpn-android.url", "https://play.google.com/store/apps/details?id=org.mozilla.firefox.vpn&referrer=utm_source%3Dfirefox-browser%26utm_medium%3Dfirefox-browser%26utm_campaign%3Dabout-protections-mobile-vpn%26anid%3D--");
pref("browser.contentblocking.report.vpn-ios.url", "https://apps.apple.com/us/app/firefox-private-network-vpn/id1489407738");

pref("browser.contentblocking.report.social.url", "https://support.mozilla.org/1/firefox/%VERSION%/%OS%/%LOCALE%/social-media-tracking-report");
pref("browser.contentblocking.report.cookie.url", "https://support.mozilla.org/1/firefox/%VERSION%/%OS%/%LOCALE%/cross-site-tracking-report");
pref("browser.contentblocking.report.tracker.url", "https://support.mozilla.org/1/firefox/%VERSION%/%OS%/%LOCALE%/tracking-content-report");
pref("browser.contentblocking.report.fingerprinter.url", "https://support.mozilla.org/1/firefox/%VERSION%/%OS%/%LOCALE%/fingerprinters-report");
pref("browser.contentblocking.report.cryptominer.url", "https://support.mozilla.org/1/firefox/%VERSION%/%OS%/%LOCALE%/cryptominers-report");

pref("browser.contentblocking.cfr-milestone.enabled", true);
pref("browser.contentblocking.cfr-milestone.milestone-achieved", 0);
pref("browser.contentblocking.cfr-milestone.milestones", "[1000, 5000, 10000, 25000, 50000, 100000, 250000, 314159, 500000, 750000, 1000000, 1250000, 1500000, 1750000, 2000000, 2250000, 2500000, 8675309]");

pref("browser.protections_panel.infoMessage.seen", false);

pref("privacy.usercontext.about_newtab_segregation.enabled", true);
pref("privacy.userContext.enabled", true);
pref("privacy.userContext.ui.enabled", true);
pref("privacy.userContext.newTabContainerOnLeftClick.enabled", false);


pref("privacy.exposeContentTitleInWindow", true);
pref("privacy.exposeContentTitleInWindow.pbm", true);

pref("browser.tabs.remote.warmup.enabled", true);

pref("browser.tabs.remote.tabCacheSize", 0);

pref("browser.tabs.remote.warmup.maxTabs", 3);
pref("browser.tabs.remote.warmup.unloadDelayMs", 2000);

pref("browser.tabs.unloadTabInContextMenu", true);

pref("browser.tabs.fadeOutExplicitlyUnloadedTabs", true);

pref("browser.tabs.fadeOutUnloadedTabs", false);

pref("browser.tabs.splitView.enabled", true);
pref("browser.tabs.splitview.hasUsed", false);

pref("browser.tabs.remoteSVGIconDecoding", true);




pref("view_source.tab", true);

pref("toolkit.pageThumbs.minWidth", 280);
pref("toolkit.pageThumbs.minHeight", 190);


pref("browser.laterrun.enabled", false);

pref("dom.ipc.processPrelaunch.enabled", true);







pref("browser.sessionstore.restore_tabs_lazily", true);

pref("browser.suppress_first_window_animation", true);

pref("doh-rollout.clearModeOnShutdown", false);

pref("network.trr_ui.fallback_was_checked", true);

pref("browser.aboutConfig.showWarning", true);

pref("browser.toolbars.keyboard_navigation", true);

pref("browser.toolbars.bookmarks.visibility", "newtab");

pref("browser.toolbars.bookmarks.showOtherBookmarks", true);

pref("security.certerrors.felt-privacy-v1", true);



#if defined(MOZ_DEV_EDITION)
  pref("browser.menu.showViewImageInfo", true);
#else
  pref("browser.menu.showViewImageInfo", false);
#endif

pref("svg.context-properties.content.allowed-domains", "profile.accounts.firefox.com,profile.stage.mozaws.net");

pref("browser.places.interactions.enabled", true);

  pref("browser.swipe.navigation-icon-start-position", -40);
  pref("browser.swipe.navigation-icon-end-position", 60);
  pref("browser.swipe.navigation-icon-min-radius", 12);
  pref("browser.swipe.navigation-icon-max-radius", 20);

pref("browser.settings-redesign.enabled", true);

#if defined(MOZ_WIDGET_GTK)
pref("widget.support-xdg-config", true, locked);
#endif

#if defined(MOZ_MINIMAL_BROWSER)
  pref("browser.region.network.url", "");
  pref("browser.search.suggest.enabled", false);
  pref("browser.search.suggest.enabled.private", false);
  pref("browser.search.update", false);
  pref("browser.startup.homepage", "about:blank");
  pref("browser.startup.homepage.abouthome_cache.enabled", false);
  pref("browser.startup.page", 0);
  pref("browser.toolbars.bookmarks.showOtherBookmarks", false);
  pref("browser.toolbars.bookmarks.visibility", "never");
  pref("browser.urlbar.suggest.bookmark", false);
  pref("browser.urlbar.suggest.engines", false);
  pref("browser.urlbar.suggest.history", false);
  pref("dom.webgpu.blocked-domains", "");
  pref("dom.webgpu.enabled", true);
  pref("dom.webgpu.service-workers.enabled", true);
  pref("gfx.webgpu.ignore-blocklist", true);
  pref("network.connectivity-service.enabled", false);
  pref("services.settings.poll_interval", 2147483647);
#endif
