// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/. */


pref("security.tls.insecure_fallback_hosts", "");

pref("security.default_personal_cert",   "Ask Every Time");

pref("security.signed_app_signatures.policy", 2);

pref("security.pki.mitm_canary_issuer", "");
pref("security.pki.mitm_canary_issuer.enabled", true);

pref("security.pki.mitm_detected", false);

pref("security.remote_settings.intermediates.enabled", true);
pref("security.remote_settings.intermediates.downloads_per_poll", 5000);
pref("security.remote_settings.intermediates.parallel_downloads", 8);

pref("security.remote_settings.crlite_filters.enabled", true);

pref("security.osreauthenticator.blank_password", false);
pref("security.osreauthenticator.password_last_changed_lo", 0);
pref("security.osreauthenticator.password_last_changed_hi", 0);

pref("security.crash_tracking.js_load_1.prevCrashes", 0);
pref("security.crash_tracking.js_load_1.maxCrashes", 1);

pref("general.useragent.compatMode.firefox", false);

pref("general.config.obscure_value", 13); 

pref("general.autoscroll.prevent_to_start.shiftKey", true); 
pref("general.autoscroll.prevent_to_start.ctrlKey", false); 
pref("general.autoscroll.prevent_to_start.altKey", false);  
pref("general.autoscroll.prevent_to_start.metaKey", false);

pref("general.autoscroll.prevent_to_collapse_selection_by_middle_mouse_down", false);

pref("browser.bookmarks.max_backups",       5);

pref("browser.cache.disk_cache_ssl",        true);
pref("browser.cache.frecency_half_life_hours", 6);

pref("browser.download.forbid_open_with", false);

pref("dom.indexedDB.logging.enabled", true);
pref("dom.indexedDB.logging.details", true);
pref("dom.workers.maxPerDomain", 512);

pref("dom.serviceWorkers.idle_timeout", 30000);

pref("dom.serviceWorkers.idle_extended_timeout", 30000);

pref("dom.serviceWorkers.update_delay", 1000);

pref("dom.serviceWorkers.testUpdateOverOneDay", false);

pref("dom.keyboardevent.keypress.hack.dispatch_non_printable_keys", "www.icloud.com");
pref("dom.keyboardevent.keypress.hack.dispatch_non_printable_keys.addl", "");

pref("dom.keyboardevent.keypress.hack.use_legacy_keycode_and_charcode", "*.collabserv.com,*.gov.online.office365.us,*.officeapps-df.live.com,*.officeapps.live.com,*.online.office.de,*.partner.officewebapps.cn,*.scniris.com");
pref("dom.keyboardevent.keypress.hack.use_legacy_keycode_and_charcode.addl", "");

pref("dom.text-recognition.enabled", true);

pref("editor.texteditor.inputevent.hack.no_dispatch_before_compositionend", "");
pref("editor.htmleditor.inputevent.hack.no_dispatch_before_compositionend", "");
pref("editor.texteditor.inputevent.hack.no_dispatch_before_compositionend.addl", "");
pref("editor.htmleditor.inputevent.hack.no_dispatch_before_compositionend.addl", "");

pref("editor.texteditor.inputevent.hack.no_dispatch_after_compositionend", "");
pref("editor.htmleditor.inputevent.hack.no_dispatch_after_compositionend", "www.icloud.com");
pref("editor.texteditor.inputevent.hack.no_dispatch_after_compositionend.addl", "");
pref("editor.htmleditor.inputevent.hack.no_dispatch_after_compositionend.addl", "");

pref("browser.sessionhistory.max_total_viewers", -1);

pref("browser.send_pings", false);
pref("browser.send_pings.max_per_link", 1);           
pref("browser.send_pings.require_same_host", false);  

pref("browser.helperApps.neverAsk.saveToDisk", "");
pref("browser.helperApps.neverAsk.openFile", "");
pref("browser.helperApps.deleteTempFileOnExit", false);

pref("browser.triple_click_selects_paragraph", true);

pref("mathml.disabled",    false);

pref("media.throttle-factor", 2);

pref("media.volume_scale", "1.0");

pref("media.play-stand-alone", true);


#if defined(NIGHTLY_BUILD)
  pref("media.decoder-doctor.notifications-allowed", "MediaWMFNeeded,MediaWidevineNoWMF,MediaCannotInitializePulseAudio,MediaCannotPlayNoDecoders,MediaUnsupportedLibavcodec,MediaPlatformDecoderNotFound,MediaDecodeError");
#else
  pref("media.decoder-doctor.notifications-allowed", "MediaWMFNeeded,MediaWidevineNoWMF,MediaCannotInitializePulseAudio,MediaCannotPlayNoDecoders,MediaUnsupportedLibavcodec,MediaPlatformDecoderNotFound");
#endif
pref("media.decoder-doctor.decode-errors-allowed", "");
pref("media.decoder-doctor.decode-warnings-allowed", "");
pref("media.decoder-doctor.verbose", false);
pref("media.decoder-doctor.new-issue-endpoint", "https://webcompat.com/issues/new");

pref("media.videocontrols.keyboard-tab-to-all-controls", true);

pref("media.webvtt.debug.logging", false);

pref("media.recorder.audio_node.enabled", false);

pref("media.recorder.video.frame_drops", true);

pref("media.cubeb.output_voice_routing", true);

pref("media.cubeb.force_mock_context", false);

pref("apz.overscroll.stop_velocity_threshold", "0.01");
pref("apz.overscroll.stretch_factor", "0.35");

pref("apz.zoom-to-focused-input.enabled", true);



pref("gfx.downloadable_fonts.enabled", true);
pref("gfx.downloadable_fonts.fallback_delay", 3000);
pref("gfx.downloadable_fonts.fallback_delay_short", 100);

pref("gfx.canvas.azure.backends", "skia");
pref("gfx.content.azure.backends", "skia");

pref("gfx.webrender.debug.texture-cache", false);
pref("gfx.webrender.debug.texture-cache.clear-evicted", true);
pref("gfx.webrender.debug.render-targets", false);
pref("gfx.webrender.debug.gpu-cache", false);
pref("gfx.webrender.debug.alpha-primitives", false);
pref("gfx.webrender.debug.gpu-time-queries", false);
pref("gfx.webrender.debug.gpu-sample-queries", false);
pref("gfx.webrender.debug.disable-batching", false);
pref("gfx.webrender.debug.epochs", false);
pref("gfx.webrender.debug.echo-driver-messages", false);
pref("gfx.webrender.debug.show-overdraw", false);
pref("gfx.webrender.debug.slow-frame-indicator", false);
pref("gfx.webrender.debug.picture-caching", false);
pref("gfx.webrender.debug.picture-borders", false);
pref("gfx.webrender.debug.force-picture-invalidation", false);
pref("gfx.webrender.debug.primitives", false);
pref("gfx.webrender.debug.small-screen", false);
pref("gfx.webrender.debug.obscure-images", false);
pref("gfx.webrender.debug.glyph-flashing", false);
pref("gfx.webrender.debug.window-visibility", false);
pref("gfx.webrender.debug.external-composite-borders", false);

pref("gfx.webrender.multithreading", true);
pref("gfx.webrender.pbo-uploads", true);
pref("gfx.webrender.batched-texture-uploads", false);
pref("gfx.webrender.draw-calls-for-texture-copy", false);


