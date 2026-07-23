/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use crate::query::{
    CRLiteCoverage, CRLiteKey, CRLiteQuery, IssuerSpkiHash, LogId, Timestamp, TimestampInterval,
};
use clubcard::{AsQuery, Equation, Filterable};
use serde::Deserialize;
use std::collections::HashMap;

use base64::Engine;
use std::io::Read;

impl CRLiteCoverage {
    pub fn from_mozilla_ct_logs_json<T>(reader: T) -> Self
    where
        T: Read,
    {
        #[allow(non_snake_case)]
        #[derive(Deserialize)]
        struct MozillaCtLogsJson {
            LogID: String,
            MaxTimestamp: u64,
            MinTimestamp: u64,
            MMD: u64,
            MinEntry: u64,
        }

        let mut coverage = HashMap::new();
        let json_entries: Vec<MozillaCtLogsJson> = match serde_json::from_reader(reader) {
            Ok(json_entries) => json_entries,
            _ => return CRLiteCoverage(Default::default()),
        };
        for entry in json_entries {
            let mut log_id = [0u8; 32];
            match base64::prelude::BASE64_STANDARD.decode(&entry.LogID) {
                Ok(bytes) if bytes.len() == 32 => log_id.copy_from_slice(&bytes),
                _ => continue,
            };
            let Some(entry_mmd_ms) = entry.MMD.checked_mul(1000) else {
                continue;
            };
            let low = Timestamp(if entry.MinEntry == 0 {
                entry.MinTimestamp
            } else {
                entry.MinTimestamp + entry_mmd_ms
            });
            let high = Timestamp(entry.MaxTimestamp.saturating_sub(entry_mmd_ms));
            if low < high {
                coverage.insert(LogId(log_id), TimestampInterval { low, high });
            }
        }
        CRLiteCoverage(coverage)
    }
}

pub struct CRLiteBuilderItem {
    /// issuer spki hash
    issuer: IssuerSpkiHash,
    /// serial number. TODO: smallvec?
    serial: Vec<u8>,
    /// revocation status
    revoked: bool,
}

impl CRLiteBuilderItem {
    pub fn revoked(issuer: IssuerSpkiHash, serial: Vec<u8>) -> Self {
        Self {
            issuer,
            serial,
            revoked: true,
        }
    }

    pub fn not_revoked(issuer: IssuerSpkiHash, serial: Vec<u8>) -> Self {
        Self {
            issuer,
            serial,
            revoked: false,
        }
    }
}

impl AsQuery<4> for CRLiteBuilderItem {
    fn as_query(&self, m: usize) -> Equation<4> {
        let crlite_key = CRLiteKey::new(&self.issuer, &self.serial);
        let crlite_query = CRLiteQuery::new(&crlite_key, None);
        crlite_query.as_query(m)
    }

    fn block(&self) -> &[u8] {
        &self.issuer.0
    }

    fn discriminant(&self) -> &[u8] {
        &self.serial
    }
}

impl Filterable<4> for CRLiteBuilderItem {
    fn included(&self) -> bool {
        self.revoked
    }
}
