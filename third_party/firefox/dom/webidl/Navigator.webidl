/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://html.spec.whatwg.org/#the-navigator-object
 * http://www.w3.org/TR/tracking-dnt/
 * http://www.w3.org/TR/geolocation-API/#geolocation_interface
 * http://www.w3.org/TR/battery-status/#navigatorbattery-interface
 * http://www.w3.org/TR/vibration/#vibration-interface
 * http://www.w3.org/2012/sysapps/runtime/#extension-to-the-navigator-interface-1
 * https://dvcs.w3.org/hg/gamepad/raw-file/default/gamepad.html#navigator-interface-extension
 * http://www.w3.org/TR/beacon/#sec-beacon-method
 * https://html.spec.whatwg.org/#navigatorconcurrenthardware
 * http://wicg.github.io/netinfo/#extensions-to-the-navigator-interface
 * https://w3c.github.io/webappsec-credential-management/#framework-credential-management
 * https://w3c.github.io/webdriver/webdriver-spec.html#interface
 * https://wicg.github.io/media-capabilities/#idl-index
 * https://w3c.github.io/mediasession/#idl-index
 *
 * © Copyright 2004-2011 Apple Computer, Inc., Mozilla Foundation, and
 * Opera Software ASA. You are granted a license to use, reproduce
 * and create derivative works of this document.
 */

interface URI;

// https://html.spec.whatwg.org/#the-navigator-object
[HeaderFile="Navigator.h",
 Exposed=Window]
interface Navigator {
  // objects implementing this interface also implement the interfaces given below
};
Navigator includes NavigatorID;
Navigator includes NavigatorLanguage;
Navigator includes NavigatorOnLine;
Navigator includes NavigatorStorageUtils;
Navigator includes NavigatorConcurrentHardware;
Navigator includes NavigatorStorage;
Navigator includes NavigatorAutomationInformation;
Navigator includes NavigatorGPU;
Navigator includes GlobalPrivacyControl;

interface mixin NavigatorID {
  // WebKit/Blink/Trident/Presto support this (hardcoded "Mozilla").
  [Constant, Cached, Throws]
  readonly attribute DOMString appCodeName; // constant "Mozilla"
  [Constant, Cached]
  readonly attribute DOMString appName; // constant "Netscape"
  [Constant, Cached, Throws, NeedsCallerType]
  readonly attribute DOMString appVersion;
  [Pure, Cached, Throws, NeedsCallerType]
  readonly attribute DOMString platform;
  [Pure, Cached, Throws, NeedsCallerType]
  readonly attribute DOMString userAgent;
  [Constant, Cached]
  readonly attribute DOMString product; // constant "Gecko"

  // Everyone but WebKit/Blink supports this.  See bug 679971.
  [Exposed=Window]
  boolean taintEnabled(); // constant false
};

interface mixin NavigatorLanguage {

  // These two attributes are cached because this interface is also implemented
  // by Workernavigator and this way we don't have to go back to the
  // main-thread from the worker thread anytime we need to retrieve them. They
  // are updated when pref intl.accept_languages is changed.

  [Pure, Cached]
  readonly attribute DOMString? language;
  [Pure, Cached, Frozen]
  readonly attribute sequence<DOMString> languages;
};

interface mixin NavigatorOnLine {
  readonly attribute boolean onLine;
};

[SecureContext]
interface mixin NavigatorStorage {
  readonly attribute StorageManager storage;
};

interface mixin NavigatorStorageUtils {
  // NOT IMPLEMENTED
  //undefined yieldForStorageUpdates();
};

// https://w3c.github.io/permissions/#webidl-2112232240
partial interface Navigator {
  [Throws, SameObject]
  readonly attribute Permissions permissions;
};

partial interface Navigator {
  [Throws, SameObject]
  readonly attribute MimeTypeArray mimeTypes;
  [Throws, SameObject]
  readonly attribute PluginArray plugins;
  readonly attribute boolean pdfViewerEnabled;
};

// http://www.w3.org/TR/tracking-dnt/ sort of
partial interface Navigator {
  readonly attribute DOMString doNotTrack;
};

// https://globalprivacycontrol.github.io/gpc-spec/
interface mixin GlobalPrivacyControl {
  [Pref="privacy.globalprivacycontrol.functionality.enabled"]
  readonly attribute boolean globalPrivacyControl;
};

