/**
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

export class CredentialChooserService {
  classID = Components.ID("{673ddc19-03e2-4b30-a868-06297e8fed89}");
  QueryInterface = ChromeUtils.generateQI(["nsICredentialChooserService"]);

  async fetchImageToDataURI(window, uri) {
    if (uri.protocol === "data:") {
      return uri.href;
    }
    let request = new window.Request(uri.spec, { mode: "cors" });
    request.overrideContentPolicyType(Ci.nsIContentPolicy.TYPE_IMAGE);
    let blob;
    let response = await window.fetch(request);
    if (!response.ok) {
      return Promise.reject(new Error("HTTP failure on Fetch"));
    }
    blob = await response.blob();
    return new Promise((resolve, reject) => {
      let reader = new FileReader();
      reader.onloadend = () => resolve(reader.result);
      reader.onerror = reject;
      reader.readAsDataURL(blob);
    });
  }

  async fetchWellKnown(uri, triggeringPrincipal) {
    let request = new Request(uri.spec, {
      mode: "no-cors",
      referrerPolicy: "no-referrer",
      triggeringPrincipal: Services.scriptSecurityManager.createNullPrincipal(
        triggeringPrincipal.originAttributes
      ),
      neverTaint: true,
      credentials: "omit",
      headers: [["Accept", "application/json"]],
    });
    request.overrideContentPolicyType(Ci.nsIContentPolicy.TYPE_WEB_IDENTITY);
    try {
      let response = await fetch(request);
      return response.json();
    } catch (e) {
      return Promise.reject(e);
    }
  }

  async fetchConfig(uri, triggeringPrincipal) {
    let request = new Request(uri.spec, {
      mode: "no-cors",
      referrerPolicy: "no-referrer",
      redirect: "error",
      triggeringPrincipal: Services.scriptSecurityManager.createNullPrincipal(
        triggeringPrincipal.originAttributes
      ),
      neverTaint: true,
      credentials: "omit",
      headers: [["Accept", "application/json"]],
    });
    request.overrideContentPolicyType(Ci.nsIContentPolicy.TYPE_WEB_IDENTITY);
    try {
      let response = await fetch(request);
      return response.json();
    } catch (e) {
      return Promise.reject(e);
    }
  }

  async fetchAccounts(uri, triggeringPrincipal) {
    let request = new Request(uri.spec, {
      mode: "no-cors",
      redirect: "error",
      referrerPolicy: "no-referrer",
      triggeringPrincipal,
      neverTaint: true,
      credentials: "include",
      headers: [["Accept", "application/json"]],
    });
    request.overrideContentPolicyType(Ci.nsIContentPolicy.TYPE_WEB_IDENTITY);
    try {
      let response = await fetch(request);
      return response.json();
    } catch (e) {
      return Promise.reject(e);
    }
  }

  async fetchToken(uri, body, triggeringPrincipal) {
    let request = new Request(uri.spec, {
      mode: "cors",
      method: "POST",
      redirect: "error",
      triggeringPrincipal,
      body,
      credentials: "include",
      headers: [["Content-type", "application/x-www-form-urlencoded"]],
    });
    request.overrideContentPolicyType(Ci.nsIContentPolicy.TYPE_WEB_IDENTITY);
    try {
      let response = await fetch(request);
      return response.json();
    } catch (e) {
      return Promise.reject(e);
    }
  }

  async fetchDisconnect(uri, body, triggeringPrincipal) {
    let request = new Request(uri.spec, {
      mode: "cors",
      method: "POST",
      redirect: "error",
      triggeringPrincipal,
      body,
      credentials: "include",
      headers: [["Content-type", "application/x-www-form-urlencoded"]],
    });
    request.overrideContentPolicyType(Ci.nsIContentPolicy.TYPE_WEB_IDENTITY);
    try {
      let response = await fetch(request);
      return response.json();
    } catch (e) {
      return Promise.reject(e);
    }
  }
}
