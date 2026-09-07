/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { RemotePageChild } from "resource://gre/actors/RemotePageChild.sys.mjs";

export class AboutHttpsOnlyErrorChild extends RemotePageChild {
  actorCreated() {
    super.actorCreated();

    const exportableFunctions = [
      "RPMTryPingSecureWWWLink",
      "RPMOpenSecureWWWLink",
    ];
    this.exportFunctions(exportableFunctions);
  }

  RPMTryPingSecureWWWLink() {

    const httpsOnlySuggestionPref = Services.prefs.getBoolPref(
      "dom.security.https_only_mode_error_page_user_suggestions"
    );

    if (!httpsOnlySuggestionPref) {
      return;
    }

    const wwwURL = "https://www." + this.contentWindow.location.host;
    fetch(wwwURL, {
      credentials: "omit",
      cache: "no-store",
    })
      .then(data => {
        if (data.status === 200) {
          this.contentWindow.dispatchEvent(
            new this.contentWindow.CustomEvent("pingSecureWWWLinkSuccess")
          );
        }
      })
      .catch(() => {
        dump("No secure www suggestion possible for " + wwwURL);
      });
  }

  RPMOpenSecureWWWLink() {
    const context = this.manager.browsingContext;
    const docShell = context.docShell;
    const httpChannel = docShell.failedChannel.QueryInterface(
      Ci.nsIHttpChannel
    );
    const webNav = docShell.QueryInterface(Ci.nsIWebNavigation);
    const triggeringPrincipal =
      docShell.failedChannel.loadInfo.triggeringPrincipal;
    const oldURI = httpChannel.URI;
    const newWWWURI = oldURI
      .mutate()
      .setHost("www." + oldURI.host)
      .finalize();

    webNav.loadURI(newWWWURI, {
      triggeringPrincipal,
      loadFlags: Ci.nsIWebNavigation.LOAD_FLAGS_REPLACE_HISTORY,
    });
  }
}
