/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "gNetworkLinkService",
  "@mozilla.org/network/network-link-service;1",
  Ci.nsINetworkLinkService
);

ChromeUtils.defineESModuleGetters(lazy, {
  DoHConfigController: "moz-src:///toolkit/components/doh/DoHConfig.sys.mjs",
});

const GLOBAL_CANARY = "use-application-dns.net.";

const NXDOMAIN_ERR = "NS_ERROR_UNKNOWN_HOST";

export const Heuristics = {
  ENABLE_DOH: "enable_doh",
  DISABLE_DOH: "disable_doh",

  async run() {
    let [safeSearchChecks, zscaler, canary] = await Promise.all([
      safeSearch(),
      zscalerCanary(),
      globalCanary(),
    ]);

    let platformChecks = await platform();
    let results = {
      google: safeSearchChecks.google,
      youtube: safeSearchChecks.youtube,
      zscalerCanary: zscaler,
      canary,
      policy: await enterprisePolicy(),
      vpn: platformChecks.vpn,
      proxy: platformChecks.proxy,
      nrpt: platformChecks.nrpt,
      steeredProvider: "",
    };

    if (Object.values(results).includes("disable_doh")) {
      return results;
    }

    results.steeredProvider = (await providerSteering()) || "";
    return results;
  },

  async checkEnterprisePolicy() {
    return enterprisePolicy();
  },

  async _setMockLinkService(mockLinkService) {
    this.mockLinkService = mockLinkService;
  },

  heuristicNameToSkipReason(heuristicName) {
    const namesToSkipReason = {
      google: Ci.nsITRRSkipReason.TRR_HEURISTIC_TRIPPED_GOOGLE_SAFESEARCH,
      youtube: Ci.nsITRRSkipReason.TRR_HEURISTIC_TRIPPED_YOUTUBE_SAFESEARCH,
      zscalerCanary: Ci.nsITRRSkipReason.TRR_HEURISTIC_TRIPPED_ZSCALER_CANARY,
      canary: Ci.nsITRRSkipReason.TRR_HEURISTIC_TRIPPED_CANARY,
      modifiedRoots: Ci.nsITRRSkipReason.TRR_HEURISTIC_TRIPPED_MODIFIED_ROOTS,
      policy: Ci.nsITRRSkipReason.TRR_HEURISTIC_TRIPPED_ENTERPRISE_POLICY,
      vpn: Ci.nsITRRSkipReason.TRR_HEURISTIC_TRIPPED_VPN,
      proxy: Ci.nsITRRSkipReason.TRR_HEURISTIC_TRIPPED_PROXY,
      nrpt: Ci.nsITRRSkipReason.TRR_HEURISTIC_TRIPPED_NRPT,
    };

    let value = namesToSkipReason[heuristicName];
    if (value != undefined) {
      return value;
    }
    return Ci.nsITRRSkipReason.TRR_FAILED;
  },

  Telemetry: {
    incomplete: 0,
    pass: 1,
    optOut: 2,
    manuallyDisabled: 3,
    manuallyEnabled: 4,
    enterpriseDisabled: 5,
    enterprisePresent: 6,
    enterpriseEnabled: 7,
    vpn: 8,
    proxy: 9,
    nrpt: 10,
    modifiedRoots: 12,
    thirdPartyRoots: 13,
    google: 14,
    youtube: 15,
    zscalerCanary: 16,
    canary: 17,
    ignored: 18,

    heuristicNames() {
      return [
        "google",
        "youtube",
        "zscalerCanary",
        "canary",
        "thirdPartyRoots",
        "policy",
        "vpn",
        "proxy",
        "nrpt",
      ];
    },

    fromResults(results) {
      for (let label of Heuristics.Telemetry.heuristicNames()) {
        if (results[label] == Heuristics.DISABLE_DOH) {
          return Heuristics.Telemetry[label];
        }
      }
      return Heuristics.Telemetry.pass;
    },
  },
};

async function dnsLookup(hostname, resolveCanonicalName = false) {
  let lookupPromise = new Promise((resolve, reject) => {
    let request;
    let response = {
      addresses: [],
    };
    let listener = {
      onLookupComplete(inRequest, inRecord, inStatus) {
        if (inRequest === request) {
          if (!Components.isSuccessCode(inStatus)) {
            reject({ message: new Components.Exception("", inStatus).name });
            return;
          }
          inRecord.QueryInterface(Ci.nsIDNSAddrRecord);
          if (resolveCanonicalName) {
            try {
              response.canonicalName = inRecord.canonicalName;
            } catch (e) {
            }
          }
          while (inRecord.hasMore()) {
            let addr = inRecord.getNextAddrAsString();
            if (!response.addresses.includes(addr)) {
              response.addresses.push(addr);
            }
          }
          resolve(response);
        }
      },
    };
    let dnsFlags =
      Ci.nsIDNSService.RESOLVE_TRR_DISABLED_MODE |
      Ci.nsIDNSService.RESOLVE_DISABLE_IPV6 |
      Ci.nsIDNSService.RESOLVE_BYPASS_CACHE |
      Ci.nsIDNSService.RESOLVE_CANONICAL_NAME;
    try {
      request = Services.dns.asyncResolve(
        hostname,
        Ci.nsIDNSService.RESOLVE_TYPE_DEFAULT,
        dnsFlags,
        null,
        listener,
        null,
        {} 
      );
    } catch (e) {
      reject({ message: e.name });
    }
  });

  let addresses, canonicalName, err;

  try {
    let response = await lookupPromise;
    addresses = response.addresses;
    canonicalName = response.canonicalName;
  } catch (e) {
    addresses = [null];
    err = e.message;
  }

  return { addresses, canonicalName, err };
}

