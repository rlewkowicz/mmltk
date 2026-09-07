/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const nsIFactoryQI = ChromeUtils.generateQI(["nsIFactory"]);

export var ComponentUtils = {
  generateSingletonFactory(aServiceConstructor) {
    return {
      _instance: null,
      createInstance(aIID) {
        if (this._instance === null) {
          this._instance = new aServiceConstructor();
        }
        return this._instance.QueryInterface(aIID);
      },
      QueryInterface: nsIFactoryQI,
    };
  },
};
