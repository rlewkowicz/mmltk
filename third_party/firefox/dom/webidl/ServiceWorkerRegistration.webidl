/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * http://slightlyoff.github.io/ServiceWorker/spec/service_worker/index.html
 * https://wicg.github.io/cookie-store/#idl-index
 */

[Func="ServiceWorkersEnabled",
 Exposed=(Window,Worker)]
interface ServiceWorkerRegistration : EventTarget {
  readonly attribute ServiceWorker? installing;
  readonly attribute ServiceWorker? waiting;
  readonly attribute ServiceWorker? active;

  [Pref="dom.serviceWorkers.navigationPreload.enabled", SameObject]
  readonly attribute NavigationPreloadManager navigationPreload;

  readonly attribute USVString scope;
  [Throws]
  readonly attribute ServiceWorkerUpdateViaCache updateViaCache;

  [Throws, NewObject]
  Promise<undefined> update();

  [Throws, NewObject]
  Promise<boolean> unregister();

  // event
  attribute EventHandler onupdatefound;
};

enum ServiceWorkerUpdateViaCache {
  "imports",
  "all",
  "none"
};

partial interface ServiceWorkerRegistration {
  [Throws, SameObject, Exposed=(ServiceWorker,Window), Pref="dom.cookieStore.enabled"]
  readonly attribute CookieStoreManager cookies;
};