pref("accessibility.warn_on_browsewithcaret", true);

pref("accessibility.browsewithcaret_shortcut.enabled", true);

#if !0 && !defined(MOZ_WIDGET_GTK)
  pref("ui.scrollToClick", 0);
#endif

pref("ui.textSelectDisabledBackground", "#b0b0b0");

pref("ui.textSelectAttentionBackground", "#38d878");
pref("ui.textSelectAttentionForeground", "#ffffff");

pref("ui.textHighlightBackground", "#ef0fff");
pref("ui.textHighlightForeground", "#ffffff");

pref("accessibility.force_disabled", 0);


pref("accessibility.typeaheadfind", true);
pref("accessibility.typeaheadfind.manual", true);
pref("accessibility.typeaheadfind.casesensitive", 0);
pref("accessibility.typeaheadfind.linksonly", true);
pref("accessibility.typeaheadfind.startlinksonly", false);
pref("accessibility.typeaheadfind.timeout", 4000);
pref("accessibility.typeaheadfind.soundURL", "beep");
pref("accessibility.typeaheadfind.wrappedSoundURL", "");
pref("accessibility.typeaheadfind.enablesound", true);
  pref("accessibility.typeaheadfind.prefillwithselection", true);
pref("accessibility.typeaheadfind.matchesCountLimit", 1000);
pref("findbar.highlightAll", false);
pref("findbar.entireword", false);
pref("findbar.iteratorTimeout", 100);
pref("findbar.matchdiacritics", 0);
pref("findbar.modalHighlight", false);

pref("gfx.use_text_smoothing_setting", false);

pref("toolkit.autocomplete.richBoundaryCutoff", 200);

pref("toolkit.scrollbox.scrollIncrement", 20);
pref("toolkit.scrollbox.clickToScroll.scrollDelay", 150);

pref("toolkit.shopping.ohttpConfigURL", "https://prod.ohttp-gateway.prod.webservices.mozgcp.net/ohttp-configs");
pref("toolkit.shopping.ohttpRelayURL", "https://mozilla-ohttp.fastly-edge.com/");

pref("toolkit.sqlitejsm.loglevel", "Error");

pref("toolkit.tabbox.switchByScrolling", false);

#if defined(MOZ_ASAN)
  pref("toolkit.asyncshutdown.crash_timeout", 300000); 
#elif defined(MOZ_TSAN)
  pref("toolkit.asyncshutdown.crash_timeout", 360000); 
#elif defined(MOZ_CODE_COVERAGE)
  pref("toolkit.asyncshutdown.crash_timeout", 180000); 
#else
  pref("toolkit.asyncshutdown.crash_timeout", 60000); 
#endif
pref("toolkit.asyncshutdown.log", false);

#if defined(MOZILLA_OFFICIAL)
  pref("browser.dom.window.dump.enabled", false, sticky);
#else
  pref("browser.dom.window.dump.enabled", true, sticky);
#endif

pref("toolkit.dump.emit", false);

pref("view_source.editor.path", "");
pref("view_source.editor.args", "");

pref("nglayout.enable_drag_images", true);

pref("browser.fixup.alternate.prefix", "www.");
pref("browser.fixup.alternate.protocol", "https");
pref("browser.fixup.alternate.suffix", ".com");
pref("browser.fixup.fallback-to-https", true);

pref("dom.beforeunload_timeout_ms",         1000);
pref("dom.disable_window_flip",             false);
pref("dom.disable_window_move_resize",      false);

pref("dom.allow_scripts_to_close_windows",          false);

pref("dom.popup_allowed_events", "change click dblclick auxclick mousedown mouseup pointerdown pointerup notificationclick reset submit touchend contextmenu");

pref("dom.storage.shadow_writes", false);
pref("dom.storage.snapshot_prefill", 16384);
pref("dom.storage.snapshot_gradual_prefill", 4096);
pref("dom.storage.snapshot_reusing", true);
pref("dom.storage.client_validation", true);

pref("dom.forms.datetime.timepicker", true);

pref("dom.forms.selectSearch", false);
#if defined(MOZ_WIDGET_GTK) || 0
  pref("dom.forms.select.customstyling", false);
#else
  pref("dom.forms.select.customstyling", true);
#endif

pref("dom.cycle_collector.incremental", true);

pref("privacy.resistFingerprinting.exemptedDomains", "*.example.invalid");

pref("privacy.fingerprintingProtection.overrides", "");

pref("privacy.fingerprintingProtection.granularOverrides", "");

pref("privacy.baselineFingerprintingProtection.overrides", "");

pref("privacy.baselineFingerprintingProtection.granularOverrides", "");

pref("privacy.restrict3rdpartystorage.partitionedHosts", "accounts.google.com/o/oauth2/,d35nw2lg0ahg0v.cloudfront.net/,datastudio.google.com/embed/reporting/,d3qlaywcwingl6.cloudfront.net/");

pref("privacy.restrict3rdpartystorage.userInteractionRequiredForHosts", "");

pref("privacy.restrict3rdpartystorage.url_decorations", "");

pref("privacy.popups.maxReported", 100);

pref("privacy.purge_trackers.enabled", true);
#if defined(NIGHTLY_BUILD)
  pref("privacy.purge_trackers.logging.level", "Warn");
#else
  pref("privacy.purge_trackers.logging.level", "Error");
#endif

pref("privacy.purge_trackers.max_purge_count", 100);

pref("privacy.purge_trackers.consider_entity_list", false);

pref("privacy.wallet_schemes", "openid4vp,mdoc,mdoc-openid4vp,haip,eudi-wallet,eudi-openid4vp,openid-credential-offer");

pref("dom.event.contextmenu.enabled",       true);

pref("javascript.enabled",                  true);
pref("javascript.options.wasm",                   true);
pref("javascript.options.wasm_trustedprincipals", true);
pref("javascript.options.wasm_verbose",           false);
pref("javascript.options.wasm_baselinejit",       true);

pref("javascript.options.asyncstack", true);

pref("javascript.options.discardSystemSource", false);


pref("javascript.options.mem.max", -1);

pref("javascript.options.mem.nursery.min_kb", 256);
pref("javascript.options.mem.nursery.max_kb", 65536);

pref("javascript.options.mem.gc_per_zone", true);
pref("javascript.options.mem.gc_incremental", true);

pref("javascript.options.mem.incremental_weakmap", true);

pref("javascript.options.mem.gc_incremental_slice_ms", 5);

pref("javascript.options.mem.gc_compacting", true);

pref("javascript.options.mem.gc_generational", true);

#if defined(NIGHTLY_BUILD)
pref("javascript.options.mem.gc_experimental_semispace_nursery", false);
#endif

pref("javascript.options.mem.gc_parallel_marking", true);

#if defined(XP_UNIX)
pref("javascript.options.mem.gc_parallel_marking_threshold_mb", 16);
#endif

pref("javascript.options.mem.gc_max_parallel_marking_threads", 2);

#if defined(NIGHTLY_BUILD)
pref("javascript.options.mem.gc_experimental_concurrent_marking", false);
#endif

pref("javascript.options.mem.gc_high_frequency_time_limit_ms", 1000);

pref("javascript.options.mem.gc_small_heap_size_max_mb", 100);

pref("javascript.options.mem.gc_large_heap_size_min_mb", 500);

pref("javascript.options.mem.gc_high_frequency_small_heap_growth", 300);

