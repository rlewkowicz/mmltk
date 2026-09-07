/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { OpenSearchParser } from "moz-src:///toolkit/components/search/OpenSearchParser.sys.mjs";

export class OpenSearchLoaderChild extends JSWindowActorChild {
  receiveMessage({ name, data }) {
    if (name === "OpenSearchLoader:getEngineData") {
      return OpenSearchParser.parseXMLData(data);
    }
    return undefined;
  }
}
