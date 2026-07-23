/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

document.addEventListener("DOMContentLoaded", () => {
  if (!RPMIsWindowPrivate()) {
    document.documentElement.classList.remove("private");
    document.documentElement.classList.add("normal");
    document
      .getElementById("startPrivateBrowsing")
      .addEventListener("click", () =>
        RPMSendAsyncMessage("OpenPrivateWindow")
      );
    return;
  }

  document.getElementById("private-browsing-myths").href =
    RPMGetFormatURLPref("app.support.baseURL") + "private-browsing-myths";
  document.documentElement.setAttribute("PrivateBrowsingRenderComplete", true);
});
