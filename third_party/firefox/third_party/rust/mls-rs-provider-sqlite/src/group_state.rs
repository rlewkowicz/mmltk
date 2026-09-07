// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use mls_rs_core::{
    group::{EpochRecord, GroupState, GroupStateStorage},
    mls_rs_codec::MlsEncode,
};
use rusqlite::{params, Connection, OptionalExtension};
use std::{
    fmt::Debug,
    sync::{Arc, Mutex},
};
use zeroize::Zeroizing;

use crate::SqLiteDataStorageError;

pub(crate) const DEFAULT_EPOCH_RETENTION_LIMIT: u64 = 3;

#[derive(Debug, Clone)]
/// SQLite Storage for MLS group states.
///
/// # Limitations
///
/// Epoch IDs are stored as SQLite INTEGER (signed 64-bit), limiting the maximum
/// epoch ID to [`i64::MAX`] (9,223,372,036,854,775,807). Operations with epoch IDs
/// exceeding this value will return [`SqLiteDataStorageError::EpochIdOverflow`].
pub struct SqLiteGroupStateStorage {
    connection: Arc<Mutex<Connection>>,
    max_epoch_retention: u64,
    state_context: Option<Vec<u8>>,
}

impl SqLiteGroupStateStorage {
    pub(crate) fn new(
        connection: Connection,
        state_context: Option<Vec<u8>>,
    ) -> SqLiteGroupStateStorage {
        SqLiteGroupStateStorage {
            connection: Arc::new(Mutex::new(connection)),
            max_epoch_retention: DEFAULT_EPOCH_RETENTION_LIMIT,
            state_context,
        }
    }

    pub fn with_max_epoch_retention(self, max_epoch_retention: u64) -> Self {
        Self {
            connection: self.connection,
            max_epoch_retention,
            state_context: self.state_context,
        }
    }