pref("javascript.options.mem.gc_high_frequency_large_heap_growth", 150);

pref("javascript.options.mem.gc_low_frequency_heap_growth", 150);

pref("javascript.options.mem.gc_balanced_heap_limits", false);

pref("javascript.options.mem.gc_heap_growth_factor", 50);

pref("javascript.options.mem.gc_allocation_threshold_mb", 27);

pref("javascript.options.mem.gc_malloc_threshold_base_mb", 38);

pref("javascript.options.mem.gc_small_heap_incremental_limit", 150);

pref("javascript.options.mem.gc_large_heap_incremental_limit", 110);

pref("javascript.options.mem.gc_urgent_threshold_mb", 16);

pref("javascript.options.mem.gc_min_empty_chunk_count", 1);

pref("javascript.options.mem.gc_helper_thread_ratio", 50);

pref("javascript.options.mem.gc_max_helper_threads", 8);

pref("javascript.options.mem.nursery_eager_collection_threshold_kb", 256);
pref("javascript.options.mem.nursery_eager_collection_threshold_percent", 25);
pref("javascript.options.mem.nursery_eager_collection_timeout_ms", 5000);

pref("javascript.options.mem.nursery_max_time_goal_ms", 4);

#if defined(JS_GC_ZEAL)
pref("javascript.options.mem.gc_zeal.mode", 0);
pref("javascript.options.mem.gc_zeal.frequency", 5000);
#endif

pref("javascript.options.shared_memory", true);

pref("javascript.options.throw_on_debuggee_would_run", false);
pref("javascript.options.dump_stack_on_debuggee_would_run", false);

pref("image.animation_mode",                "normal");

pref("keyword.enabled", true);

pref("browser.fixup.domainwhitelist.localhost", true);
pref("browser.fixup.domainsuffixwhitelist.test", true);
pref("browser.fixup.domainsuffixwhitelist.example", true);
pref("browser.fixup.domainsuffixwhitelist.invalid", true);
pref("browser.fixup.domainsuffixwhitelist.localhost", true);
pref("browser.fixup.domainsuffixwhitelist.internal", true);
pref("browser.fixup.domainsuffixwhitelist.local", true);

pref("browser.fixup.dns_first_for_single_words", false);


pref("network.tickle-wifi.enabled", false);
pref("network.tickle-wifi.duration", 400);
pref("network.tickle-wifi.delay", 16);

pref("network.protocol-handler.external-default", true);      
pref("network.protocol-handler.warn-external-default", true); 

pref("network.protocol-handler.external.hcp", false);
pref("network.protocol-handler.external.vbscript", false);
pref("network.protocol-handler.external.javascript", false);
pref("network.protocol-handler.external.data", false);
pref("network.protocol-handler.external.ie.http", false);
pref("network.protocol-handler.external.iehistory", false);
pref("network.protocol-handler.external.ierss", false);
pref("network.protocol-handler.external.mk", false);
pref("network.protocol-handler.external.ms-cxh", false);
pref("network.protocol-handler.external.ms-cxh-full", false);
pref("network.protocol-handler.external.ms-help", false);
pref("network.protocol-handler.external.ms-msdt", false);
pref("network.protocol-handler.external.res", false);
pref("network.protocol-handler.external.search", false);
pref("network.protocol-handler.external.search-ms", false);
pref("network.protocol-handler.external.shell", false);
pref("network.protocol-handler.external.vnd.ms.radio", false);
pref("network.protocol-handler.external.disk", false);
pref("network.protocol-handler.external.disks", false);
pref("network.protocol-handler.external.afp", false);
pref("network.protocol-handler.external.moz-icon", false);

pref("network.protocol-handler.external.ttp", false);  
pref("network.protocol-handler.external.htp", false);  
pref("network.protocol-handler.external.ttps", false); 
pref("network.protocol-handler.external.tps", false);  
pref("network.protocol-handler.external.ps", false);   
pref("network.protocol-handler.external.htps", false); 
pref("network.protocol-handler.external.ile", false);  
pref("network.protocol-handler.external.le", false);   


pref("network.protocol-handler.expose-all", true);


pref("network.manage-offline-status", true);

pref("network.http.version", "1.1");      

pref("network.http.proxy.version", "1.1");    

pref("network.http.proxy.respect-be-conservative", true);

pref("network.http.default-socket-type", "");

pref("network.http.keep-alive.timeout", 115);

pref("network.http.response.timeout", 300);

  pref("network.http.max-connections", 900);

pref("network.http.max-persistent-connections-per-server", 6);

pref("network.http.max-urgent-start-excessive-connections-per-host", 3);

pref("network.http.max-persistent-connections-per-proxy", 32);

pref("network.http.request.max-start-delay", 10);

pref("network.http.request.max-attempts", 10);

pref("network.http.redirection-limit", 20);

pref("network.http.accept-encoding", "gzip, deflate");
pref("network.http.accept-encoding.secure", "gzip, deflate, br, zstd");
pref("network.http.accept-encoding.dictionary", "dcb, dcz");

pref("network.http.prompt-temp-redirect", false);

pref("network.http.assoc-req.enforce", false);


pref("network.http.qos", 0);

pref("network.http.connection-retry-timeout", 250);

pref("network.http.connection-timeout", 90);

pref("network.http.tls-handshake-timeout", 30);

pref("network.http.fallback-connection-timeout", 5);

pref("network.http.network-changed.timeout", 5);

pref("network.http.speculative-parallel-limit", 20);

pref("network.http.rendering-critical-requests-prioritization", true);

pref("network.http.fast-fallback-to-IPv4", true);

pref("network.http.http3.default-qpack-table-size", 65536); 
pref("network.http.http3.default-max-stream-blocked", 20);


pref("network.http.http3.alt-svc-mapping-for-testing", "");

pref("network.http.altsvc.enabled", true);

pref("network.http.diagnostics", false);

pref("network.http.pacing.requests.enabled", true);
pref("network.http.pacing.requests.min-parallelism", 6);
pref("network.http.pacing.requests.hz", 80);
pref("network.http.pacing.requests.burst", 10);

pref("network.http.tcp_keepalive.short_lived_connections", true);
pref("network.http.tcp_keepalive.short_lived_time", 60);
pref("network.http.tcp_keepalive.short_lived_idle_time", 10);

pref("network.http.tcp_keepalive.long_lived_connections", true);
pref("network.http.tcp_keepalive.long_lived_idle_time", 600);

pref("network.http.enforce-framing.http1", false); 
pref("network.http.enforce-framing.soft", true);
pref("network.http.enforce-framing.strict_chunked_encoding", true);

pref("network.http.focused_window_transaction_ratio", "0.9");

pref("network.http.send_window_size", 1024);

pref("network.http.accept", "");

pref("network.sts.max_time_for_events_between_two_polls", 100);

pref("network.sts.poll_busy_wait_period", 50);

pref("network.sts.poll_busy_wait_period_timeout", 7);

pref("network.sts.max_time_for_pr_close_during_shutdown", 5000);

pref("network.sts.pollable_event_timeout", 6);

pref("network.websocket.max-message-size", 2147483647);

pref("network.websocket.timeout.open", 20);

pref("network.websocket.timeout.close", 20);

pref("network.websocket.timeout.ping.request", 0);

pref("network.websocket.timeout.ping.response", 10);