// http://www.w3.org/TR/vibration/#vibration-interface
partial interface Navigator {
    // We don't support sequences in unions yet
    //boolean vibrate ((unsigned long or sequence<unsigned long>) pattern);
    [Pref="dom.vibrator.enabled"]
    boolean vibrate(unsigned long duration);
    [Pref="dom.vibrator.enabled"]
    boolean vibrate(sequence<unsigned long> pattern);
};

// http://www.w3.org/TR/pointerevents/#extensions-to-the-navigator-interface
partial interface Navigator {
    [NeedsCallerType]
    readonly attribute long maxTouchPoints;
};

// Mozilla-specific extensions

// Chrome-only interface for Vibration API permission handling.
partial interface Navigator {
    /* Set permission state to device vibration.
     * @param permitted permission state (true for allowing vibration)
     * @param persistent make the permission session-persistent
     */
    [ChromeOnly]
    undefined setVibrationPermission(boolean permitted,
                                     optional boolean persistent = true);
};

partial interface Navigator {
  [Throws, Constant, Cached, NeedsCallerType]
  readonly attribute DOMString oscpu;
  // WebKit/Blink support this; Trident/Presto do not.
  readonly attribute DOMString vendor;
  // WebKit/Blink supports this (hardcoded ""); Trident/Presto do not.
  readonly attribute DOMString vendorSub;
  // WebKit/Blink supports this (hardcoded "20030107"); Trident/Presto don't
  readonly attribute DOMString productSub;
  // WebKit/Blink/Trident/Presto support this.
  readonly attribute boolean cookieEnabled;
  [Throws, Constant, Cached, NeedsCallerType]
  readonly attribute DOMString buildID;

  // WebKit/Blink/Trident/Presto support this.
  [Affects=Nothing, DependsOn=Nothing]
  boolean javaEnabled();
};

// NetworkInformation
partial interface Navigator {
  [Throws, Pref="dom.netinfo.enabled"]
  readonly attribute NetworkInformation connection;
};

// Service Workers/Navigation Controllers
// https://w3c.github.io/ServiceWorker/#navigator-serviceworker
partial interface Navigator {
  [Func="ServiceWorkersEnabled", SameObject]
  readonly attribute ServiceWorkerContainer serviceWorker;
};

partial interface Navigator {
  [Throws, Pref="beacon.enabled"]
  boolean sendBeacon(DOMString url,
                     optional BodyInit? data = null);
};

partial interface Navigator {
  [NewObject, Func="mozilla::dom::TCPSocket::ShouldTCPSocketExist"]
  readonly attribute LegacyMozTCPSocket mozTCPSocket;
};

interface mixin NavigatorConcurrentHardware {
  readonly attribute unsigned long long hardwareConcurrency;
};

// https://w3c.github.io/webdriver/webdriver-spec.html#interface
interface mixin NavigatorAutomationInformation {
  [Constant, Cached]
  readonly attribute boolean webdriver;
};

// https://www.w3.org/TR/clipboard-apis/#navigator-interface
partial interface Navigator {
  [SecureContext, SameObject]
  readonly attribute Clipboard clipboard;
};

// https://w3c.github.io/mediasession/#idl-index
[Exposed=Window]
partial interface Navigator {
  [SameObject]
  readonly attribute MediaSession mediaSession;
};

// https://w3c.github.io/audio-session/
[Exposed=Window]
partial interface Navigator {
  [Pref="dom.audio_session.enabled", SameObject]
  readonly attribute AudioSession audioSession;
};

// https://w3c.github.io/web-locks/#navigator-mixins
[SecureContext]
interface mixin NavigatorLocks {
  readonly attribute LockManager locks;
};
Navigator includes NavigatorLocks;

// https://w3c.github.io/autoplay/#autoplay-policy
enum AutoplayPolicy {
  "allowed",
  "allowed-muted",
  "disallowed"
};

enum AutoplayPolicyMediaType {
  "mediaelement",
  "audiocontext"
};

// https://w3c.github.io/autoplay/#autoplay-detection-methods
partial interface Navigator {
  [Pref="dom.media.autoplay-policy-detection.enabled"]
  AutoplayPolicy getAutoplayPolicy(AutoplayPolicyMediaType type);

  [Pref="dom.media.autoplay-policy-detection.enabled"]
  AutoplayPolicy getAutoplayPolicy(HTMLMediaElement element);

  [Pref="dom.media.autoplay-policy-detection.enabled"]
  AutoplayPolicy getAutoplayPolicy(AudioContext context);
};

// https://html.spec.whatwg.org/multipage/interaction.html#the-useractivation-interface
partial interface Navigator {
  [SameObject] readonly attribute UserActivation userActivation;
};
