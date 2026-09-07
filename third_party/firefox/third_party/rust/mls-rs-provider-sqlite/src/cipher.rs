// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::connection_strategy::ConnectionStrategy;
use crate::SqLiteDataStorageError;
use rusqlite::Connection;

use hex::ToHex;
use zeroize::{ZeroizeOnDrop, Zeroizing};

#[allow(dead_code)]
#[derive(Debug, ZeroizeOnDrop, Clone)]
/// Representation of a SQLCipher key used to unlock a database.
pub enum SqlCipherKey {
    /// Passphrase based key.
    Passphrase(String),
    /// Raw key material without a salt value.
    RawKey([u8; 32]),
    /// Raw key material with a salt value.
    RawKeyWithSalt([u8; 48]),
}

fn blob_string_repr(val: &[u8]) -> String {
    format!("x'{}'", val.encode_hex_upper::<String>())
}

impl SqlCipherKey {
    fn to_key_pragma_value(&self) -> Zeroizing<String> {
        Zeroizing::new(match self {
            SqlCipherKey::Passphrase(pass) => pass.clone(),
            SqlCipherKey::RawKey(key) => blob_string_repr(key.as_slice()),
            SqlCipherKey::RawKeyWithSalt(key) => blob_string_repr(key.as_slice()),
        })
    }
}

#[derive(Debug, Clone)]
/// SQLCipher connection config.
pub struct SqlCipherConfig {
    key: SqlCipherKey,
    plaintext_header_size: u8,
}

impl SqlCipherConfig {
    /// Create a new config with a specific key.
    pub fn new(key: SqlCipherKey) -> SqlCipherConfig {
        SqlCipherConfig {
            key,
            plaintext_header_size: 0,
        }
    }

    /// Adjust the plaintext header size.
    pub fn with_plaintext_header(self, size: u8) -> SqlCipherConfig {
        SqlCipherConfig {
            plaintext_header_size: size,
            ..self
        }
    }
}

/// Encrypted database connection with SQLCipher.
pub struct CipheredConnectionStrategy<I>
where
    I: ConnectionStrategy,
{
    inner: I,
    cipher_config: SqlCipherConfig,
}

impl<CS> CipheredConnectionStrategy<CS>
where
    CS: ConnectionStrategy,
{
    /// Create a new SQLCipher connection that inherits another connection strategy.
    pub fn new(strategy: CS, cipher_config: SqlCipherConfig) -> CipheredConnectionStrategy<CS> {
        CipheredConnectionStrategy {
            inner: strategy,
            cipher_config,
        }
    }
}

impl<I> ConnectionStrategy for CipheredConnectionStrategy<I>
where
    I: ConnectionStrategy,
{
    fn make_connection(&self) -> Result<Connection, SqLiteDataStorageError> {
        if self.cipher_config.plaintext_header_size > 0
            && !matches!(self.cipher_config.key, SqlCipherKey::RawKeyWithSalt(_))
        {
            return Err(SqLiteDataStorageError::SqlCipherKeyInvalidWithHeader);
        }

        let connection = self.inner.make_connection()?;

        connection
            .pragma_update(
                None,
                "key",
                self.cipher_config.key.to_key_pragma_value().as_str(),
            )
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))?;

        connection
            .pragma_update(
                None,
                "cipher_plaintext_header_size",
                self.cipher_config.plaintext_header_size,
            )
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))?;

        connection
            .query_row("SELECT count(*) FROM sqlite_master", [], |_| Ok(()))
            .map_err(|e| SqLiteDataStorageError::SqlEngineError(e.into()))?;

        Ok(connection)
    }
}
