/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export var ForgetAboutSite = {
  async removeDataFromBaseDomain(aDomainOrHost) {
    if (!aDomainOrHost) {
      throw new Error("aDomainOrHost can not be empty.");
    }

    let schemelessSite = Services.eTLD.getSchemelessSiteFromHost(aDomainOrHost);
    let errorCount = await new Promise(resolve => {
      Services.clearData.deleteDataFromSite(
        schemelessSite,
        {},
        true ,
        Ci.nsIClearDataService.CLEAR_FORGET_ABOUT_SITE,
        errorCode => resolve(bitCounting(errorCode))
      );
    });

    if (errorCount !== 0) {
      throw new Error(
        `There were a total of ${errorCount} errors during removal`
      );
    }
  },
};

function bitCounting(value) {
  const count =
    value - ((value >> 1) & 0o33333333333) - ((value >> 2) & 0o11111111111);
  return ((count + (count >> 3)) & 0o30707070707) % 63;
}
