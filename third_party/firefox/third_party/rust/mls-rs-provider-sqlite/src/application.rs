// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use std::{
    fmt::{self, Debug},
    sync::{Arc, Mutex},
};

use rusqlite::{params, Connection, OptionalExtension};

use crate::SqLiteDataStorageError;

const INSERT_SQL: &str =
    "INSERT INTO kvs (key, value) VALUES (?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value WHERE value != excluded.value";

#[derive(Debug, Clone)]
/// SQLite key-value storage for application specific data.
pub struct SqLiteApplicationStorage {
    connection: Arc<Mutex<Connection>>,
}

impl SqLiteApplicationStorage {
    pub(crate) fn new(connection: Connection) -> SqLiteApplicationStorage {
        SqLiteApplicationStorage {
            connection: Arc::new(Mutex::new(connection)),
        }
    }

    /// Insert `value` into storage indexed by `key`.
    ///
    /// If a value already exists for `key` it will be overwritten.
    /// Returns the number of rows modified (0 if the key-value pair already exists).
    pub fn insert(&self, key: &str, value: &[u8]) -> Result<usize, SqLiteDataStorageError> {
        let connection = self.connection.lock().unwrap();

        connection
            .execute(INSERT_SQL, params![key, value])
            .map_err(sql_engine_error)
    }

    /// Execute multiple [`SqLiteApplicationStorage::insert`] operations in a transaction.
    /// Returns the total number of rows modified.
    pub fn transact_insert(&self, items: &[Item]) -> Result<usize, SqLiteDataStorageError> {
        let mut connection = self.connection.lock().unwrap();

        let tx = connection.transaction().map_err(sql_engine_error)?;

        let total_modified = items.iter().try_fold(0, |acc, item| {
            tx.execute(INSERT_SQL, params![item.key, item.value])
                .map_err(sql_engine_error)
                .map(|rows| acc + rows)
        })?;

        tx.commit().map_err(sql_engine_error)?;

        Ok(total_modified)
    }

    /// Get a value from storage based on its `key`.
    pub fn get(&self, key: &str) -> Result<Option<Vec<u8>>, SqLiteDataStorageError> {
        let connection = self.connection.lock().unwrap();

        connection
            .query_row("SELECT value FROM kvs WHERE key = ?", params![key], |row| {
                row.get(0)
            })
            .optional()
            .map_err(sql_engine_error)
    }

    /// Delete a value from storage based on its `key`.
    /// Returns the number of rows modified (0 if the key-value pair didnt exist).
    pub fn delete(&self, key: &str) -> Result<usize, SqLiteDataStorageError> {
        let connection = self.connection.lock().unwrap();

        connection
            .execute("DELETE FROM kvs WHERE key = ?", params![key])
            .map_err(sql_engine_error)
    }

    /// Get all keys and values from storage for which key starts with `key_prefix`.
    pub fn get_by_prefix(&self, key_prefix: &str) -> Result<Vec<Item>, SqLiteDataStorageError> {
        let connection = self.connection.lock().unwrap();
        let mut key_prefix = sanitize(key_prefix);
        key_prefix.push('%');

        let mut stmt = connection
            .prepare("SELECT key, value FROM kvs WHERE key LIKE ? ESCAPE '$'")
            .map_err(sql_engine_error)?;

        let rows = stmt
            .query(params![key_prefix])
            .map_err(sql_engine_error)?
            .mapped(|row| Ok(Item::new(row.get(0)?, row.get(1)?)));

        rows.collect::<Result<_, _>>().map_err(sql_engine_error)
    }

    /// Delete all values from storage for which key starts with `key_prefix`.
    /// Returns the total number of rows modified.
    pub fn delete_by_prefix(&self, key_prefix: &str) -> Result<usize, SqLiteDataStorageError> {
        let connection = self.connection.lock().unwrap();
        let mut key_prefix = sanitize(key_prefix);
        key_prefix.push('%');

        connection
            .execute(
                "DELETE FROM kvs WHERE key LIKE ? ESCAPE '$'",
                params![key_prefix],
            )
            .map_err(sql_engine_error)
    }
}

fn sanitize(string: &str) -> String {
    string.replace('_', "$_").replace('%', "$%")
}

fn sql_engine_error(e: rusqlite::Error) -> SqLiteDataStorageError {
    SqLiteDataStorageError::SqlEngineError(e.into())
}

#[derive(Clone, Default, Hash, PartialEq, Eq, PartialOrd, Ord)]
pub struct Item {
    pub key: String,
    pub value: Vec<u8>,
}

impl Debug for Item {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Item")
            .field("key", &self.key)
            .field("value", &mls_rs_core::debug::pretty_bytes(&self.value))
            .finish()
    }
}

impl Item {
    pub fn new(key: String, value: Vec<u8>) -> Self {
        Self { key, value }
    }

    pub fn key(&self) -> &str {
        &self.key
    }

    pub fn value(&self) -> &[u8] {
        &self.value
    }
}