pref("network.websocket.max-connections", 200);

pref("network.websocket.allowInsecureFromHTTPS", false);

pref("network.websocket.delay-failed-reconnects", true);


pref("network.prefetch-next", true);


pref("network.negotiate-auth.trusted-uris", "");
pref("network.negotiate-auth.delegation-uris", "");

pref("network.negotiate-auth.allow-non-fqdn", false);

pref("network.negotiate-auth.allow-proxies", true);

pref("network.negotiate-auth.gsslib", "");

pref("network.negotiate-auth.using-native-gsslib", true);


pref("network.auth.force-generic-ntlm", false);

pref("network.automatic-ntlm-auth.allow-proxies", true);
pref("network.automatic-ntlm-auth.allow-non-fqdn", false);
pref("network.automatic-ntlm-auth.trusted-uris", "");

pref("network.auth.private-browsing-sso", false);

pref("network.http.throttle.enable", false);

pref("network.http.throttle.suspend-for", 900);
pref("network.http.throttle.resume-for", 100);

pref("network.http.throttle.hold-time-ms", 800);
pref("network.http.throttle.max-time-ms", 500);

pref("network.http.on_click_priority", true);

pref("network.proxy.http",                  "");
pref("network.proxy.http_port",             0);
pref("network.proxy.ssl",                   "");
pref("network.proxy.ssl_port",              0);
pref("network.proxy.socks",                 "");
pref("network.proxy.socks_port",            0);
pref("network.proxy.socks_version",         5);
pref("network.proxy.proxy_over_tls",        true);
pref("network.proxy.no_proxies_on",         "");
pref("network.proxy.failover_timeout",      1800); 
pref("network.online",                      true); 

pref("network.cookie.sameSite.laxByDefault.disabledHosts", "");

pref("network.cookie.maxNumber", 3000);
pref("network.cookie.maxPerHost", 180);
pref("network.cookie.quotaPerHost", 150);

pref("network.proxy.autoconfig_url", "");
pref("network.proxy.autoconfig_url.include_path", false);

pref("network.proxy.autoconfig_retry_interval_min", 5);    
pref("network.proxy.autoconfig_retry_interval_max", 300);  
pref("network.proxy.enable_wpad_over_dhcp", true);

pref("converter.html2txt.structs",          true); 
pref("converter.html2txt.header_strategy",  1); 

pref("intl.accept_languages",               "und");

pref("intl.regional_prefs.use_os_locales",  false);

pref("font.cjk_pref_fallback_order",        "zh-cn,zh-hk,zh-tw,ja,ko");

pref("intl.l10n.pseudo", "");

pref("font.name.serif.ar", "");
pref("font.name.sans-serif.ar", "");
pref("font.name.monospace.ar", "");
pref("font.name.cursive.ar", "");

pref("font.name.serif.el", "");
pref("font.name.sans-serif.el", "");
pref("font.name.monospace.el", "");
pref("font.name.cursive.el", "");

pref("font.name.serif.he", "");
pref("font.name.sans-serif.he", "");
pref("font.name.monospace.he", "");
pref("font.name.cursive.he", "");

pref("font.name.serif.ja", "");
pref("font.name.sans-serif.ja", "");
pref("font.name.monospace.ja", "");
pref("font.name.cursive.ja", "");

pref("font.name.serif.ko", "");
pref("font.name.sans-serif.ko", "");
pref("font.name.monospace.ko", "");
pref("font.name.cursive.ko", "");

pref("font.name.serif.th", "");
pref("font.name.sans-serif.th", "");
pref("font.name.monospace.th", "");
pref("font.name.cursive.th", "");

pref("font.name.serif.x-cyrillic", "");
pref("font.name.sans-serif.x-cyrillic", "");
pref("font.name.monospace.x-cyrillic", "");
pref("font.name.cursive.x-cyrillic", "");

pref("font.name.serif.x-unicode", "");
pref("font.name.sans-serif.x-unicode", "");
pref("font.name.monospace.x-unicode", "");
pref("font.name.cursive.x-unicode", "");

pref("font.name.serif.x-western", "");
pref("font.name.sans-serif.x-western", "");
pref("font.name.monospace.x-western", "");
pref("font.name.cursive.x-western", "");

pref("font.name.serif.zh-CN", "");
pref("font.name.sans-serif.zh-CN", "");
pref("font.name.monospace.zh-CN", "");
pref("font.name.cursive.zh-CN", "");

pref("font.name.serif.zh-TW", "");
pref("font.name.sans-serif.zh-TW", "");
pref("font.name.monospace.zh-TW", "");
pref("font.name.cursive.zh-TW", "");

pref("font.name.serif.zh-HK", "");
pref("font.name.sans-serif.zh-HK", "");
pref("font.name.monospace.zh-HK", "");
pref("font.name.cursive.zh-HK", "");

pref("font.name.serif.x-devanagari", "");
pref("font.name.sans-serif.x-devanagari", "");
pref("font.name.monospace.x-devanagari", "");
pref("font.name.cursive.x-devanagari", "");

pref("font.name.serif.x-tamil", "");
pref("font.name.sans-serif.x-tamil", "");
pref("font.name.monospace.x-tamil", "");
pref("font.name.cursive.x-tamil", "");

pref("font.name.serif.x-armn", "");
pref("font.name.sans-serif.x-armn", "");
pref("font.name.monospace.x-armn", "");
pref("font.name.cursive.x-armn", "");

pref("font.name.serif.x-beng", "");
pref("font.name.sans-serif.x-beng", "");
pref("font.name.monospace.x-beng", "");
pref("font.name.cursive.x-beng", "");

pref("font.name.serif.x-cans", "");
pref("font.name.sans-serif.x-cans", "");
pref("font.name.monospace.x-cans", "");
pref("font.name.cursive.x-cans", "");

pref("font.name.serif.x-ethi", "");
pref("font.name.sans-serif.x-ethi", "");
pref("font.name.monospace.x-ethi", "");
pref("font.name.cursive.x-ethi", "");

pref("font.name.serif.x-geor", "");
pref("font.name.sans-serif.x-geor", "");
pref("font.name.monospace.x-geor", "");
pref("font.name.cursive.x-geor", "");

pref("font.name.serif.x-gujr", "");
pref("font.name.sans-serif.x-gujr", "");
pref("font.name.monospace.x-gujr", "");
pref("font.name.cursive.x-gujr", "");

pref("font.name.serif.x-guru", "");
pref("font.name.sans-serif.x-guru", "");
pref("font.name.monospace.x-guru", "");
pref("font.name.cursive.x-guru", "");

pref("font.name.serif.x-khmr", "");
pref("font.name.sans-serif.x-khmr", "");
pref("font.name.monospace.x-khmr", "");
pref("font.name.cursive.x-khmr", "");

pref("font.name.serif.x-mlym", "");
pref("font.name.sans-serif.x-mlym", "");
pref("font.name.monospace.x-mlym", "");
pref("font.name.cursive.x-mlym", "");

pref("font.name.serif.x-orya", "");
pref("font.name.sans-serif.x-orya", "");
pref("font.name.monospace.x-orya", "");
pref("font.name.cursive.x-orya", "");

pref("font.name.serif.x-telu", "");
pref("font.name.sans-serif.x-telu", "");
pref("font.name.monospace.x-telu", "");
pref("font.name.cursive.x-telu", "");

