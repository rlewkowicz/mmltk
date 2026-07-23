/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use adler2::adler32_slice;
use log::warn;
use nsstring::nsCString;
use serde::{Deserialize, Serialize};
use static_assertions::const_assert;
use std::ffi::c_void;
use std::io::Write as _;
use std::path::Path;
use thin_vec::ThinVec;

/// Callback type for [`ssl_tokens_cache_read`].
pub type SslTokensReadCallback =
    unsafe extern "C" fn(ctx: *mut c_void, record: *const SslTokensPersistedRecord);

/// FFI-safe representation of one persisted token record.
#[repr(C)]
pub struct SslTokensPersistedRecord {
    pub id: u64,
    pub key: nsCString,
    pub expiration_time: PrTime,
    /// Cached for `ShouldPersistKey` filtering without decompressing the payload.
    pub overridable_error: u8,
    pub compressed_payload: *const u8,
    pub compressed_payload_len: usize,
}

#[derive(Clone, Serialize, Deserialize)]
#[expect(
    clippy::unsafe_derive_deserialize,
    reason = "from_record is unrelated to deserialization"
)]
struct PersistedRecord {
    id: u64,
    key: Vec<u8>,
    expiration_time: PrTime,
    overridable_error: u8,
    compressed_payload: Vec<u8>,
}

impl PersistedRecord {
    /// # Safety
    ///
    /// `compressed_payload` must be valid for `compressed_payload_len` bytes.
    unsafe fn from_record(rec: &SslTokensPersistedRecord) -> Self {
        let key = rec.key.as_ref().to_vec();
        let compressed_payload = unsafe {
            std::slice::from_raw_parts(rec.compressed_payload, rec.compressed_payload_len)
        }
        .to_vec();
        Self {
            id: rec.id,
            key,
            expiration_time: rec.expiration_time,
            overridable_error: rec.overridable_error,
            compressed_payload,
        }
    }

    fn with_record<F: FnOnce(&SslTokensPersistedRecord)>(&self, f: F) {
        let rec = SslTokensPersistedRecord {
            id: self.id,
            key: nsCString::from(self.key.as_slice()),
            expiration_time: self.expiration_time,
            overridable_error: self.overridable_error,
            compressed_payload: self.compressed_payload.as_ptr(),
            compressed_payload_len: self.compressed_payload.len(),
        };
        f(&rec);
    }
}

/// Microseconds since the Unix epoch, matching the C++ `PRTime` type.
type PrTime = i64;

const MAGIC: [u8; 4] = *b"STCF";
const VERSION: u8 = 3;
/// File layout: magic(4) + version(1) + `bincode_body(N)` + `adler32_le(4)`.
/// VERSION 3: each record stores a single compressed payload (token + cert
/// info together) instead of separate fields.
const HEADER_SIZE: usize = MAGIC.len() + size_of::<u8>();
const_assert!(HEADER_SIZE == 5);
/// Sanity cap on the bincode body to guard against corrupt size fields.
const MAX_PAYLOAD_SIZE: usize = 16 * 1024 * 1024;

#[derive(Debug)]
enum ParseError {
    BadMagic,
    BadVersion,
    Truncated,
}

fn to_file_bytes(records: &[PersistedRecord], magic: [u8; 4]) -> Vec<u8> {
    let body = bincode::serialize(records).unwrap_or_default();
    let checksum = adler32_slice(&body).to_le_bytes();
    let mut out = Vec::with_capacity(HEADER_SIZE + body.len() + 4);
    out.extend_from_slice(&magic);
    out.push(VERSION);
    out.extend_from_slice(&body);
    out.extend_from_slice(&checksum);
    out
}

fn from_file_bytes(
    data: &[u8],
    expected_magic: [u8; 4],
) -> Result<Vec<PersistedRecord>, ParseError> {
    let Some(([magic @ .., version], rest)) = data.split_first_chunk::<HEADER_SIZE>() else {
        return Err(ParseError::Truncated);
    };
    if magic != &expected_magic {
        return Err(ParseError::BadMagic);
    }
    if *version != VERSION {
        return Err(ParseError::BadVersion);
    }
    let Some((body, stored)) = rest.split_last_chunk::<4>() else {
        return Err(ParseError::Truncated);
    };
    if body.len() > MAX_PAYLOAD_SIZE {
        return Err(ParseError::Truncated);
    }
    if adler32_slice(body).to_le_bytes() != *stored {
        return Err(ParseError::Truncated);
    }
    bincode::deserialize::<Vec<PersistedRecord>>(body).map_err(|_| ParseError::Truncated)
}

/// Reads `bin_path`, falling back to `bin_path.with_extension("tmp")` if the
/// canonical file is absent (crash-mid-rename recovery). Discards a stale .tmp
/// when the .bin is present. Returns `(data, loaded_from_tmp)` or `None`.
fn read_file_with_tmp_fallback(bin_path: &Path) -> Option<(Vec<u8>, bool)> {
    let tmp_path = bin_path.with_extension("tmp");
    std::fs::read(bin_path)
        .map(|data| {
            _ = std::fs::remove_file(&tmp_path);
            (data, false)
        })
        .or_else(|_| std::fs::read(&tmp_path).map(|data| (data, true)))
        .ok()
}