    /// List all the group ids for groups that are stored.
    pub fn group_ids(&self) -> Result<Vec<Vec<u8>>, SqLiteDataStorageError> {
        let connection = self.connection.lock().unwrap();

        let mut statement = connection
            .prepare("SELECT group_id FROM mls_group")
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))?;

        let res = statement
            .query_map([], |row| row.get(0))
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))?
            .try_fold(Vec::new(), |mut ids, id| {
                ids.push(id.map_err(|e| SqLiteDataStorageError::DataConversionError(e.into()))?);
                Ok::<_, SqLiteDataStorageError>(ids)
            })
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))?;

        Ok(res)
    }

    /// Delete a group from storage.
    pub fn delete_group(&self, group_id: &[u8]) -> Result<(), SqLiteDataStorageError> {
        let connection = self.connection.lock().unwrap();

        let alternative_gid = self.alternative_group_id(group_id)?;
        let group_id = alternative_gid.as_deref().unwrap_or(group_id);

        connection
            .execute(
                "DELETE FROM mls_group WHERE group_id = ?",
                params![group_id],
            )
            .map(|_| ())
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))
    }

    pub fn max_epoch_retention(&self) -> u64 {
        self.max_epoch_retention
    }

    fn get_snapshot_data(
        &self,
        group_id: &[u8],
    ) -> Result<Option<Vec<u8>>, SqLiteDataStorageError> {
        let connection = self.connection.lock().unwrap();

        let alternative_gid = self.alternative_group_id(group_id)?;
        let group_id = alternative_gid.as_deref().unwrap_or(group_id);

        connection
            .query_row(
                "SELECT snapshot FROM mls_group where group_id = ?",
                [group_id],
                |row| row.get::<_, Vec<u8>>(0),
            )
            .optional()
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))
    }

    fn get_epoch_data(
        &self,
        group_id: &[u8],
        epoch_id: u64,
    ) -> Result<Option<Vec<u8>>, SqLiteDataStorageError> {
        let connection = self.connection.lock().unwrap();

        let alternative_gid = self.alternative_group_id(group_id)?;
        let group_id = alternative_gid.as_deref().unwrap_or(group_id);

        connection
            .query_row(
                "SELECT epoch_data FROM epoch where group_id = ? AND epoch_id = ?",
                params![
                    group_id,
                    i64::try_from(epoch_id)
                        .map_err(|_| SqLiteDataStorageError::EpochIdOverflow(epoch_id))?
                ],
                |row| row.get::<_, Vec<u8>>(0),
            )
            .optional()
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))
    }

    fn max_epoch_id(&self, group_id: &[u8]) -> Result<Option<u64>, SqLiteDataStorageError> {
        let connection = self.connection.lock().unwrap();

        connection
            .query_row(
                "SELECT MAX(epoch_id) FROM epoch WHERE group_id = ?",
                params![group_id],
                |row| {
                    row.get::<_, Option<i64>>(0).and_then(|opt| {
                        opt.map(|v| {
                            u64::try_from(v)
                                .map_err(|_| rusqlite::Error::IntegralValueOutOfRange(0, v))
                        })
                        .transpose()
                    })
                },
            )
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))
    }

    fn update_group_state(
        &self,
        group_id: &[u8],
        group_snapshot: &[u8],
        inserts: Vec<EpochRecord>,
        updates: Vec<EpochRecord>,
    ) -> Result<(), SqLiteDataStorageError> {
        let mut max_epoch_id = None;

        let alternative_gid = self.alternative_group_id(group_id)?;
        let group_id = alternative_gid.as_deref().unwrap_or(group_id);

        let mut connection = self.connection.lock().unwrap();
        let transaction = connection
            .transaction()
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))?;

        transaction.execute(
            "INSERT INTO mls_group (group_id, snapshot) VALUES (?, ?) ON CONFLICT(group_id) DO UPDATE SET snapshot=excluded.snapshot",
            params![group_id, group_snapshot],
        ).map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))?;

        for epoch in inserts {
            max_epoch_id = Some(epoch.id);

            transaction
                .execute(
                    "INSERT INTO epoch (group_id, epoch_id, epoch_data) VALUES (?, ?, ?)",
                    params![
                        group_id,
                        i64::try_from(epoch.id)
                            .map_err(|_| SqLiteDataStorageError::EpochIdOverflow(epoch.id))?,
                        &*epoch.data
                    ],
                )
                .map(|_| ())
                .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))?;
        }

        updates.into_iter().try_for_each(|epoch| {
            transaction
                .execute(
                    "UPDATE epoch SET epoch_data = ? WHERE group_id = ? AND epoch_id = ?",
                    params![
                        &*epoch.data,
                        group_id,
                        i64::try_from(epoch.id)
                            .map_err(|_| SqLiteDataStorageError::EpochIdOverflow(epoch.id))?
                    ],
                )
                .map(|_| ())
                .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))
        })?;

        if let Some(max_epoch_id) = max_epoch_id {
            if max_epoch_id >= self.max_epoch_retention {
                let delete_under = max_epoch_id - self.max_epoch_retention;

                transaction
                    .execute(
                        "DELETE FROM epoch WHERE group_id = ? AND epoch_id <= ?",
                        params![
                            group_id,
                            i64::try_from(delete_under).map_err(|_| {
                                SqLiteDataStorageError::EpochIdOverflow(delete_under)
                            })?
                        ],
                    )
                    .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))?;
            }
        }

        transaction
            .commit()
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))
    }

    fn alternative_group_id(
        &self,
        group_id: &[u8],
    ) -> Result<Option<Vec<u8>>, SqLiteDataStorageError> {
        self.state_context
            .as_ref()
            .map(|context| {
                (context, group_id)
                    .mls_encode_to_vec()
                    .map_err(|e| SqLiteDataStorageError::DataConversionError(Box::new(e)))
            })
            .transpose()
    }
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
#[cfg_attr(mls_build_async, maybe_async::must_be_async)]
impl GroupStateStorage for SqLiteGroupStateStorage {
    type Error = SqLiteDataStorageError;

    async fn write(
        &mut self,
        state: GroupState,
        inserts: Vec<EpochRecord>,
        updates: Vec<EpochRecord>,
    ) -> Result<(), Self::Error> {
        self.update_group_state(&state.id, &state.data, inserts, updates)
    }

    async fn state(&self, group_id: &[u8]) -> Result<Option<Zeroizing<Vec<u8>>>, Self::Error> {
        let data = self.get_snapshot_data(group_id)?;
        Ok(data.map(Into::into))
    }

    async fn max_epoch_id(&self, group_id: &[u8]) -> Result<Option<u64>, Self::Error> {
        self.max_epoch_id(group_id)
    }

    async fn epoch(
        &self,
        group_id: &[u8],
        epoch_id: u64,
    ) -> Result<Option<Zeroizing<Vec<u8>>>, Self::Error> {
        let data = self.get_epoch_data(group_id, epoch_id)?;
        Ok(data.map(Into::into))
    }
}