pref("font.name.serif.x-knda", "");
pref("font.name.sans-serif.x-knda", "");
pref("font.name.monospace.x-knda", "");
pref("font.name.cursive.x-knda", "");

pref("font.name.serif.x-sinh", "");
pref("font.name.sans-serif.x-sinh", "");
pref("font.name.monospace.x-sinh", "");
pref("font.name.cursive.x-sinh", "");

pref("font.name.serif.x-tibt", "");
pref("font.name.sans-serif.x-tibt", "");
pref("font.name.monospace.x-tibt", "");
pref("font.name.cursive.x-tibt", "");

pref("font.name.serif.x-math", "");
pref("font.name.sans-serif.x-math", "");
pref("font.name.monospace.x-math", "");
pref("font.name.cursive.x-math", "");

pref("font.name-list.serif.x-math", "Latin Modern Math, STIX Two Math, XITS Math, Cambria Math, Libertinus Math, DejaVu Math TeX Gyre, TeX Gyre Bonum Math, TeX Gyre Pagella Math, TeX Gyre Schola, TeX Gyre Termes Math, STIX Math, Asana Math, STIXGeneral, DejaVu Serif, DejaVu Sans, serif");
pref("font.name-list.sans-serif.x-math", "sans-serif");
pref("font.name-list.monospace.x-math", "monospace");

pref("font.blacklist.underline_offset", "FangSong,Gulim,GulimChe,MingLiU,MingLiU-ExtB,MingLiU_HKSCS,MingLiU-HKSCS-ExtB,MS Gothic,MS Mincho,MS PGothic,MS PMincho,MS UI Gothic,PMingLiU,PMingLiU-ExtB,SimHei,SimSun,SimSun-ExtB,Hei,Kai,Apple LiGothic,Apple LiSung,Osaka");

pref("security.dialog_enable_delay", 1000);
pref("security.notification_enable_delay", 500);

pref("security.insecure_field_warning.ignore_local_ip_address", true);

pref("services.settings.poll_interval", 86400); 

pref("middlemouse.paste", false);
pref("middlemouse.contentLoadURL", false);
pref("middlemouse.scrollbarPosition", false);

pref("mousebutton.4th.enabled", true);
pref("mousebutton.5th.enabled", true);

pref("mousewheel.default.action", 1);
pref("mousewheel.with_alt.action", 2);
pref("mousewheel.with_control.action", 3);
pref("mousewheel.with_meta.action", 1);
pref("mousewheel.with_shift.action", 4);

pref("mousewheel.default.action.override_x", -1);
pref("mousewheel.with_alt.action.override_x", -1);
pref("mousewheel.with_control.action.override_x", -1);
pref("mousewheel.with_meta.action.override_x", -1);
pref("mousewheel.with_shift.action.override_x", -1);

pref("mousewheel.default.delta_multiplier_x", 100);
pref("mousewheel.default.delta_multiplier_y", 100);
pref("mousewheel.default.delta_multiplier_z", 100);
pref("mousewheel.with_alt.delta_multiplier_x", 100);
pref("mousewheel.with_alt.delta_multiplier_y", 100);
pref("mousewheel.with_alt.delta_multiplier_z", 100);
pref("mousewheel.with_control.delta_multiplier_x", 100);
pref("mousewheel.with_control.delta_multiplier_y", 100);
pref("mousewheel.with_control.delta_multiplier_z", 100);
pref("mousewheel.with_meta.delta_multiplier_x", 100);
pref("mousewheel.with_meta.delta_multiplier_y", 100);
pref("mousewheel.with_meta.delta_multiplier_z", 100);
pref("mousewheel.with_shift.delta_multiplier_x", 100);
pref("mousewheel.with_shift.delta_multiplier_y", 100);
pref("mousewheel.with_shift.delta_multiplier_z", 100);

pref("gestures.enable_single_finger_input", true);

pref("dom.use_watchdog", true);

pref("dom.global_stop_script", true);

#if !defined(MOZ_ASAN) && !defined(MOZ_TSAN)
  pref("dom.ipc.processCount", 8);
#else
  pref("dom.ipc.processCount", 4);
#endif

pref("dom.ipc.processCount.file", 1);

pref("dom.ipc.processCount.extension", 1);

pref("dom.ipc.processCount.privilegedabout", 1);

pref("dom.ipc.processCount.privilegedmozilla", 1);

pref("dom.ipc.processCount.webIsolated", 4);

pref("dom.ipc.processCount.inference", 1);

pref("dom.ipc.keepProcessesAlive.privilegedabout", 1);

pref("svg.disabled", false);

pref("browser.tabs.remote.enforceRemoteTypeRestrictions", false);

pref("browser.tabs.remote.separatePrivilegedContentProcess", false);

pref("browser.tabs.remote.separatedMozillaDomains", "addons.mozilla.org,accounts.firefox.com");

pref("font.default.ar", "sans-serif");
pref("font.minimum-size.ar", 0);
pref("font.size.variable.ar", 16);
pref("font.size.monospace.ar", 13);

pref("font.default.el", "serif");
pref("font.minimum-size.el", 0);
pref("font.size.variable.el", 16);
pref("font.size.monospace.el", 13);

pref("font.default.he", "sans-serif");
pref("font.minimum-size.he", 0);
pref("font.size.variable.he", 16);
pref("font.size.monospace.he", 13);

pref("font.default.ja", "sans-serif");
pref("font.minimum-size.ja", 0);
pref("font.size.variable.ja", 16);
pref("font.size.monospace.ja", 16);

pref("font.default.ko", "sans-serif");
pref("font.minimum-size.ko", 0);
pref("font.size.variable.ko", 16);
pref("font.size.monospace.ko", 16);

pref("font.default.th", "sans-serif");
pref("font.minimum-size.th", 0);
pref("font.size.variable.th", 16);
pref("font.size.monospace.th", 13);

pref("font.default.x-cyrillic", "serif");
pref("font.minimum-size.x-cyrillic", 0);
pref("font.size.variable.x-cyrillic", 16);
pref("font.size.monospace.x-cyrillic", 13);

pref("font.default.x-devanagari", "serif");
pref("font.minimum-size.x-devanagari", 0);
pref("font.size.variable.x-devanagari", 16);
pref("font.size.monospace.x-devanagari", 13);

pref("font.default.x-tamil", "serif");
pref("font.minimum-size.x-tamil", 0);
pref("font.size.variable.x-tamil", 16);
pref("font.size.monospace.x-tamil", 13);

pref("font.default.x-armn", "serif");
pref("font.minimum-size.x-armn", 0);
pref("font.size.variable.x-armn", 16);
pref("font.size.monospace.x-armn", 13);

pref("font.default.x-beng", "serif");
pref("font.minimum-size.x-beng", 0);
pref("font.size.variable.x-beng", 16);
pref("font.size.monospace.x-beng", 13);

pref("font.default.x-cans", "serif");
pref("font.minimum-size.x-cans", 0);
pref("font.size.variable.x-cans", 16);
pref("font.size.monospace.x-cans", 13);

pref("font.default.x-ethi", "serif");
pref("font.minimum-size.x-ethi", 0);
pref("font.size.variable.x-ethi", 16);
pref("font.size.monospace.x-ethi", 13);

