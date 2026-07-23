/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import {
  UrlbarProvider,
  UrlbarUtils,
} from "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs";
import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  ProvidersManager:
    "moz-src:///browser/components/urlbar/UrlbarProvidersManager.sys.mjs",
  UrlbarResult: "chrome://browser/content/urlbar/UrlbarResult.mjs",
  UrlbarShared: "chrome://browser/content/urlbar/UrlbarShared.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "logger", () =>
  lazy.UrlbarShared.getLogger({ prefix: "Provider.OpenTabs" })
);

const PRIVATE_USER_CONTEXT_ID = -1;

var gOpenTabUrls = new Map();

export class UrlbarProviderOpenTabs extends UrlbarProvider {
  constructor() {
    super();
  }

  get type() {
    return UrlbarUtils.PROVIDER_TYPE.PROFILE;
  }

  async isActive() {
    return false;
  }

  static memoryTableInitialized = false;

  static getOpenTabUrlsForUserContextId(
    userContextId,
    isInPrivateWindow = false
  ) {
    userContextId = parseInt(`${userContextId}`);
    userContextId = UrlbarProviderOpenTabs.getUserContextIdForOpenPagesTable(
      userContextId,
      isInPrivateWindow
    );

    let groupEntries = gOpenTabUrls.get(userContextId);
    if (!groupEntries) {
      return [];
    }

    let result = new Set();
    groupEntries.forEach((urls, groupId) => {
      for (let url of urls.keys()) {
        result.add([url, userContextId, groupId]);
      }
    });
    return Array.from(result);
  }

  static getOpenTabUrls(isInPrivateWindow = false) {
    let uniqueUrls = new Map();
    if (isInPrivateWindow) {
      let urlInfo = UrlbarProviderOpenTabs.getOpenTabUrlsForUserContextId(
        PRIVATE_USER_CONTEXT_ID,
        true
      );
      for (let [url, contextId, groupId] of urlInfo) {
        uniqueUrls.set(url, new Set([[contextId, groupId]]));
      }
    } else {
      gOpenTabUrls.forEach((groups, userContextId) => {
        if (userContextId == PRIVATE_USER_CONTEXT_ID) {
          return;
        }

        groups.forEach((urls, groupId) => {
          for (let url of urls.keys()) {
            let userContextAndGroupIds = uniqueUrls.get(url);
            if (!userContextAndGroupIds) {
              userContextAndGroupIds = new Set();
              uniqueUrls.set(url, userContextAndGroupIds);
            }
            userContextAndGroupIds.add([userContextId, groupId]);
          }
        });
      });
    }
    return uniqueUrls;
  }

  static async getDatabaseRegisteredOpenTabsForTests() {
    let conn = await lazy.PlacesUtils.promiseLargeCacheDBConnection();
    let rows = await conn.execute(
      "SELECT url, userContextId, NULLIF(groupId, '') groupId, open_count" +
        " FROM moz_openpages_temp ORDER BY url, userContextId, groupId"
    );
    return rows.map(r => ({
      url: r.getResultByName("url"),
      userContextId: r.getResultByName("userContextId"),
      tabGroup: r.getResultByName("groupId"),
      count: r.getResultByName("open_count"),
    }));
  }

  static getUserContextIdForOpenPagesTable(userContextId, isInPrivateWindow) {
    return isInPrivateWindow ? PRIVATE_USER_CONTEXT_ID : userContextId;
  }

  static isNonPrivateUserContextId(userContextId) {
    return userContextId != PRIVATE_USER_CONTEXT_ID;
  }

  static isContainerUserContextId(userContextId) {
    return userContextId > 0;
  }

  static promiseDBPopulated = AppConstants.MOZ_PLACES
    ? lazy.PlacesUtils.largeCacheDBConnDeferred.promise.then(async () => {
        UrlbarProviderOpenTabs.memoryTableInitialized = true;
        for (let [userContextId, groupEntries] of gOpenTabUrls) {
          for (let [groupId, entries] of groupEntries) {
            for (let [url, count] of entries) {
              await addToMemoryTable(url, userContextId, groupId, count).catch(
                console.error
              );
            }
          }
        }
      })
    : Promise.resolve();

