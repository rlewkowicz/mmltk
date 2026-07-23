/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "gStringBundle", function () {
  return Services.strings.createBundle(
    "chrome://browser/locale/siteData.properties"
  );
});

ChromeUtils.defineLazyGetter(lazy, "gBrandBundle", function () {
  return Services.strings.createBundle(
    "chrome://branding/locale/brand.properties"
  );
});

ChromeUtils.defineESModuleGetters(lazy, {
  Sanitizer: "resource:///modules/Sanitizer.sys.mjs",
});

export var SiteDataManager = {
  _sites: new Map(),

  _getCacheSizeObserver: null,

  _getCacheSizePromise: null,

  _getQuotaUsagePromise: null,

  _quotaUsageRequest: null,

  async updateSites(entryUpdatedCallback) {
    Services.obs.notifyObservers(null, "sitedatamanager:updating-sites");
    this._sites.clear();
    this._getAllCookies(entryUpdatedCallback);
    await this._getQuotaUsage(entryUpdatedCallback);
    Services.obs.notifyObservers(null, "sitedatamanager:sites-updated");
  },

  getBaseDomainFromHost(host) {
    let result = host;
    try {
      result = Services.eTLD.getBaseDomainFromHost(host);
    } catch (e) {
      if (
        e.result == Cr.NS_ERROR_HOST_IS_IP_ADDRESS ||
        e.result == Cr.NS_ERROR_INSUFFICIENT_DOMAIN_LEVELS
      ) {
        result = host;
      } else {
        throw e;
      }
    }
    return result;
  },

  _getOrInsertSite(baseDomainOrHost) {
    let site = this._sites.get(baseDomainOrHost);
    if (!site) {
      site = {
        baseDomainOrHost,
        cookies: [],
        persisted: false,
        quotaUsage: 0,
        lastAccessed: 0,
        principals: [],
      };
      this._sites.set(baseDomainOrHost, site);
    }
    return site;
  },

  _testInsertSite(
    baseDomainOrHost,
    {
      cookies = [],
      persisted = false,
      quotaUsage = 0,
      lastAccessed = 0,
      principals = [],
    }
  ) {
    let site = {
      baseDomainOrHost,
      cookies,
      persisted,
      quotaUsage,
      lastAccessed,
      principals,
    };
    this._sites.set(baseDomainOrHost, site);

    return site;
  },

  _getOrInsertContainersData(site, userContextId) {
    if (!site.containersData) {
      site.containersData = new Map();
    }

    let containerData = site.containersData.get(userContextId);
    if (!containerData) {
      containerData = {
        cookiesBlocked: 0,
        lastAccessed: new Date(0),
        quotaUsage: 0,
      };
      site.containersData.set(userContextId, containerData);
    }
    return containerData;
  },

  getCacheSize() {
    if (this._getCacheSizePromise) {
      return this._getCacheSizePromise;
    }

    this._getCacheSizePromise = new Promise((resolve, reject) => {
      this._getCacheSizeObserver = {
        onNetworkCacheDiskConsumption: consumption => {
          resolve(consumption);
          this._getCacheSizePromise = null;
          this._getCacheSizeObserver = null;
        },

        QueryInterface: ChromeUtils.generateQI([
          "nsICacheStorageConsumptionObserver",
          "nsISupportsWeakReference",
        ]),
      };

      try {
        Services.cache2.asyncGetDiskConsumption(this._getCacheSizeObserver);
      } catch (e) {
        reject(e);
        this._getCacheSizePromise = null;
        this._getCacheSizeObserver = null;
      }
    });

    return this._getCacheSizePromise;
  },

  _getQuotaUsage(entryUpdatedCallback) {
    this._cancelGetQuotaUsage();
    this._getQuotaUsagePromise = new Promise(resolve => {
      let onUsageResult = request => {
        if (request.resultCode == Cr.NS_OK) {
          let items = request.result;
          for (let item of items) {
            if (!item.persisted && item.usage <= 0) {
              continue;
            }
            let principal =
              Services.scriptSecurityManager.createContentPrincipalFromOrigin(
                item.origin
              );
            if (principal.schemeIs("http") || principal.schemeIs("https")) {
              let pkBaseDomain;
              try {
                pkBaseDomain = ChromeUtils.getBaseDomainFromPartitionKey(
                  principal.originAttributes.partitionKey
                );
              } catch (e) {
                console.error(e);
              }
              let site = this._getOrInsertSite(
                pkBaseDomain || principal.baseDomain
              );
              if (item.persisted) {
                site.persisted = true;
              }
              if (site.lastAccessed < item.lastAccessed) {
                site.lastAccessed = item.lastAccessed;
              }
              if (Number.isInteger(principal.userContextId)) {
                let containerData = this._getOrInsertContainersData(
                  site,
                  principal.userContextId
                );
                containerData.quotaUsage = item.usage;
                let itemTime = item.lastAccessed / 1000;
                if (containerData.lastAccessed.getTime() < itemTime) {
                  containerData.lastAccessed.setTime(itemTime);
                }
              }
              site.principals.push(principal);
              site.quotaUsage += item.usage;
              if (entryUpdatedCallback) {
                entryUpdatedCallback(principal.baseDomain, site);
              }
            }
          }
        }
        resolve();
      };
      this._quotaUsageRequest = Services.qms.getUsage(onUsageResult);
    });
    return this._getQuotaUsagePromise;
  },

  _getAllCookies(entryUpdatedCallback) {
    for (let cookie of Services.cookies.cookies) {
      let pkBaseDomain;
      try {
        pkBaseDomain = ChromeUtils.getBaseDomainFromPartitionKey(
          cookie.originAttributes.partitionKey
        );
      } catch (e) {
        console.error(e);
      }
      let baseDomainOrHost =
        pkBaseDomain || this.getBaseDomainFromHost(cookie.rawHost);
      let site = this._getOrInsertSite(baseDomainOrHost);
      if (entryUpdatedCallback) {
        entryUpdatedCallback(baseDomainOrHost, site);
      }
      site.cookies.push(cookie);
      if (Number.isInteger(cookie.originAttributes.userContextId)) {
        let containerData = this._getOrInsertContainersData(
          site,
          cookie.originAttributes.userContextId
        );
        containerData.cookiesBlocked += 1;
        let cookieTime = cookie.lastAccessed / 1000;
        if (containerData.lastAccessed.getTime() < cookieTime) {
          containerData.lastAccessed.setTime(cookieTime);
        }
      }
      if (site.lastAccessed < cookie.lastAccessed) {
        site.lastAccessed = cookie.lastAccessed;
      }
    }
  },

  _cancelGetQuotaUsage() {
    if (this._quotaUsageRequest) {
      this._quotaUsageRequest.cancel();
      this._quotaUsageRequest = null;
    }
  },

  async hasSiteData(asciiHost) {
    if (
      Services.cookies.hasCookiesForSite(
        asciiHost,
        JSON.stringify({ privateBrowsingId: 0 })
      )
    ) {
      return true;
    }

    let hasQuota = await new Promise(resolve => {
      Services.qms.getUsage(request => {
        if (request.resultCode != Cr.NS_OK) {
          resolve(false);
          return;
        }

        for (let item of request.result) {
          if (!item.persisted && item.usage <= 0) {
            continue;
          }

          let principal =
            Services.scriptSecurityManager.createContentPrincipalFromOrigin(
              item.origin
            );
          if (principal.asciiHost == asciiHost) {
            resolve(true);
            return;
          }
        }

        resolve(false);
      });
    });

    if (hasQuota) {
      return true;
    }

    return false;
  },

  getTotalUsage() {
    return this._getQuotaUsagePromise.then(() => {
      let usage = 0;
      for (let site of this._sites.values()) {
        usage += site.quotaUsage;
      }
      return usage;
    });
  },

  async getQuotaUsageForTimeRanges(timeSpanArr) {
    let usage = {};
    await this._getQuotaUsagePromise;

    for (let timespan of timeSpanArr) {
      usage[timespan] = 0;
    }

    let timeNow = Date.now();
    for (let site of this._sites.values()) {
      let lastAccessed = new Date(site.lastAccessed / 1000);
      for (let timeSpan of timeSpanArr) {
        let compareTime = new Date(
          timeNow - lazy.Sanitizer.timeSpanMsMap[timeSpan]
        );

        if (timeSpan === "TIMESPAN_EVERYTHING") {
          usage[timeSpan] += site.quotaUsage;
        } else if (lastAccessed >= compareTime) {
          usage[timeSpan] += site.quotaUsage;
        }
      }
    }
    return usage;
  },

  async getSites() {
    await this._getQuotaUsagePromise;

    return Array.from(this._sites.values()).map(site => ({
      baseDomain: site.baseDomainOrHost,
      cookies: site.cookies,
      usage: site.quotaUsage,
      containersData: site.containersData,
      persisted: site.persisted,
      lastAccessed: new Date(site.lastAccessed / 1000),
    }));
  },

  async getSite(baseDomainOrHost) {
    let baseDomain = this.getBaseDomainFromHost(baseDomainOrHost);

    let site = this._sites.get(baseDomain);
    if (!site) {
      return null;
    }
    return {
      baseDomain: site.baseDomainOrHost,
      cookies: site.cookies,
      usage: site.quotaUsage,
      containersData: site.containersData,
      persisted: site.persisted,
      lastAccessed: new Date(site.lastAccessed / 1000),
    };
  },

  _removePermission(site) {
    let removals = new Set();
    for (let principal of site.principals) {
      let { originNoSuffix } = principal;
      if (removals.has(originNoSuffix)) {
        continue;
      }
      removals.add(originNoSuffix);
      Services.perms.removeFromPrincipal(principal, "persistent-storage");
    }
  },

  _removeCookies(site) {
    for (let cookie of site.cookies) {
      Services.cookies.remove(
        cookie.host,
        cookie.name,
        cookie.path,
        cookie.originAttributes
      );
    }
    site.cookies = [];
  },

  async remove(domainsOrHosts) {
    if (domainsOrHosts == null) {
      throw new Error("domainsOrHosts is required.");
    }
    if (!Array.isArray(domainsOrHosts)) {
      domainsOrHosts = [domainsOrHosts];
    }

    let promises = [];
    for (let domainOrHost of domainsOrHosts) {
      promises.push(
        new Promise(function (resolve) {
          const { clearData } = Services;
          if (domainOrHost) {
            let schemelessSite =
              Services.eTLD.getSchemelessSiteFromHost(domainOrHost);
            clearData.deleteDataFromSite(
              schemelessSite,
              {},
              true,
              Ci.nsIClearDataService.CLEAR_COOKIES_AND_SITE_DATA |
                Ci.nsIClearDataService.CLEAR_ALL_CACHES,
              resolve
            );
          } else {
            clearData.deleteDataFromLocalFiles(
              true,
              Ci.nsIClearDataService.CLEAR_COOKIES_AND_SITE_DATA |
                Ci.nsIClearDataService.CLEAR_ALL_CACHES,
              resolve
            );
          }
        })
      );
    }

    await Promise.all(promises);

    return this.updateSites();
  },

  promptSiteDataRemoval(win, removals) {
    if (removals) {
      let args = {
        hosts: removals,
        allowed: false,
      };
      let features = "centerscreen,chrome,modal,resizable=no";
      win.browsingContext.topChromeWindow.openDialog(
        "chrome://browser/content/preferences/dialogs/siteDataRemoveSelected.xhtml",
        "",
        features,
        args
      );
      return args.allowed;
    }

    let brandName = lazy.gBrandBundle.GetStringFromName("brandShortName");
    let flags =
      Services.prompt.BUTTON_TITLE_IS_STRING * Services.prompt.BUTTON_POS_0 +
      Services.prompt.BUTTON_TITLE_CANCEL * Services.prompt.BUTTON_POS_1 +
      Services.prompt.BUTTON_POS_0_DEFAULT;
    let title = lazy.gStringBundle.GetStringFromName(
      "clearSiteDataPromptTitle"
    );
    let text = lazy.gStringBundle.formatStringFromName(
      "clearSiteDataPromptText",
      [brandName]
    );
    let btn0Label = lazy.gStringBundle.GetStringFromName("clearSiteDataNow");

    let result = Services.prompt.confirmEx(
      win,
      title,
      text,
      flags,
      btn0Label,
      null,
      null,
      null,
      {}
    );
    return result == 0;
  },

  async removeAll() {
    await this.removeCache();
    return this.removeSiteData();
  },

  removeCache() {
    return new Promise(function (resolve) {
      Services.clearData.deleteData(
        Ci.nsIClearDataService.CLEAR_ALL_CACHES,
        resolve
      );
    });
  },

  async removeSiteData() {
    await new Promise(function (resolve) {
      Services.clearData.deleteData(
        Ci.nsIClearDataService.CLEAR_COOKIES_AND_SITE_DATA,
        resolve
      );
    });

    return this.updateSites();
  },
};