pref("font.default.x-geor", "serif");
pref("font.minimum-size.x-geor", 0);
pref("font.size.variable.x-geor", 16);
pref("font.size.monospace.x-geor", 13);

pref("font.default.x-gujr", "serif");
pref("font.minimum-size.x-gujr", 0);
pref("font.size.variable.x-gujr", 16);
pref("font.size.monospace.x-gujr", 13);

pref("font.default.x-guru", "serif");
pref("font.minimum-size.x-guru", 0);
pref("font.size.variable.x-guru", 16);
pref("font.size.monospace.x-guru", 13);

pref("font.default.x-khmr", "serif");
pref("font.minimum-size.x-khmr", 0);
pref("font.size.variable.x-khmr", 16);
pref("font.size.monospace.x-khmr", 13);

pref("font.default.x-mlym", "serif");
pref("font.minimum-size.x-mlym", 0);
pref("font.size.variable.x-mlym", 16);
pref("font.size.monospace.x-mlym", 13);

pref("font.default.x-orya", "serif");
pref("font.minimum-size.x-orya", 0);
pref("font.size.variable.x-orya", 16);
pref("font.size.monospace.x-orya", 13);

pref("font.default.x-telu", "serif");
pref("font.minimum-size.x-telu", 0);
pref("font.size.variable.x-telu", 16);
pref("font.size.monospace.x-telu", 13);

pref("font.default.x-knda", "serif");
pref("font.minimum-size.x-knda", 0);
pref("font.size.variable.x-knda", 16);
pref("font.size.monospace.x-knda", 13);

pref("font.default.x-sinh", "serif");
pref("font.minimum-size.x-sinh", 0);
pref("font.size.variable.x-sinh", 16);
pref("font.size.monospace.x-sinh", 13);

pref("font.default.x-tibt", "serif");
pref("font.minimum-size.x-tibt", 0);
pref("font.size.variable.x-tibt", 16);
pref("font.size.monospace.x-tibt", 13);

pref("font.default.x-unicode", "serif");
pref("font.minimum-size.x-unicode", 0);
pref("font.size.variable.x-unicode", 16);
pref("font.size.monospace.x-unicode", 13);

pref("font.default.x-western", "serif");
pref("font.minimum-size.x-western", 0);
pref("font.size.variable.x-western", 16);
pref("font.size.monospace.x-western", 13);

pref("font.default.zh-CN", "sans-serif");
pref("font.minimum-size.zh-CN", 0);
pref("font.size.variable.zh-CN", 16);
pref("font.size.monospace.zh-CN", 16);

pref("font.default.zh-HK", "sans-serif");
pref("font.minimum-size.zh-HK", 0);
pref("font.size.variable.zh-HK", 16);
pref("font.size.monospace.zh-HK", 16);

pref("font.default.zh-TW", "sans-serif");
pref("font.minimum-size.zh-TW", 0);
pref("font.size.variable.zh-TW", 16);
pref("font.size.monospace.zh-TW", 16);

pref("font.default.x-math", "serif");
pref("font.minimum-size.x-math", 0);
pref("font.size.variable.x-math", 16);
pref("font.size.monospace.x-math", 13);





#if !0 && !0 && defined(XP_UNIX)
  pref("network.protocol-handler.warn-external.file", false);
  pref("browser.drag_out_of_frame_style", 1);

  pref("middlemouse.paste", true);
  pref("middlemouse.scrollbarPosition", true);


  pref("helpers.global_mime_types_file", "/etc/mime.types");
  pref("helpers.global_mailcap_file", "/etc/mailcap");
  pref("helpers.private_mime_types_file", "~/.mime.types");
  pref("helpers.private_mailcap_file", "~/.mailcap");


  pref("font.name-list.emoji", "Noto Color Emoji, Twemoji Mozilla");

  pref("font.name-list.serif.ar", "serif");
  pref("font.name-list.sans-serif.ar", "sans-serif");
  pref("font.name-list.monospace.ar", "monospace");
  pref("font.name-list.cursive.ar", "cursive");
  pref("font.size.monospace.ar", 12);

  pref("font.name-list.serif.el", "serif");
  pref("font.name-list.sans-serif.el", "sans-serif");
  pref("font.name-list.monospace.el", "monospace");
  pref("font.name-list.cursive.el", "cursive");
  pref("font.size.monospace.el", 12);

  pref("font.name-list.serif.he", "serif");
  pref("font.name-list.sans-serif.he", "sans-serif");
  pref("font.name-list.monospace.he", "monospace");
  pref("font.name-list.cursive.he", "cursive");
  pref("font.size.monospace.he", 12);

  pref("font.name-list.serif.ja", "serif");
  pref("font.name-list.sans-serif.ja", "sans-serif");
  pref("font.name-list.monospace.ja", "monospace");
  pref("font.name-list.cursive.ja", "cursive");

  pref("font.name-list.serif.ko", "serif");
  pref("font.name-list.sans-serif.ko", "sans-serif");
  pref("font.name-list.monospace.ko", "monospace");
  pref("font.name-list.cursive.ko", "cursive");

  pref("font.name-list.serif.th", "serif");
  pref("font.name-list.sans-serif.th", "sans-serif");
  pref("font.name-list.monospace.th", "monospace");
  pref("font.name-list.cursive.th", "cursive");
  pref("font.minimum-size.th", 13);

  pref("font.name-list.serif.x-armn", "serif");
  pref("font.name-list.sans-serif.x-armn", "sans-serif");
  pref("font.name-list.monospace.x-armn", "monospace");
  pref("font.name-list.cursive.x-armn", "cursive");

  pref("font.name-list.serif.x-beng", "serif");
  pref("font.name-list.sans-serif.x-beng", "sans-serif");
  pref("font.name-list.monospace.x-beng", "monospace");
  pref("font.name-list.cursive.x-beng", "cursive");

  pref("font.name-list.serif.x-cans", "serif");
  pref("font.name-list.sans-serif.x-cans", "sans-serif");
  pref("font.name-list.monospace.x-cans", "monospace");
  pref("font.name-list.cursive.x-cans", "cursive");

  pref("font.name-list.serif.x-cyrillic", "serif");
  pref("font.name-list.sans-serif.x-cyrillic", "sans-serif");
  pref("font.name-list.monospace.x-cyrillic", "monospace");
  pref("font.name-list.cursive.x-cyrillic", "cursive");
  pref("font.size.monospace.x-cyrillic", 12);

  pref("font.name-list.serif.x-devanagari", "serif");
  pref("font.name-list.sans-serif.x-devanagari", "sans-serif");
  pref("font.name-list.monospace.x-devanagari", "monospace");
  pref("font.name-list.cursive.x-devanagari", "cursive");

  pref("font.name-list.serif.x-ethi", "serif");
  pref("font.name-list.sans-serif.x-ethi", "sans-serif");
  pref("font.name-list.monospace.x-ethi", "monospace");
  pref("font.name-list.cursive.x-ethi", "cursive");

  pref("font.name-list.serif.x-geor", "serif");
  pref("font.name-list.sans-serif.x-geor", "sans-serif");
  pref("font.name-list.monospace.x-geor", "monospace");
  pref("font.name-list.cursive.x-geor", "cursive");

  pref("font.name-list.serif.x-gujr", "serif");
  pref("font.name-list.sans-serif.x-gujr", "sans-serif");
  pref("font.name-list.monospace.x-gujr", "monospace");
  pref("font.name-list.cursive.x-gujr", "cursive");

  pref("font.name-list.serif.x-guru", "serif");
  pref("font.name-list.sans-serif.x-guru", "sans-serif");
  pref("font.name-list.monospace.x-guru", "monospace");
  pref("font.name-list.cursive.x-guru", "cursive");

  pref("font.name-list.serif.x-khmr", "serif");
  pref("font.name-list.sans-serif.x-khmr", "sans-serif");
  pref("font.name-list.monospace.x-khmr", "monospace");
  pref("font.name-list.cursive.x-khmr", "cursive");

  pref("font.name-list.serif.x-knda", "serif");
  pref("font.name-list.sans-serif.x-knda", "sans-serif");
  pref("font.name-list.monospace.x-knda", "monospace");
  pref("font.name-list.cursive.x-knda", "cursive");

  pref("font.name-list.serif.x-mlym", "serif");
  pref("font.name-list.sans-serif.x-mlym", "sans-serif");
  pref("font.name-list.monospace.x-mlym", "monospace");
  pref("font.name-list.cursive.x-mlym", "cursive");

  pref("font.name-list.serif.x-orya", "serif");
  pref("font.name-list.sans-serif.x-orya", "sans-serif");
  pref("font.name-list.monospace.x-orya", "monospace");
  pref("font.name-list.cursive.x-orya", "cursive");

  pref("font.name-list.serif.x-sinh", "serif");
  pref("font.name-list.sans-serif.x-sinh", "sans-serif");
  pref("font.name-list.monospace.x-sinh", "monospace");
  pref("font.name-list.cursive.x-sinh", "cursive");

  pref("font.name-list.serif.x-tamil", "serif");
  pref("font.name-list.sans-serif.x-tamil", "sans-serif");
  pref("font.name-list.monospace.x-tamil", "monospace");
  pref("font.name-list.cursive.x-tamil", "cursive");

  pref("font.name-list.serif.x-telu", "serif");
  pref("font.name-list.sans-serif.x-telu", "sans-serif");
  pref("font.name-list.monospace.x-telu", "monospace");
  pref("font.name-list.cursive.x-telu", "cursive");

  pref("font.name-list.serif.x-tibt", "serif");
  pref("font.name-list.sans-serif.x-tibt", "sans-serif");
  pref("font.name-list.monospace.x-tibt", "monospace");
  pref("font.name-list.cursive.x-tibt", "cursive");

  pref("font.name-list.serif.x-unicode", "serif");
  pref("font.name-list.sans-serif.x-unicode", "sans-serif");
  pref("font.name-list.monospace.x-unicode", "monospace");
  pref("font.name-list.cursive.x-unicode", "cursive");
  pref("font.size.monospace.x-unicode", 12);

  pref("font.name-list.serif.x-western", "serif");
  pref("font.name-list.sans-serif.x-western", "sans-serif");
  pref("font.name-list.monospace.x-western", "monospace");
  pref("font.name-list.cursive.x-western", "cursive");
  pref("font.size.monospace.x-western", 12);

  pref("font.name-list.serif.zh-CN", "serif");
  pref("font.name-list.sans-serif.zh-CN", "sans-serif");
  pref("font.name-list.monospace.zh-CN", "monospace");
  pref("font.name-list.cursive.zh-CN", "cursive");

  pref("font.name-list.serif.zh-HK", "serif");
  pref("font.name-list.sans-serif.zh-HK", "sans-serif");
  pref("font.name-list.monospace.zh-HK", "monospace");
  pref("font.name-list.cursive.zh-HK", "cursive");

  pref("font.name-list.serif.zh-TW", "serif");
  pref("font.name-list.sans-serif.zh-TW", "sans-serif");
  pref("font.name-list.monospace.zh-TW", "monospace");
  pref("font.name-list.cursive.zh-TW", "cursive");

  pref("intl.ime.use_simple_context_on_password_field", false);

  pref("intl.ime.hack.uim.using_key_snooper", true);

