/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export const SitePolicyUtils = {
  isAllowedForURI(manager, sitePolicies, feature, uri) {
    for (let policies of sitePolicies) {
      if (
        policies.exceptions.matches(uri) ||
        policies.exceptions.matchesAllWebUrls
      ) {
        continue;
      }

      if (
        !policies.match.matches(uri) &&
        !policies.match.matchesAllWebUrls
      ) {
        continue;
      }

      if (feature in policies.features) {
        return policies.features[feature];
      }
    }

    return manager.isAllowed(feature);
  },
};
