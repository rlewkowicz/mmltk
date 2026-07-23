/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const MS_PER_DAY = 86400000;
const CORRUPT_DB_RETAIN_DAYS = 14;

const MAINTENANCE_INTERVAL_SECONDS = 7 * 86400;

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PlacesPreviews: "resource://gre/modules/PlacesPreviews.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  Sqlite: "resource://gre/modules/Sqlite.sys.mjs",
});

export var PlacesDBUtils = {
  _isShuttingDown: false,

  _clearTaskQueue: false,
  clearPendingTasks() {
    PlacesDBUtils._clearTaskQueue = true;
  },

  async maintenanceOnIdle() {
    let tasks = [
      this.checkIntegrity,
      this.checkCoherence,
      this._refreshUI,
      this.incrementalVacuum,
      this.optimize,
      this.removeOldCorruptDBs,
      this.deleteOrphanPreviews,
    ];
    let taskStatusMap = await PlacesDBUtils.runTasks(tasks);

    Services.prefs.setIntPref(
      "places.database.lastMaintenance",
      Math.floor(Date.now() / 1000)
    );
    return taskStatusMap;
  },

  async checkAndFixDatabase() {
    let tasks = [
      this.checkIntegrity,
      this.checkCoherence,
      this.expire,
      this.vacuum,
      this.stats,
      this._refreshUI,
    ];
    return PlacesDBUtils.runTasks(tasks);
  },

  async _refreshUI() {
    PlacesObservers.notifyListeners([new PlacesPurgeCaches()]);
    return [];
  },

  async checkIntegrity() {
    let logs = [];

    async function check(dbName) {
      try {
        await integrity(dbName);
        logs.push(`The ${dbName} database is sane`);
      } catch (ex) {
        PlacesDBUtils.clearPendingTasks();
        if (ex.result == Cr.NS_ERROR_FILE_CORRUPTED) {
          logs.push(`The ${dbName} database is corrupt`);
          Services.prefs.setCharPref(
            "places.database.replaceDatabaseOnStartup",
            dbName
          );
          throw new Error(
            `Unable to fix corruption, ${dbName} will be replaced on next startup`
          );
        }
        throw new Error(`Unable to check ${dbName} integrity: ${ex}`);
      }
    }

    await check("places.sqlite");
    await check("favicons.sqlite");

    return logs;
  },

  async checkCoherence() {
    let logs = [];
    let stmts = await PlacesDBUtils._getCoherenceStatements();
    let coherenceCheck = true;
    await lazy.PlacesUtils.withConnectionWrapper(
      "PlacesDBUtils: coherence check:",
      db =>
        db.executeTransaction(async () => {
          for (let { query, params } of stmts) {
            try {
              await db.execute(query, params || null);
            } catch (ex) {
              console.error(ex);
              coherenceCheck = false;
            }
          }
        })
    );

    if (coherenceCheck) {
      logs.push("The database is coherent");
    } else {
      PlacesDBUtils.clearPendingTasks();
      throw new Error("Unable to complete the coherence check");
    }
    return logs;
  },

  async incrementalVacuum() {
    let logs = [];
    return lazy.PlacesUtils.withConnectionWrapper(
      "PlacesDBUtils: incrementalVacuum",
      async db => {
        let count = (
          await db.execute("PRAGMA favicons.freelist_count")
        )[0].getResultByIndex(0);
        if (count < 10) {
          logs.push(
            `The favicons database has only ${count} free pages, not vacuuming.`
          );
        } else {
          logs.push(
            `The favicons database has ${count} free pages, vacuuming.`
          );
          await db.execute("PRAGMA favicons.incremental_vacuum");
          count = (
            await db.execute("PRAGMA favicons.freelist_count")
          )[0].getResultByIndex(0);
          logs.push(
            `The database has been vacuumed and has now ${count} free pages.`
          );
        }
        return logs;
      }
    ).catch(ex => {
      PlacesDBUtils.clearPendingTasks();
      throw new Error(
        "Unable to incrementally vacuum the favicons database " + ex
      );
    });
  },

  async deleteOrphanPreviews() {
    let logs = [];
    try {
      let deleted = await lazy.PlacesPreviews.deleteOrphans();
      if (deleted) {
        logs.push(`Orphan previews deleted.`);
      }
    } catch (ex) {
      throw new Error("Unable to delete orphan previews " + ex);
    }
    return logs;
  },

  async optimize() {
    let logs = [];
    return lazy.PlacesUtils.withConnectionWrapper(
      "PlacesDBUtils: optimize",
      async db => {
        await db.execute("PRAGMA optimize(0x10012)");
        logs.push("The database has been optimized.");
        return logs;
      }
    ).catch(ex => {
      PlacesDBUtils.clearPendingTasks();
      throw new Error("Unable to optimize the database " + ex);
    });
  },

  async _getCoherenceStatements() {
    let cleanupStatements = [
      {
        query: `CREATE TEMP TABLE IF NOT EXISTS moz_places_dupes_temp(
          id INTEGER PRIMARY KEY
        , hash INTEGER NOT NULL
        , url TEXT UNIQUE NOT NULL
        , count INTEGER NOT NULL DEFAULT 0
        )`,
      },
      {
        query: `CREATE TEMP TRIGGER IF NOT EXISTS moz_places_remove_dupes_temp_trigger
        AFTER DELETE ON moz_places_dupes_temp
        FOR EACH ROW
        BEGIN
          /* Reassign history visits. */
          UPDATE moz_historyvisits SET
            place_id = OLD.id
          WHERE place_id IN (SELECT id FROM moz_places
                             WHERE id <> OLD.id AND
                                   url_hash = OLD.hash AND
                                   url = OLD.url);

          /* Merge autocomplete history entries. */
          INSERT INTO moz_inputhistory(place_id, input, use_count)
          SELECT OLD.id, a.input, a.use_count
          FROM moz_inputhistory a
          JOIN moz_places h ON h.id = a.place_id
          WHERE h.id <> OLD.id AND
                h.url_hash = OLD.hash AND
                h.url = OLD.url
          ON CONFLICT(place_id, input) DO UPDATE SET
            place_id = excluded.place_id,
            use_count = use_count + excluded.use_count;

          /* Merge page annos, ignoring annos with the same name that are
             already set on the destination. */
          INSERT OR IGNORE INTO moz_annos(id, place_id, anno_attribute_id,
                                          content, flags, expiration, type,
                                          dateAdded, lastModified)
          SELECT (SELECT k.id FROM moz_annos k
                  WHERE k.place_id = OLD.id AND
                        k.anno_attribute_id = a.anno_attribute_id), OLD.id,
                 a.anno_attribute_id, a.content, a.flags, a.expiration, a.type,
                 a.dateAdded, a.lastModified
          FROM moz_annos a
          JOIN moz_places h ON h.id = a.place_id
          WHERE h.id <> OLD.id AND
                url_hash = OLD.hash AND
                url = OLD.url;

          /* Reassign bookmarks. */
          UPDATE moz_bookmarks SET
            fk = OLD.id
          WHERE fk IN (SELECT id FROM moz_places
                       WHERE url_hash = OLD.hash AND
                             url = OLD.url);

          /* Reassign keywords. */
          UPDATE moz_keywords SET
            place_id = OLD.id
          WHERE place_id IN (SELECT id FROM moz_places
                             WHERE id <> OLD.id AND
                                   url_hash = OLD.hash AND
                                   url = OLD.url);

          /* Now that we've updated foreign key references, drop the
             conflicting source. */
          DELETE FROM moz_places
          WHERE id <> OLD.id AND
                url_hash = OLD.hash AND
                url = OLD.url;

          /* Recalculate frecency for the destination. */
          UPDATE moz_places SET recalc_frecency = 1, recalc_alt_frecency = 1
          WHERE id = OLD.id;
        END`,
      },
      {
        query: `INSERT INTO moz_places_dupes_temp(id, hash, url, count)
        SELECT h.id, h.url_hash, h.url, 1
        FROM moz_places h
        JOIN (SELECT url_hash FROM moz_places
              GROUP BY url_hash
              HAVING count(*) > 1) d ON d.url_hash = h.url_hash
        ON CONFLICT(url) DO UPDATE SET
          count = count + 1`,
      },
      { query: `DELETE FROM moz_places_dupes_temp WHERE count > 1` },
      { query: `DROP TABLE moz_places_dupes_temp` },

      {
        query: `DELETE FROM moz_annos
        WHERE type = 4 OR anno_attribute_id IN (
          SELECT id FROM moz_anno_attributes
          WHERE name = 'downloads/destinationFileName' OR
                name BETWEEN 'weave/' AND 'weave0'
        )`,
      },

      {
        query: `DELETE FROM moz_anno_attributes WHERE id IN (
          SELECT id FROM moz_anno_attributes n
          WHERE NOT EXISTS
              (SELECT id FROM moz_annos WHERE anno_attribute_id = n.id LIMIT 1)
        )`,
      },

      {
        query: `DELETE FROM moz_annos WHERE id IN (
          SELECT id FROM moz_annos a
          WHERE NOT EXISTS
            (SELECT id FROM moz_anno_attributes
              WHERE id = a.anno_attribute_id LIMIT 1)
        )`,
      },

      {
        query: `DELETE FROM moz_annos WHERE id IN (
          SELECT id FROM moz_annos a
          WHERE NOT EXISTS
            (SELECT id FROM moz_places WHERE id = a.place_id LIMIT 1)
        )`,
      },

      {
        query: `DELETE FROM moz_bookmarks WHERE guid NOT IN (
          :rootGuid, :menuGuid, :toolbarGuid, :unfiledGuid, :tagsGuid  /* skip roots */
        ) AND id IN (
          SELECT b.id FROM moz_bookmarks b
          WHERE b.parent IN
            (SELECT id FROM moz_bookmarks WHERE parent = :tags_folder)
            AND b.type <> :bookmark_type
        )`,
        params: {
          tags_folder: lazy.PlacesUtils.tagsFolderId,
          bookmark_type: lazy.PlacesUtils.bookmarks.TYPE_BOOKMARK,
          rootGuid: lazy.PlacesUtils.bookmarks.rootGuid,
          menuGuid: lazy.PlacesUtils.bookmarks.menuGuid,
          toolbarGuid: lazy.PlacesUtils.bookmarks.toolbarGuid,
          unfiledGuid: lazy.PlacesUtils.bookmarks.unfiledGuid,
          tagsGuid: lazy.PlacesUtils.bookmarks.tagsGuid,
        },
      },

      {
        query: `DELETE FROM moz_bookmarks WHERE guid NOT IN (
          :rootGuid, :menuGuid, :toolbarGuid, :unfiledGuid, :tagsGuid  /* skip roots */
        ) AND id IN (
          SELECT b.id FROM moz_bookmarks b
          WHERE b.id IN
            (SELECT id FROM moz_bookmarks WHERE parent = :tags_folder)
            AND NOT EXISTS
              (SELECT id from moz_bookmarks WHERE parent = b.id LIMIT 1)
        )`,
        params: {
          tags_folder: lazy.PlacesUtils.tagsFolderId,
          rootGuid: lazy.PlacesUtils.bookmarks.rootGuid,
          menuGuid: lazy.PlacesUtils.bookmarks.menuGuid,
          toolbarGuid: lazy.PlacesUtils.bookmarks.toolbarGuid,
          unfiledGuid: lazy.PlacesUtils.bookmarks.unfiledGuid,
          tagsGuid: lazy.PlacesUtils.bookmarks.tagsGuid,
        },
      },

      {
        query: `UPDATE moz_bookmarks SET
          parent = (SELECT id FROM moz_bookmarks WHERE guid = :unfiledGuid)
        WHERE guid NOT IN (
          :rootGuid, :menuGuid, :toolbarGuid, :unfiledGuid, :tagsGuid  /* skip roots */
        ) AND id IN (
          SELECT b.id FROM moz_bookmarks b
          WHERE NOT EXISTS
            (SELECT id FROM moz_bookmarks WHERE id = b.parent LIMIT 1)
        )`,
        params: {
          rootGuid: lazy.PlacesUtils.bookmarks.rootGuid,
          menuGuid: lazy.PlacesUtils.bookmarks.menuGuid,
          toolbarGuid: lazy.PlacesUtils.bookmarks.toolbarGuid,
          unfiledGuid: lazy.PlacesUtils.bookmarks.unfiledGuid,
          tagsGuid: lazy.PlacesUtils.bookmarks.tagsGuid,
        },
      },

      {
        query: `UPDATE moz_bookmarks
        SET guid = GENERATE_GUID(),
            type = :bookmark_type
        WHERE guid NOT IN (
          :rootGuid, :menuGuid, :toolbarGuid, :unfiledGuid, :tagsGuid  /* skip roots */
        ) AND id IN (
          SELECT id FROM moz_bookmarks b
          WHERE type IN (:folder_type, :separator_type)
            AND fk NOTNULL
        )`,
        params: {
          bookmark_type: lazy.PlacesUtils.bookmarks.TYPE_BOOKMARK,
          folder_type: lazy.PlacesUtils.bookmarks.TYPE_FOLDER,
          separator_type: lazy.PlacesUtils.bookmarks.TYPE_SEPARATOR,
          rootGuid: lazy.PlacesUtils.bookmarks.rootGuid,
          menuGuid: lazy.PlacesUtils.bookmarks.menuGuid,
          toolbarGuid: lazy.PlacesUtils.bookmarks.toolbarGuid,
          unfiledGuid: lazy.PlacesUtils.bookmarks.unfiledGuid,
          tagsGuid: lazy.PlacesUtils.bookmarks.tagsGuid,
        },
      },

      {
        query: `UPDATE moz_bookmarks
        SET guid = GENERATE_GUID(),
            type = :folder_type
        WHERE guid NOT IN (
          :rootGuid, :menuGuid, :toolbarGuid, :unfiledGuid, :tagsGuid  /* skip roots */
        ) AND id IN (
          SELECT id FROM moz_bookmarks b
          WHERE type = :bookmark_type
            AND fk IS NULL
        )`,
        params: {
          bookmark_type: lazy.PlacesUtils.bookmarks.TYPE_BOOKMARK,
          folder_type: lazy.PlacesUtils.bookmarks.TYPE_FOLDER,
          rootGuid: lazy.PlacesUtils.bookmarks.rootGuid,
          menuGuid: lazy.PlacesUtils.bookmarks.menuGuid,
          toolbarGuid: lazy.PlacesUtils.bookmarks.toolbarGuid,
          unfiledGuid: lazy.PlacesUtils.bookmarks.unfiledGuid,
          tagsGuid: lazy.PlacesUtils.bookmarks.tagsGuid,
        },
      },

      {
        query: `UPDATE moz_bookmarks
        SET guid = GENERATE_GUID(),
            type = CASE WHEN fk NOT NULL THEN :bookmark_type ELSE :folder_type END
        WHERE guid NOT IN (
         :rootGuid, :menuGuid, :toolbarGuid, :unfiledGuid, :tagsGuid  /* skip roots */
        ) AND type IS NULL`,
        params: {
          bookmark_type: lazy.PlacesUtils.bookmarks.TYPE_BOOKMARK,
          folder_type: lazy.PlacesUtils.bookmarks.TYPE_FOLDER,
          rootGuid: lazy.PlacesUtils.bookmarks.rootGuid,
          menuGuid: lazy.PlacesUtils.bookmarks.menuGuid,
          toolbarGuid: lazy.PlacesUtils.bookmarks.toolbarGuid,
          unfiledGuid: lazy.PlacesUtils.bookmarks.unfiledGuid,
          tagsGuid: lazy.PlacesUtils.bookmarks.tagsGuid,
        },
      },

      {
        query: `UPDATE moz_bookmarks SET
          parent = (SELECT id FROM moz_bookmarks WHERE guid = :unfiledGuid)
        WHERE guid NOT IN (
          :rootGuid, :menuGuid, :toolbarGuid, :unfiledGuid, :tagsGuid  /* skip roots */
        ) AND id IN (
          SELECT id FROM moz_bookmarks b
          WHERE EXISTS
            (SELECT id FROM moz_bookmarks WHERE id = b.parent
              AND type IN (:bookmark_type, :separator_type)
              LIMIT 1)
        )`,
        params: {
          bookmark_type: lazy.PlacesUtils.bookmarks.TYPE_BOOKMARK,
          separator_type: lazy.PlacesUtils.bookmarks.TYPE_SEPARATOR,
          rootGuid: lazy.PlacesUtils.bookmarks.rootGuid,
          menuGuid: lazy.PlacesUtils.bookmarks.menuGuid,
          toolbarGuid: lazy.PlacesUtils.bookmarks.toolbarGuid,
          unfiledGuid: lazy.PlacesUtils.bookmarks.unfiledGuid,
          tagsGuid: lazy.PlacesUtils.bookmarks.tagsGuid,
        },
      },

      {
        query: `DELETE FROM moz_bookmarks AS b
        WHERE b.guid NOT IN (
          :rootGuid, :menuGuid, :toolbarGuid, :unfiledGuid, :tagsGuid  /* skip roots */
        ) AND b.fk NOT NULL
          AND b.type = :bookmark_type
          AND NOT EXISTS (SELECT 1 FROM moz_places h WHERE h.id = b.fk)`,
        params: {
          bookmark_type: lazy.PlacesUtils.bookmarks.TYPE_BOOKMARK,
          rootGuid: lazy.PlacesUtils.bookmarks.rootGuid,
          menuGuid: lazy.PlacesUtils.bookmarks.menuGuid,
          toolbarGuid: lazy.PlacesUtils.bookmarks.toolbarGuid,
          unfiledGuid: lazy.PlacesUtils.bookmarks.unfiledGuid,
          tagsGuid: lazy.PlacesUtils.bookmarks.tagsGuid,
        },
      },

      {
        query: `
          WITH positions(item_id, pos, seq) AS (
            SELECT id, position AS pos,
                   (row_number() OVER (PARTITION BY parent ORDER BY position)) - 1 AS seq
            FROM moz_bookmarks
          )
          UPDATE moz_bookmarks
          SET position = seq
          FROM positions
          WHERE item_id = moz_bookmarks.id AND seq <> pos`,
      },

      {
        query: `UPDATE moz_bookmarks SET title = :empty_title
        WHERE length(title) = 0 AND type = :folder_type
          AND parent = :tags_folder`,
        params: {
          empty_title: "(notitle)",
          folder_type: lazy.PlacesUtils.bookmarks.TYPE_FOLDER,
          tags_folder: lazy.PlacesUtils.tagsFolderId,
        },
      },

      {
        query: `DELETE FROM moz_pages_w_icons WHERE page_url_hash NOT IN (
          SELECT url_hash FROM moz_places
        )`,
      },

      {
        query: `DELETE FROM moz_icons WHERE id IN (
          SELECT id FROM moz_icons WHERE root = 0
          UNION ALL
          SELECT id FROM moz_icons
          WHERE root = 1
            AND get_host_and_port(icon_url) NOT IN (SELECT host FROM moz_origins)
            AND fixup_url(get_host_and_port(icon_url)) NOT IN (SELECT host FROM moz_origins)
          EXCEPT
          SELECT icon_id FROM moz_icons_to_pages
        )`,
      },

      {
        query: `DELETE FROM moz_historyvisits WHERE id IN (
          SELECT id FROM moz_historyvisits v
          WHERE NOT EXISTS
            (SELECT id FROM moz_places WHERE id = v.place_id LIMIT 1)
        )`,
      },

      {
        query: `DELETE FROM moz_inputhistory WHERE place_id IN (
          SELECT place_id FROM moz_inputhistory i
          WHERE NOT EXISTS
            (SELECT id FROM moz_places WHERE id = i.place_id LIMIT 1)
        )`,
      },

      {
        query: `DELETE FROM moz_keywords WHERE id IN (
          SELECT id FROM moz_keywords k
          WHERE NOT EXISTS
            (SELECT 1 FROM moz_places h WHERE k.place_id = h.id)
        )`,
      },

      {
        query: `UPDATE moz_places
        SET visit_count = (SELECT count(*) FROM moz_historyvisits
                            WHERE place_id = moz_places.id AND visit_type NOT IN (0,4,7,8,9)),
            last_visit_date = (SELECT MAX(visit_date) FROM moz_historyvisits
                                WHERE place_id = moz_places.id)
        WHERE id IN (
          SELECT h.id FROM moz_places h
          WHERE visit_count <> (SELECT count(*) FROM moz_historyvisits v
                                WHERE v.place_id = h.id AND visit_type NOT IN (0,4,7,8,9))
              OR last_visit_date IS NOT
                (SELECT MAX(visit_date) FROM moz_historyvisits v WHERE v.place_id = h.id)
        )`,
      },

      {
        query: `UPDATE moz_places
        SET hidden = 1
        WHERE id IN (
          SELECT h.id FROM moz_places h
          JOIN moz_historyvisits src ON src.place_id = h.id
          JOIN moz_historyvisits dst ON dst.from_visit = src.id AND dst.visit_type IN (5,6)
          LEFT JOIN moz_bookmarks on fk = h.id AND fk ISNULL
          GROUP BY src.place_id HAVING count(*) = visit_count
        )`,
      },

      {
        query: `UPDATE moz_places SET foreign_count =
          (SELECT count(*) FROM moz_bookmarks WHERE fk = moz_places.id ) +
          (SELECT count(*) FROM moz_keywords WHERE place_id = moz_places.id )`,
      },

      {
        query: `UPDATE moz_places SET url_hash = hash(url) WHERE url_hash = 0`,
      },

      {
        query: `UPDATE moz_places
        SET guid = GENERATE_GUID()
        WHERE guid IS NULL OR
              NOT IS_VALID_GUID(guid)`,
      },

      {
        query: `UPDATE moz_bookmarks
        SET guid = GENERATE_GUID()
        WHERE guid IS NULL OR
              NOT IS_VALID_GUID(guid)`,
      },

      {
        query: `UPDATE moz_bookmarks
        SET dateAdded = COALESCE(NULLIF(dateAdded, 0), NULLIF(lastModified, 0), NULLIF((
              SELECT MIN(visit_date) FROM moz_historyvisits
              WHERE place_id = fk
            ), 0), STRFTIME('%s', 'now', 'localtime', 'utc') * 1000000),
            lastModified = COALESCE(NULLIF(lastModified, 0), NULLIF(dateAdded, 0), NULLIF((
              SELECT MAX(visit_date) FROM moz_historyvisits
              WHERE place_id = fk
            ), 0), STRFTIME('%s', 'now', 'localtime', 'utc') * 1000000)
        WHERE NULLIF(dateAdded, 0) IS NULL OR
              NULLIF(lastModified, 0) IS NULL`,
      },

      {
        query: `UPDATE moz_bookmarks
         SET dateAdded = lastModified
         WHERE dateAdded > lastModified`,
      },
    ];

    return cleanupStatements;
  },

  async vacuum() {
    let logs = [];
    let placesDbPath = PathUtils.join(PathUtils.profileDir, "places.sqlite");
    let info = await IOUtils.stat(placesDbPath);
    logs.push(`Initial database size is ${Math.floor(info.size / 1024)}KiB`);
    return lazy.PlacesUtils.withConnectionWrapper(
      "PlacesDBUtils: vacuum",
      async db => {
        await db.execute("VACUUM");
        logs.push("The database has been vacuumed");
        info = await IOUtils.stat(placesDbPath);
        logs.push(`Final database size is ${Math.floor(info.size / 1024)}KiB`);
        return logs;
      }
    ).catch(() => {
      PlacesDBUtils.clearPendingTasks();
      throw new Error("Unable to vacuum database");
    });
  },

  async expire() {
    let logs = [];

    let expiration = Cc["@mozilla.org/places/expiration;1"].getService(
      Ci.nsIObserver
    );

    let returnPromise = new Promise(res => {
      let observer = (subject, topic) => {
        Services.obs.removeObserver(observer, topic);
        logs.push("Database cleaned up");
        res(logs);
      };
      Services.obs.addObserver(
        observer,
        lazy.PlacesUtils.TOPIC_EXPIRATION_FINISHED
      );
    });

    if (typeof expiration !== "function") {
      expiration.observe(null, "places-debug-start-expiration", "0");
    }
    return returnPromise;
  },

  async stats() {
    let logs = [];
    let placesDbPath = PathUtils.join(PathUtils.profileDir, "places.sqlite");
    let info = await IOUtils.stat(placesDbPath);
    logs.push(`Places.sqlite size is ${Math.floor(info.size / 1024)}KiB`);
    let faviconsDbPath = PathUtils.join(
      PathUtils.profileDir,
      "favicons.sqlite"
    );
    info = await IOUtils.stat(faviconsDbPath);
    logs.push(`Favicons.sqlite size is ${Math.floor(info.size / 1024)}KiB`);

    let pragmas = [
      "user_version",
      "page_size",
      "cache_size",
      "journal_mode",
      "synchronous",
    ].map(p => `pragma_${p}`);
    let pragmaQuery = `SELECT * FROM ${pragmas.join(", ")}`;
    await lazy.PlacesUtils.withConnectionWrapper(
      "PlacesDBUtils: pragma for stats",
      async db => {
        let row = (await db.execute(pragmaQuery))[0];
        for (let i = 0; i != pragmas.length; i++) {
          logs.push(`${pragmas[i]} is ${row.getResultByIndex(i)}`);
        }
      }
    ).catch(() => {
      logs.push("Could not set pragma for stat collection");
    });

    try {

      let expiration =  (
        Cc["@mozilla.org/places/expiration;1"].getService(Ci.nsISupports)
          .wrappedJSObject
      );
      let limitURIs = await expiration.getPagesLimit();
      logs.push(
        "History can store a maximum of " + limitURIs + " unique pages"
      );
    } catch (ex) {}

    let query = "SELECT name FROM sqlite_master WHERE type = :type";
    let params = {};
    let _getTableCount = async tableName => {
      let db = await lazy.PlacesUtils.promiseDBConnection();
      let rows = await db.execute(`SELECT count(*) FROM ${tableName}`);
      logs.push(
        `Table ${tableName} has ${rows[0].getResultByIndex(0)} records`
      );
    };

    try {
      params.type = "table";
      let db = await lazy.PlacesUtils.promiseDBConnection();
      await db.execute(query, params, r =>
        _getTableCount(r.getResultByIndex(0))
      );
    } catch (ex) {
      throw new Error("Unable to collect stats.");
    }

    let details = await PlacesDBUtils.getEntitiesStats();
    logs.push(
      `Pages sequentiality: ${details.get("moz_places").sequentialityPerc}`
    );
    let entities = Array.from(details.keys()).sort((a, b) => {
      return details.get(a).sizePerc - details.get(b).sizePerc;
    });
    for (let key of entities) {
      let value = details.get(key);
      logs.push(
        `${key}: ${value.sizeBytes / 1024}KiB (${value.sizePerc}%), ${
          value.efficiencyPerc
        }% eff.`
      );
    }

    return logs;
  },

  async removeOldCorruptDBs() {
    let logs = [];
    logs.push(
      "> Cleanup profile from places.sqlite.corrupt files older than " +
        CORRUPT_DB_RETAIN_DAYS +
        " days."
    );
    let re = /places\.sqlite(-\d)?\.corrupt$/;
    let currentTime = Date.now();
    let children = await IOUtils.getChildren(PathUtils.profileDir);
    try {
      for (let entry of children) {
        let fileInfo = await IOUtils.stat(entry);
        let lastModificationDate;
        if (fileInfo.type == "regular" && re.test(entry)) {
          lastModificationDate = fileInfo.lastModified;
          try {
            let days = Math.ceil(
              (currentTime - lastModificationDate) / MS_PER_DAY
            );
            if (days >= CORRUPT_DB_RETAIN_DAYS || days < 0) {
              await IOUtils.remove(entry);
            }
          } catch (error) {
            logs.push("Could not remove file: " + entry, error);
          }
        }
      }
    } catch (error) {
      logs.push("removeOldCorruptDBs failed", error);
    }
    return logs;
  },

  async getEntitiesStats() {
    let db = await lazy.PlacesUtils.promiseDBConnection();
    let rows = await db.execute(`
      /* do not warn (bug no): no need for index */
      SELECT name,
      round((pgsize - unused) * 100.0 / pgsize, 1) as efficiency_perc,
      pgsize as size_bytes, pageno as pages,
      round(pgsize * 100.0 / (SELECT sum(pgsize) FROM dbstat WHERE aggregate = TRUE), 1) as size_perc,
      round((
        WITH s(row, pageno) AS (
          SELECT row_number() OVER (ORDER BY path), pageno FROM dbstat ORDER BY path
        )
        SELECT sum(s1.pageno+1==s2.pageno)*100.0/count(*)
        FROM s AS s1, s AS s2
        WHERE s1.row+1=s2.row
      ), 1) AS sequentiality_perc
      FROM dbstat
      WHERE aggregate = TRUE
    `);
    let entitiesByName = new Map();
    for (let row of rows) {
      let details = {
        efficiencyPerc: row.getResultByName("efficiency_perc"),
        pages: row.getResultByName("pages"),
        sizeBytes: row.getResultByName("size_bytes"),
        sizePerc: row.getResultByName("size_perc"),
        sequentialityPerc: row.getResultByName("sequentiality_perc"),
      };
      entitiesByName.set(row.getResultByName("name"), details);
    }
    return entitiesByName;
  },

  async getEntitiesStatsAndCounts() {
    let stats = await PlacesDBUtils.getEntitiesStats();
    let data = [];
    let db = await lazy.PlacesUtils.promiseDBConnection();
    for (let [entity, value] of stats) {
      let count = "-";
      try {
        if (
          entity.startsWith("moz_") &&
          !entity.endsWith("index") &&
          entity != "moz_places_visitcount" 
        ) {
          count = (
            await db.execute(`SELECT count(*) FROM ${entity}`)
          )[0].getResultByIndex(0);
        }
      } catch (ex) {
        console.error(
          "Error raised while gathering stats on the Places database: ",
          ex
        );
      }
      data.push(Object.assign(value, { entity, count }));
    }
    return data;
  },

  async runTasks(tasks) {
    if (!this._registeredShutdownObserver) {
      this._registeredShutdownObserver = true;
      lazy.PlacesUtils.registerShutdownFunction(() => {
        this._isShuttingDown = true;
      });
    }
    PlacesDBUtils._clearTaskQueue = false;
    let tasksMap = new Map();
    for (let task of tasks) {
      if (PlacesDBUtils._isShuttingDown) {
        tasksMap.set(task.name, {
          succeeded: false,
          logs: ["Shutting down, will not schedule the task."],
        });
        continue;
      }

      if (PlacesDBUtils._clearTaskQueue) {
        tasksMap.set(task.name, {
          succeeded: false,
          logs: ["The task queue was cleared by an error in another task."],
        });
        continue;
      }

      let result = await task()
        .then((logs = [`${task.name} complete`]) => ({ succeeded: true, logs }))
        .catch(err => ({ succeeded: false, logs: [err.message] }));
      tasksMap.set(task.name, result);
    }
    return tasksMap;
  },

  async removeDownloadsMetadataFromDb(placesDbPath) {
    if (!(await IOUtils.exists(placesDbPath))) {
      return;
    }

    let connection;
    try {
      connection = await lazy.Sqlite.openConnection({
        path: placesDbPath,
      });
      const removeDownloads = `
        -- Find download annotations
        WITH found_annos AS (
            SELECT a.id AS anno_id
            FROM moz_annos a
            JOIN moz_anno_attributes attr
              ON a.anno_attribute_id = attr.id
            WHERE INSTR(attr.name, 'downloads/') = 1
        )
        -- Delete downloads from moz_annos but leave the URLs in moz_places history
        DELETE FROM moz_annos
        WHERE id IN (SELECT anno_id FROM found_annos);
      `;
      await connection.execute(removeDownloads);
    } finally {
      await connection?.close();
    }
  },
};