#endif



pref("browser.zoom.full", false);
pref("toolkit.zoomManager.zoomValues", ".2,.3,.4,.5,.6,.7,.8,.9,1,1.1,1.2,1.3,1.4,1.5,1.6,1.7,1.8,1.9,2,2.2,2.4,2.6,2.8,3,4,5");


pref("image.http.accept", "");


pref("network.tcp.keepalive.enabled", true);
pref("network.tcp.keepalive.idle_time", 600); 
#if defined(XP_UNIX) && !0 || 0
  pref("network.tcp.keepalive.retry_interval", 1); 
#endif
#if defined(XP_UNIX) && !0
  pref("network.tcp.keepalive.probe_count", 4);
#endif

#if defined(MOZ_WIDGET_GTK)
  pref("widget.disable-workspace-management", false);
#endif

pref("browser.region.log", false);
pref("browser.region.network.url", "https://location.services.mozilla.com/v1/country?key=%MOZILLA_API_KEY%");
pref("browser.region.network.scan", false);
pref("browser.region.timeout", 5000);
pref("browser.region.update.enabled", true);

pref("browser.meta_refresh_when_inactive.disabled", false);


pref("network.buffer.cache.count", 24);
pref("network.buffer.cache.size",  32768);

#if !defined(MOZ_WIDGET_GTK)
  pref("full-screen-api.transition-duration.enter", "200 200");
  pref("full-screen-api.transition-duration.leave", "200 200");
#else
  pref("full-screen-api.transition-duration.enter", "0 0");
  pref("full-screen-api.transition-duration.leave", "0 0");
#endif
pref("full-screen-api.transition.timeout", 1000);
pref("full-screen-api.warning.timeout", 3000);
pref("full-screen-api.keyboardlock-warning.timeout", 4000);
pref("full-screen-api.warning.delay", 500);

pref("pointer-lock-api.warning.timeout", 3000);

pref("memory.ghost_window_timeout_seconds", 60);

pref("memory.dump_reports_on_oom", false);

pref("memory.blob_report.stack_frames", 0);

pref("memory_info_dumper.watch_fifo.enabled", false);

pref("network.captive-portal-service.minInterval", 60000); 
pref("network.captive-portal-service.maxInterval", 1500000); 
pref("network.captive-portal-service.backoffFactor", "5.0");
pref("network.captive-portal-service.enabled", false);

pref("network.connectivity-service.enabled", true);
pref("network.connectivity-service.DNSv4.domain", "example.org");
pref("network.connectivity-service.DNSv6.domain", "example.org");
pref("network.connectivity-service.DNS_HTTPS.domain", "cloudflare-dns.com");
pref("network.connectivity-service.IPv4.url", "http://detectportal.firefox.com/success.txt?ipv4");
pref("network.connectivity-service.IPv6.url", "http://detectportal.firefox.com/success.txt?ipv6");

pref("network.trr.uri", "");
pref("network.trr.credentials", "");
pref("network.trr.custom_uri", "");
pref("network.trr.confirmationNS", "example.com");
pref("network.trr.excluded-domains", "");
pref("network.trr.builtin-excluded-domains", "localhost,local");

pref("network.lna.etp.enabled", true);

pref("captivedetect.canonicalURL", "http://detectportal.firefox.com/canonical.html");
pref("captivedetect.canonicalContent", "<meta http-equiv=\"refresh\" content=\"0;url=https://support.mozilla.org/kb/captive-portal\"/>");
pref("captivedetect.maxWaitingTime", 5000);
pref("captivedetect.pollingTime", 3000);
pref("captivedetect.maxRetryCount", 5);

