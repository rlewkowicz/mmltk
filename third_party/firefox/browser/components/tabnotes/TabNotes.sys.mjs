/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


import { Sqlite } from "resource://gre/modules/Sqlite.sys.mjs";

const GET_NOTE_BY_URL = `
SELECT
  id,
  canonical_url,
  created,
  note_text
FROM tabnotes
WHERE
  canonical_url = :url
`;

const CREATE_NOTE = `
INSERT INTO tabnotes
  (canonical_url, created, note_text)
VALUES
  (:url, unixepoch("now"), :note)
RETURNING
  id, canonical_url, created, note_text

`;

const UPDATE_NOTE = `
UPDATE
  tabnotes
SET
  note_text = :note
WHERE
  canonical_url = :url
RETURNING
  id, canonical_url, created, note_text
`;

const DELETE_NOTE = `
DELETE FROM
  tabnotes
WHERE
  canonical_url = :url
RETURNING
  id, canonical_url, created, note_text
`;

const GET_NOTE_COUNT = `
SELECT COUNT(*) FROM tabnotes
`;

export class TabNotesStorage {
  DATABASE_FILE_NAME = Object.freeze("tabnotes.sqlite");
  TELEMETRY_SOURCE = Object.freeze({
    TAB_CONTEXT_MENU: "context_menu",
    TAB_HOVER_PREVIEW_PANEL: "hover_menu",
    TAB_NOTE_PREVIEW_PANEL: "note_preview",
  });

  #initPromise;

  #databaseConnection;

  #shutdownBlocker;

  get #connection() {
    return this.init().then(() => this.#databaseConnection);
  }

  init(options) {
    if (this.#initPromise) {
      return this.#initPromise;
    }

    const basePath = options?.basePath ?? PathUtils.profileDir;
    this.dbPath = PathUtils.join(basePath, this.DATABASE_FILE_NAME);
    this.#initPromise = Sqlite.openConnection({
      path: this.dbPath,
    }).then(async connection => {
      this.#databaseConnection = connection;
      this.#shutdownBlocker = () => this.deinit();
      Sqlite.shutdown.addBlocker(
        "Closing tabnotes database",
        this.#shutdownBlocker
      );

      await connection.execute("PRAGMA journal_mode = WAL");
      await connection.execute("PRAGMA wal_autocheckpoint = 16");

      let currentVersion = await connection.getSchemaVersion();

      if (currentVersion == 1) {
        return;
      }

      if (currentVersion == 0) {
        await connection.executeTransaction(async () => {
          await connection.execute(`
          CREATE TABLE IF NOT EXISTS "tabnotes" (
            id            INTEGER PRIMARY KEY,
            canonical_url TEXT NOT NULL,
            created       INTEGER NOT NULL,
            note_text     TEXT NOT NULL
          );`);
          await connection.setSchemaVersion(1);
        });
      }
    });

    return this.#initPromise;
  }

  deinit() {
    if (this.#shutdownBlocker) {
      Sqlite.shutdown.removeBlocker(this.#shutdownBlocker);
      this.#shutdownBlocker = null;
    }
    if (this.#databaseConnection) {
      return this.#databaseConnection.close().then(() => {
        this.#databaseConnection = null;
        this.#initPromise = undefined;
      });
    }
    return Promise.resolve();
  }

  isEligible(tab) {
    if (tab?.canonicalUrl && URL.canParse(tab.canonicalUrl)) {
      return true;
    }
    return false;
  }

  async get(tab) {
    if (!this.isEligible(tab)) {
      return undefined;
    }
    const connection = await this.#connection;
    const results = await connection.executeCached(GET_NOTE_BY_URL, {
      url: tab.canonicalUrl,
    });
    if (!results?.length) {
      return undefined;
    }
    const [result] = results;
    const record = this.#mapDbRowToRecord(result);
    return record;
  }

  async set(tab, note, options = {}) {
    if (!this.isEligible(tab)) {
      throw new RangeError("Tab notes must be associated to an eligible tab");
    }
    if (!note) {
      throw new RangeError("Tab note text must be provided");
    }

    let existingNote = await this.get(tab);
    let sanitized = this.#sanitizeInput(note);

    if (existingNote && existingNote.text == sanitized) {
      return existingNote;
    }

    const connection = await this.#connection;
    return connection.executeTransaction(async () => {
      if (!existingNote) {
        const insertResult = await connection.executeCached(CREATE_NOTE, {
          url: tab.canonicalUrl,
          note: sanitized,
        });

        const insertedRecord = this.#mapDbRowToRecord(insertResult[0]);
        tab.dispatchEvent(
          new CustomEvent("TabNote:Created", {
            bubbles: true,
            detail: {
              note: insertedRecord,
              telemetrySource: options.telemetrySource,
            },
          })
        );
        return insertedRecord;
      }

      const updateResult = await connection.executeCached(UPDATE_NOTE, {
        url: tab.canonicalUrl,
        note: sanitized,
      });

      const updatedRecord = this.#mapDbRowToRecord(updateResult[0]);
      tab.dispatchEvent(
        new CustomEvent("TabNote:Edited", {
          bubbles: true,
          detail: {
            note: updatedRecord,
            telemetrySource: options.telemetrySource,
          },
        })
      );
      return updatedRecord;
    });
  }

  async delete(tab, options = {}) {
    const connection = await this.#connection;
    const deleteResult = await connection.executeCached(DELETE_NOTE, {
      url: tab.canonicalUrl,
    });

    if (deleteResult?.length > 0) {
      const deletedRecord = this.#mapDbRowToRecord(deleteResult[0]);
      tab.dispatchEvent(
        new CustomEvent("TabNote:Removed", {
          bubbles: true,
          detail: {
            note: deletedRecord,
            telemetrySource: options.telemetrySource,
          },
        })
      );
      return true;
    }

    return false;
  }

  async has(tab) {
    const record = await this.get(tab);
    return record !== undefined;
  }

  async count() {
    try {
      const connection = await this.#connection;
      const countResult = await connection.executeCached(GET_NOTE_COUNT);
      if (countResult?.length == 1) {
        return countResult[0].getDouble(0);
      }
    } catch {}
    return 0;
  }

  async reset() {
    const connection = await this.#connection;
    return connection.execute(`
      DELETE FROM "tabnotes"`);
  }

  #sanitizeInput(value) {
    return value.slice(0, 1000);
  }

  #mapDbRowToRecord(row) {
    return {
      id: row.getDouble(0),
      canonicalUrl: row.getString(1),
      created: Temporal.Instant.fromEpochMilliseconds(row.getDouble(2) * 1000),
      text: row.getString(3),
    };
  }
}

export const TabNotes = new TabNotesStorage();
