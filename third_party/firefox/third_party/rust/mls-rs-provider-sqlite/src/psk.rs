// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::SqLiteDataStorageError;
use mls_rs_core::psk::{ExternalPskId, PreSharedKey, PreSharedKeyStorage};
use rusqlite::{params, Connection, OptionalExtension};
use std::{
    ops::Deref,
    sync::{Arc, Mutex},
};

#[derive(Debug, Clone)]
/// SQLite storage for MLS pre-shared keys.
pub struct SqLitePreSharedKeyStorage {
    connection: Arc<Mutex<Connection>>,
}

impl SqLitePreSharedKeyStorage {
    pub(crate) fn new(connection: Connection) -> SqLitePreSharedKeyStorage {
        SqLitePreSharedKeyStorage {
            connection: Arc::new(Mutex::new(connection)),
        }
    }

    /// Insert a pre-shared key into storage.
    pub fn insert(&self, psk_id: &[u8], psk: &PreSharedKey) -> Result<(), SqLiteDataStorageError> {
        let connection = self.connection.lock().unwrap();

        connection
            .execute(
                "INSERT INTO psk (psk_id, data) VALUES (?,?) ON CONFLICT(psk_id) DO UPDATE SET data=excluded.data",
                params![psk_id, psk.deref()],
            )
            .map(|_| ())
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))
    }

    /// Get a pre-shared key from storage based on a unique id.
    pub fn get(&self, psk_id: &[u8]) -> Result<Option<PreSharedKey>, SqLiteDataStorageError> {
        let connection = self.connection.lock().unwrap();

        connection
            .query_row(
                "SELECT data FROM psk WHERE psk_id = ?",
                params![psk_id],
                |row| Ok(PreSharedKey::new(row.get(0)?)),
            )
            .optional()
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))
    }

    /// Delete a pre-shared key from storage based on a unique id.
    pub fn delete(&self, psk_id: &[u8]) -> Result<(), SqLiteDataStorageError> {
        let connection = self.connection.lock().unwrap();

        connection
            .execute("DELETE FROM psk WHERE psk_id = ?", params![psk_id])
            .map(|_| ())
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))
    }
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
#[cfg_attr(mls_build_async, maybe_async::must_be_async)]
impl PreSharedKeyStorage for SqLitePreSharedKeyStorage {
    type Error = SqLiteDataStorageError;

    async fn get(&self, id: &ExternalPskId) -> Result<Option<PreSharedKey>, Self::Error> {
        self.get(id)
            .map_err(|e| SqLiteDataStorageError::DataConversionError(e.into()))
    }
}