async function integrity(dbName) {
  async function check(db) {
    let row;
    await db.execute("PRAGMA integrity_check", null, (r, cancel) => {
      row = r;
      cancel();
    });
    // @ts-ignore - nsIVariant has no overlap with other Javascript types
    return row?.getResultByIndex(0) === "ok";
  }

  let path = PathUtils.join(PathUtils.profileDir, dbName);
  let db = await lazy.Sqlite.openConnection({ path });
  try {
    if (await check(db)) {
      return;
    }

    try {
      await db.execute("REINDEX");
    } catch (ex) {
      throw Components.Exception(
        "Impossible to reindex database",
        Cr.NS_ERROR_FILE_CORRUPTED
      );
    }

    if (!(await check(db))) {
      throw Components.Exception(
        "The database is still corrupt",
        Cr.NS_ERROR_FILE_CORRUPTED
      );
    }
  } finally {
    await db.close();
  }
}

export function PlacesDBUtilsIdleMaintenance() {}

PlacesDBUtilsIdleMaintenance.prototype = {
  observe(subject, topic) {
    switch (topic) {
      case "idle-daily": {
        let lastMaintenance = Services.prefs.getIntPref(
          "places.database.lastMaintenance",
          0
        );
        let nowSeconds = Math.floor(Date.now() / 1000);
        if (lastMaintenance < nowSeconds - MAINTENANCE_INTERVAL_SECONDS) {
          PlacesDBUtils.maintenanceOnIdle();
        }
        break;
      }
      default:
        throw new Error("Trying to handle an unknown category.");
    }
  },
  classID: Components.ID("d38926e0-29c1-11eb-8588-0800200c9a66"),
  QueryInterface: ChromeUtils.generateQI(["nsIObserver"]),
};