fn nscstring_as_path(s: &nsCString) -> Option<&Path> {
    std::str::from_utf8(s.as_ref()).ok().map(Path::new)
}

fn write_atomically(buf: &[u8], bin_path: &Path) -> std::io::Result<()> {
    let tmp_path = bin_path.with_extension("tmp");
    let mut f = std::fs::File::create(&tmp_path)?; 
    f.write_all(buf)?;
    f.sync_all()?;
    std::fs::rename(tmp_path, bin_path)
}

/// Calls `callback` for each non-expired record, passing a stack-allocated FFI
/// struct. The pointer passed to the callback is only valid during the call.
///
/// # Safety
///
/// `callback` must be a valid function pointer. `ctx` must remain valid for the
/// duration of this call.
unsafe fn dispatch_records(
    records: &[PersistedRecord],
    now: PrTime,
    callback: SslTokensReadCallback,
    ctx: *mut c_void,
) {
    for rec in records.iter().filter(|r| r.expiration_time > now) {
        rec.with_record(|c_rec| unsafe { callback(ctx, &raw const *c_rec) });
    }
}

/// Reads the persisted file and calls `callback` for each valid record.
///
/// # Safety
///
/// `callback` must be a valid function pointer. `ctx` must remain valid for
/// the duration of this call. The `callback` is invoked with a pointer to a
/// stack-allocated FFI struct; the pointer is only valid inside the callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ssl_tokens_cache_read(
    path: &nsCString,
    now: PrTime,
    callback: SslTokensReadCallback,
    ctx: *mut c_void,
) {
    let Some(bin_path) = nscstring_as_path(path) else {
        return;
    };

    let Some((data, loaded_from_tmp)) = read_file_with_tmp_fallback(bin_path) else {
        return;
    };

    let records = match from_file_bytes(&data, MAGIC) {
        Ok(r) => r,
        Err(e) => {
            let bad = if loaded_from_tmp {
                bin_path.with_extension("tmp")
            } else {
                bin_path.to_path_buf()
            };
            warn!(
                "SslTokensCache: parse error ({e:?}), discarding {}",
                bad.display()
            );
            _ = std::fs::remove_file(&bad);
            return;
        }
    };

    if loaded_from_tmp {
        _ = std::fs::rename(bin_path.with_extension("tmp"), bin_path);
    }

    unsafe {
        dispatch_records(&records, now, callback, ctx);
    }
}

/// Serializes `records` to STCF format, appending the bytes to `out`.
/// Stateless — does not touch any global state.
///
/// # Safety
///
/// Each record's `token` pointer must be valid and stable (no reallocation
/// of the owning buffer) for `token_len` bytes for the duration of this
/// call. `records` and `out` must be valid non-null references.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ssl_tokens_cache_serialize_snapshot(
    records: &ThinVec<SslTokensPersistedRecord>,
    out: &mut ThinVec<u8>,
) {
    let persisted: Vec<_> = records
        .iter()
        .map(|r| unsafe { PersistedRecord::from_record(r) })
        .collect();
    out.extend_from_slice(&to_file_bytes(&persisted, MAGIC));
}

/// Writes `data` atomically to `path` via the .tmp + rename dance.
///
/// # Safety
///
/// `path` and `data` must be valid non-null references.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ssl_tokens_cache_write_bytes(path: &nsCString, data: &ThinVec<u8>) {
    let Some(path) = nscstring_as_path(path) else {
        return;
    };
    if let Err(e) = write_atomically(data, path) {
        warn!("SslTokensCache: write failed: {e}");
    }
}

/// Parses an STCF-format buffer and dispatches each non-expired record to
/// `callback`.
///
/// # Safety
///
/// `data` must point to `data_len` valid bytes. `callback` must be a valid
/// function pointer. `ctx` must remain valid for the duration of this call.
/// The pointer passed to `callback` is stack-allocated and is only valid inside
/// the callback. `callback` is always invoked synchronously — it is never
/// called after this function returns, so callers may safely pass pointers to
/// stack-allocated context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ssl_tokens_cache_deserialize_ipc(
    data: *const u8,
    data_len: usize,
    now: PrTime,
    callback: SslTokensReadCallback,
    ctx: *mut c_void,
) {
    let bytes = unsafe { std::slice::from_raw_parts(data, data_len) };
    let records = match from_file_bytes(bytes, MAGIC) {
        Ok(r) => r,
        Err(e) => {
            warn!("SslTokensCache: IPC deserialize error ({e:?})");
            return;
        }
    };
    unsafe {
        dispatch_records(&records, now, callback, ctx);
    }
}