  static async registerOpenTab(url, userContextId, groupId, isInPrivateWindow) {
    userContextId = parseInt(`${userContextId}`);
    groupId = groupId ?? null;
    if (!Number.isInteger(userContextId)) {
      lazy.logger.error("Invalid userContextId while registering openTab: ", {
        url,
        userContextId,
        isInPrivateWindow,
      });
      return;
    }
    lazy.logger.info("Registering openTab: ", {
      url,
      userContextId,
      groupId,
      isInPrivateWindow,
    });
    userContextId = UrlbarProviderOpenTabs.getUserContextIdForOpenPagesTable(
      userContextId,
      isInPrivateWindow
    );

    let contextEntries = gOpenTabUrls.get(userContextId);
    if (!contextEntries) {
      contextEntries = new Map();
      gOpenTabUrls.set(userContextId, contextEntries);
    }

    let groupEntries = contextEntries.get(groupId);
    if (!groupEntries) {
      groupEntries = new Map();
      contextEntries.set(groupId, groupEntries);
    }

    groupEntries.set(url, (groupEntries.get(url) ?? 0) + 1);
    await addToMemoryTable(url, userContextId, groupId).catch(console.error);
  }

  static async unregisterOpenTab(
    url,
    userContextId,
    groupId,
    isInPrivateWindow
  ) {
    userContextId = parseInt(`${userContextId}`);
    groupId = groupId ?? null;
    lazy.logger.info("Unregistering openTab: ", {
      url,
      userContextId,
      groupId,
      isInPrivateWindow,
    });
    userContextId = UrlbarProviderOpenTabs.getUserContextIdForOpenPagesTable(
      userContextId,
      isInPrivateWindow
    );

    let contextEntries = gOpenTabUrls.get(userContextId);
    if (contextEntries) {
      let groupEntries = contextEntries.get(groupId);
      if (groupEntries) {
        let oldCount = groupEntries.get(url);
        if (oldCount == 0) {
          console.error("Tried to unregister a non registered open tab");
          return;
        }
        if (oldCount == 1) {
          groupEntries.delete(url);
        } else {
          groupEntries.set(url, oldCount - 1);
        }
        await removeFromMemoryTable(url, userContextId, groupId).catch(
          console.error
        );
      }
    }
  }

  async startQuery(queryContext, addCallback) {
    let instance = this.queryInstance;
    let conn = await lazy.PlacesUtils.promiseLargeCacheDBConnection();
    await UrlbarProviderOpenTabs.promiseDBPopulated;
    await conn.executeCached(
      `
      SELECT url, userContextId, NULLIF(groupId, '') groupId
      FROM moz_openpages_temp
    `,
      {},
      (row, cancel) => {
        if (instance != this.queryInstance) {
          cancel();
          return;
        }
        addCallback(
          this,
          new lazy.UrlbarResult({
            type: lazy.UrlbarShared.RESULT_TYPE.TAB_SWITCH,
            source: lazy.UrlbarShared.RESULT_SOURCE.TABS,
            payload: {
              url: row.getResultByName("url"),
              userContextId: row.getResultByName("userContextId"),
              tabGroup: row.getResultByName("groupId"),
            },
          })
        );
      }
    );
  }
}

async function addToMemoryTable(url, userContextId, groupId, count = 1) {
  if (!UrlbarProviderOpenTabs.memoryTableInitialized) {
    return;
  }
  await lazy.ProvidersManager.runInCriticalSection(async () => {
    let conn = await lazy.PlacesUtils.promiseLargeCacheDBConnection();
    await conn.executeCached(
      `
      INSERT INTO moz_openpages_temp (url, userContextId, groupId, open_count)
      VALUES ( :url,
               :userContextId,
               IFNULL(:groupId, ''),
               :count
             )
      ON CONFLICT DO UPDATE SET open_count = open_count + 1
    `,
      { url, userContextId, groupId, count }
    );
  });
}

async function removeFromMemoryTable(url, userContextId, groupId) {
  if (!UrlbarProviderOpenTabs.memoryTableInitialized) {
    return;
  }
  await lazy.ProvidersManager.runInCriticalSection(async () => {
    let conn = await lazy.PlacesUtils.promiseLargeCacheDBConnection();
    await conn.executeCached(
      `
      UPDATE moz_openpages_temp
      SET open_count = open_count - 1
      WHERE url = :url
        AND userContextId = :userContextId
        AND groupId = IFNULL(:groupId, '')
    `,
      { url, userContextId, groupId }
    );
  });
}