pref("urlclassifier.trackingAnnotationTable", "ads-track-digest256,social-track-digest256,analytics-track-digest256,content-track-digest256");
pref("urlclassifier.trackingAnnotationWhitelistTable", "mozstd-trackwhite-digest256,google-trackwhite-digest256");
pref("urlclassifier.trackingTable", "ads-track-digest256,social-track-digest256,analytics-track-digest256");
pref("urlclassifier.trackingWhitelistTable", "mozstd-trackwhite-digest256,google-trackwhite-digest256");

pref("urlclassifier.features.fingerprinting.blacklistTables", "base-fingerprinting-track-digest256");
pref("urlclassifier.features.fingerprinting.whitelistTables", "mozstd-trackwhite-digest256,google-trackwhite-digest256");
pref("urlclassifier.features.fingerprinting.annotate.blacklistTables", "base-fingerprinting-track-digest256");
pref("urlclassifier.features.fingerprinting.annotate.whitelistTables", "mozstd-trackwhite-digest256,google-trackwhite-digest256");
pref("urlclassifier.features.cryptomining.blacklistTables", "base-cryptomining-track-digest256");
pref("urlclassifier.features.cryptomining.whitelistTables", "mozstd-trackwhite-digest256");
pref("urlclassifier.features.cryptomining.annotate.blacklistTables", "base-cryptomining-track-digest256");
pref("urlclassifier.features.cryptomining.annotate.whitelistTables", "mozstd-trackwhite-digest256");
pref("urlclassifier.features.socialtracking.blacklistTables", "social-tracking-protection-facebook-digest256,social-tracking-protection-linkedin-digest256,social-tracking-protection-twitter-digest256");
pref("urlclassifier.features.socialtracking.whitelistTables", "mozstd-trackwhite-digest256,google-trackwhite-digest256");
pref("urlclassifier.features.socialtracking.annotate.blacklistTables", "social-tracking-protection-facebook-digest256,social-tracking-protection-linkedin-digest256,social-tracking-protection-twitter-digest256");
pref("urlclassifier.features.socialtracking.annotate.whitelistTables", "mozstd-trackwhite-digest256,google-trackwhite-digest256");
pref("urlclassifier.features.emailtracking.blocklistTables", "base-email-track-digest256");
pref("urlclassifier.features.emailtracking.allowlistTables", "mozstd-trackwhite-digest256");
pref("urlclassifier.features.emailtracking.datacollection.blocklistTables", "base-email-track-digest256,content-email-track-digest256");
pref("urlclassifier.features.emailtracking.datacollection.allowlistTables", "mozstd-trackwhite-digest256");
pref("urlclassifier.features.consentmanager.annotate.blocklistTables", "consent-manager-track-digest256");
pref("urlclassifier.features.consentmanager.annotate.allowlistTables", "mozstd-trackwhite-digest256");
pref("urlclassifier.features.antifraud.annotate.blocklistTables", "anti-fraud-track-digest256");
pref("urlclassifier.features.antifraud.annotate.allowlistTables", "mozstd-trackwhite-digest256");

pref("urlclassifier.disallow_completions", "base-track-digest256,mozstd-trackwhite-digest256,content-track-digest256,ads-track-digest256,social-track-digest256,analytics-track-digest256,base-fingerprinting-track-digest256,content-fingerprinting-track-digest256,base-cryptomining-track-digest256,content-cryptomining-track-digest256,fanboyannoyance-ads-digest256,fanboysocial-ads-digest256,easylist-ads-digest256,easyprivacy-ads-digest256,adguard-ads-digest256,social-tracking-protection-digest256,social-tracking-protection-facebook-digest256,social-tracking-protection-linkedin-digest256,social-tracking-protection-twitter-digest256,base-email-track-digest256,content-email-track-digest256,consent-manager-track-digest256,anti-fraud-track-digest256");

pref("urlclassifier.trackingAnnotationSkipURLs", "");
pref("privacy.rejectForeign.allowList", "");

pref("privacy.trackingprotection.emailtracking.webapp.domains", "mail.163.com,mail.aol.com,fastmail.com,webmail.gandi.net,mail.google.com,navigator-bs.gmx.com,app.hey.com,horde.org/apps/webmail,hushmail.com,icloud.com/mail,kolabnow.com,laposte.net/accueil,mail.lycos.com,mail.com/mail/,mail.ru,mailfence.com,outlook.live.com,email-postaci.com/,posteo.de,mail.protonmail.com,app.rackspace.com,mail.rediff.com,emailmg.ipage.com,runbox.com,mail.sina.com.cn,tutanota.com,mail.yahoo.com,mail.yandex.com,mail.zimbra.com,zoho.com/mail/");

pref("privacy.trackingprotection.allow_list.hasMigratedCategoryPrefs", false);

pref("privacy.trackingprotection.allow_list.hasUserInteractedWithETPSettings", false);


pref("browser.search.log", false);
pref("browser.search.update", true);
pref("browser.search.suggest.enabled", true);
pref("browser.search.suggest.enabled.private", false);
pref("browser.search.separatePrivateDefault", true);
pref("browser.search.separatePrivateDefault.ui.enabled", false);
pref("browser.search.removeEngineInfobar.enabled", true);

pref("memory.report_concurrency", 10);

pref("toolkit.download.loglevel", "Error");

pref("toolkit.pageThumbs.screenSizeDivisor", 7);
pref("toolkit.pageThumbs.minWidth", 0);
pref("toolkit.pageThumbs.minHeight", 0);

pref("dom.input.fallbackUploadDir", "");

pref("plugins.rewrite_youtube_embeds", true);

pref("media.default_volume", "1.0");

pref("browser.storageManager.pressureNotification.minIntervalMS", 1200000);
pref("browser.storageManager.pressureNotification.usageThresholdGB", 5);

pref("browser.sanitizer.loglevel", "Warn");

pref("prompts.authentication_dialog_abuse_limit", 2);

pref("dom.payments.request.supportedRegions", "US,CA");

pref("toolkit.aboutProcesses.showAllSubframes", false);
#if defined(NIGHTLY_BUILD)
  pref("toolkit.aboutProcesses.showThreads", true);
#else
  pref("toolkit.aboutProcesses.showThreads", false);
#endif
pref("toolkit.legacyUserProfileCustomizations.stylesheets", false);

pref("services.common.log.logger.rest.request", "Debug");
pref("services.common.log.logger.rest.response", "Debug");
pref("services.common.log.logger.tokenserverclient", "Debug");



#if defined(NIGHTLY_BUILD) || defined(MOZ_DEV_EDITION)
pref("dom.postMessage.sharedArrayBuffer.bypassCOOP_COEP.insecure.enabled", false);
#else
pref("dom.postMessage.sharedArrayBuffer.bypassCOOP_COEP.insecure.enabled", false, locked);
#endif

pref("security.storage.encryption.sqlite.enabled", false, locked);

pref("privacy.query_stripping.listService.logLevel", "Error");

pref("privacy.fingerprintingProtection.WebCompatService.logLevel", "Error");
pref("captchadetection.loglevel", "Warn");
pref("captchadetection.actor.enabled", true);

pref("general.smoothScroll", true, sticky);