async function dnsListLookup(domainList) {
  let results = [];

  let resolutions = await Promise.all(
    domainList.map(domain => dnsLookup(domain))
  );
  for (let { addresses } of resolutions) {
    results = results.concat(addresses);
  }

  return results;
}

export async function globalCanary() {
  async function preconditionCheck(domain) {
    let { addresses, err } = await dnsLookup(domain);
    if (err === NXDOMAIN_ERR || !addresses.length) {
      return false;
    }
    return true;
  }

  let preCheckSuccess = await preconditionCheck("firefox.com.");
  if (!preCheckSuccess) {
    return "enable_doh";
  }

  let { addresses, err } = await dnsLookup(GLOBAL_CANARY);

  let postCheckSuccess = await preconditionCheck("mozilla.org.");
  if (!postCheckSuccess) {
    return "enable_doh";
  }

  function isLocal(addr) {
    if (addr == "127.0.0.1" || addr == "::1" || addr == "0.0.0.0") {
      return true;
    }
    return Services.io.hostnameIsLocalIPAddress(
      Services.io.newURI(`http://${addr}`)
    );
  }

  if (
    err === NXDOMAIN_ERR ||
    !addresses.length ||
    addresses.every(addr => isLocal(addr))
  ) {
    return "disable_doh";
  }

  return "enable_doh";
}

async function enterprisePolicy() {
  if (!Services.policies) {
    return "no_policy_set";
  }

  if (Services.policies.status === Services.policies.ACTIVE) {
    let policies = Services.policies.getActivePolicies();

    if (!policies.hasOwnProperty("DNSOverHTTPS")) {
      return "policy_without_doh";
    }

    if (policies.DNSOverHTTPS.Enabled === true) {
      return "enable_doh";
    }

    return "disable_doh";
  }

  return "no_policy_set";
}

async function safeSearch() {
  const providerList = [
    {
      name: "google",
      unfiltered: ["www.google.com.", "google.com."],
      safeSearch: ["forcesafesearch.google.com."],
    },
    {
      name: "youtube",
      unfiltered: [
        "www.youtube.com.",
        "m.youtube.com.",
        "youtubei.googleapis.com.",
        "youtube.googleapis.com.",
        "www.youtube-nocookie.com.",
      ],
      safeSearch: ["restrict.youtube.com.", "restrictmoderate.youtube.com."],
    },
  ];

  async function checkProvider(provider) {
    let [unfilteredAnswers, safeSearchAnswers] = await Promise.all([
      dnsListLookup(provider.unfiltered),
      dnsListLookup(provider.safeSearch),
    ]);

    for (let answer of safeSearchAnswers) {
      if (answer && unfilteredAnswers.includes(answer)) {
        return { name: provider.name, result: "disable_doh" };
      }
    }

    return { name: provider.name, result: "enable_doh" };
  }

  let resolutions = await Promise.all(
    providerList.map(provider => checkProvider(provider))
  );

  return resolutions.reduce(
    (accumulator, check) => {
      accumulator[check.name] = check.result;
      return accumulator;
    },
    {} 
  );
}

async function zscalerCanary() {
  const ZSCALER_CANARY = "sitereview.zscaler.com.";

  let { addresses } = await dnsLookup(ZSCALER_CANARY);
  for (let address of addresses) {
    if (
      ["213.152.228.242", "199.168.151.251", "8.25.203.30"].includes(address)
    ) {
      return "disable_doh";
    }
  }

  return "enable_doh";
}

async function platform() {
  let platformChecks = {};

  let indications = Ci.nsINetworkLinkService.NONE_DETECTED;
  try {
    let linkService = lazy.gNetworkLinkService;
    if (Heuristics.mockLinkService) {
      linkService = Heuristics.mockLinkService;
    }
    indications = linkService.platformDNSIndications;
  } catch (e) {
    if (e.result != Cr.NS_ERROR_NOT_IMPLEMENTED) {
      console.error(e);
    }
  }

  platformChecks.vpn =
    indications & Ci.nsINetworkLinkService.VPN_DETECTED
      ? "disable_doh"
      : "enable_doh";
  platformChecks.proxy =
    indications & Ci.nsINetworkLinkService.PROXY_DETECTED
      ? "disable_doh"
      : "enable_doh";
  platformChecks.nrpt =
    indications & Ci.nsINetworkLinkService.NRPT_DETECTED
      ? "disable_doh"
      : "enable_doh";

  return platformChecks;
}

async function providerSteering() {
  if (!lazy.DoHConfigController.currentConfig.providerSteering.enabled) {
    return null;
  }
  const TEST_DOMAIN = "doh.test.";

  let steeredProviders =
    lazy.DoHConfigController.currentConfig.providerSteering.providerList;

  if (!steeredProviders || !steeredProviders.length) {
    return null;
  }

  let { canonicalName, err } = await dnsLookup(TEST_DOMAIN, true);
  if (err || !canonicalName) {
    return null;
  }

  let provider = steeredProviders.find(p => {
    return p.canonicalName == canonicalName;
  });
  if (!provider || !provider.uri || !provider.id) {
    return null;
  }

  return provider;
}
