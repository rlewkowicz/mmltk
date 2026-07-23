/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! A single store.

use std::{
    borrow::Cow,
    ffi::OsStr,
    fmt::Write,
    io, mem,
    ops::Deref,
    path::{Path, PathBuf},
    sync::{
        atomic::{self, AtomicUsize},
        Arc, Condvar, Mutex,
    },
    time::SystemTime,
};

use chrono::{DateTime, Utc};
use rusqlite::OpenFlags;

use crate::{
    checker::{Checker, CheckerAction, IntoChecker},
    connection::{
        Connection, ConnectionIncident, ConnectionIncidents, ConnectionMaintenanceTask,
        ConnectionPath, ConnectionType, ToConnectionIncident,
    },
    schema::{Schema, SchemaError},
};

/// A persistent store backed by a physical SQLite database.
///
/// Under the hood, a store holds two connections to the same physical database:
///
/// * A **read-write** connection for queries and updates. This connection
///   runs operations serially, and those operations can't be interrupted.
/// * A **read-only** connection for concurrent reads. This connection can
///   read from the physical database even if the read-write connection is busy,
///   and those reads can be interrupted. Reads on this connection won't see any
///   uncommitted changes on the read-write connection.
#[derive(Debug)]
pub struct Store {
    path: StorePath,
    state: Mutex<StoreState>,
    waiter: OperationWaiter,
}

impl Store {
    pub fn new(path: StorePath) -> Self {
        Self {
            path,
            state: Mutex::new(StoreState::Created),
            waiter: OperationWaiter::new(),
        }
    }

    /// Gets or opens both connections to the physical database.
    fn open<C>(&self) -> Result<OpenStoreGuard<'_>, StoreError>
    where
        for<'a> ConnectionIncidents<'a>: IntoChecker<C>,
        C: ConnectionMaintenanceTask,
        C::Error: std::error::Error + Send + Sync + 'static,
    {
        let guard = {
            let mut state = self.state.lock().unwrap();
            loop {
                let result = match &*state {
                    StoreState::Created => {
                        let store = Arc::new(OpenStore::new(&self.path)?);
                        *state = StoreState::Open(store);
                        continue;
                    }
                    StoreState::Open(store) => {
                        let store = store.clone();
                        match IntoChecker::<C>::into_checker(store.writer.incidents()) {
                            CheckerAction::Skip => {
                                let guard = OpenStoreGuard::new(store, self.waiter.guard());
                                Ok(CheckedStore::Healthy(guard))
                            }
                            CheckerAction::Check(checker) => {
                                let writer =
                                    Writer(OpenStoreGuard::new(store.clone(), self.waiter.guard()));
                                *state = StoreState::Maintenance(store);
                                Err(UnhealthyStore::Check(writer, checker))
                            }
                            CheckerAction::Replace => {
                                *state = StoreState::Corrupt;
                                Err(UnhealthyStore::Replace(store))
                            }
                        }
                    }
                    StoreState::Maintenance(_) => return Err(StoreError::Busy),
                    StoreState::Corrupt => return Err(StoreError::Corrupt),
                    StoreState::Closed => return Err(StoreError::Closed),
                };
                break result;
            }
        }
        .or_else(|store| {
            match store {
                UnhealthyStore::Replace(store) => {
                    Ok(CheckedStore::Corrupt(store, StoreError::Corrupt))
                }
                UnhealthyStore::Check(writer, checker) => {
                    let result = writer
                        .maintenance(checker)
                        .map_err(|err| StoreError::Maintenance(err.into()));
                    {
                        let mut state = self.state.lock().unwrap();
                        let StoreState::Maintenance(store) = &*state else {
                            return result.and_then(|_| {
                                Err(StoreError::Closed)
                            });
                        };
                        let store = store.clone();
                        match result {
                            Ok(()) => {
                                let guard = OpenStoreGuard::new(store.clone(), self.waiter.guard());
                                *state = StoreState::Open(store);
                                Ok(CheckedStore::Healthy(guard))
                            }
                            Err(err) => {
                                *state = StoreState::Corrupt;
                                Ok(CheckedStore::Corrupt(store, err))
                            }
                        }
                    }
                }
            }
        })?;

        match guard {
            CheckedStore::Healthy(guard) => Ok(guard),
            CheckedStore::Corrupt(store, err) => {
                store.reader.interrupt();
                store.writer.interrupt();

                self.waiter.wait();

                let store = Arc::into_inner(store).expect("invariant violation");

                store.close();

                if let Some(path) = self.path.on_disk() {
                    rename_corrupt_database_file(&path);
                }

                Err(err)
            }
        }
    }

    /// Returns the read-write connection to use for queries and updates.
    pub fn writer(&self) -> Result<Writer<'_>, StoreError> {
        Ok(Writer(self.open::<Checker>()?))
    }

    /// Returns the read-only connection to use for concurrent reads.
    pub fn reader(&self) -> Result<Reader<'_>, StoreError> {
        Ok(Reader(self.open::<Checker>()?))
    }

    /// Closes both connections to the physical database.
    pub fn close(&self) {
        let store = match mem::replace(&mut *self.state.lock().unwrap(), StoreState::Closed) {
            StoreState::Created | StoreState::Closed | StoreState::Corrupt => return,
            StoreState::Open(store) => {
                store.reader.interrupt();
                store
            }
            StoreState::Maintenance(store) => {
                store.reader.interrupt();
                store.writer.interrupt();
                store
            }
        };

        self.waiter.wait();

        let store = Arc::into_inner(store).expect("invariant violation");

        store.close();
    }
}

/// Either a path to a physical SQLite database file on disk, or
/// a reference to a unique in-memory database.
#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum StorePath {
    OnDisk(PathBuf),
    InMemory(usize),
}

impl StorePath {
    pub const IN_MEMORY_DATABASE_NAME: &'static str = ":memory:";

    const DEFAULT_DATABASE_FILE_NAME: &'static str = "kvstore.sqlite";

    /// Returns the canonical [`StorePath`] for a [`WidePathBuf`]. This
    /// method normalizes string paths passed to the XPCOM
    /// [`crate::skv::interface`] methods.
    ///
    /// **Canonicalization can access the filesystem**, so this method
    /// should not be called on the main thread.
    pub fn canonicalizing(path: PathBuf) -> Result<Self, StoreError> {
        Ok(if path.as_os_str() == StorePath::IN_MEMORY_DATABASE_NAME {
            StorePath::for_in_memory()
        } else {
            let dir = path.canonicalize().map_err(StoreError::StorageDir)?;
            StorePath::for_storage_dir(dir)
        })
    }

    /// Returns the path to the physical database file in the given
    /// storage directory.
    pub fn for_storage_dir(dir: impl Into<PathBuf>) -> Self {
        let mut path = dir.into();
        path.push(Self::DEFAULT_DATABASE_FILE_NAME);
        Self::OnDisk(path)
    }

    /// Returns a path to a unique in-memory physical database.
    pub fn for_in_memory() -> Self {
        static NEXT_IN_MEMORY_DATABASE_ID: AtomicUsize = AtomicUsize::new(1);
        let id = NEXT_IN_MEMORY_DATABASE_ID.fetch_add(1, atomic::Ordering::Relaxed);
        Self::InMemory(id)
    }

    /// If this path is to a physical database file on disk,
    /// returns a reference to the path.
    pub fn on_disk(&self) -> Option<OnDiskStorePath<'_>> {
        match self {
            Self::OnDisk(buf) => buf
                .file_name()
                .map(|name| OnDiskStorePath::new(buf.parent(), name.into())),
            Self::InMemory(_) => None,
        }
    }
}

impl ConnectionPath for StorePath {
    fn as_path(&self) -> Cow<'_, Path> {
        match self {
            Self::OnDisk(buf) => Cow::Borrowed(buf.as_path()),
            Self::InMemory(id) => {
                Cow::Owned(format!("file:kvstore-{id}?mode=memory&cache=shared").into())
            }
        }
    }

    fn flags(&self) -> OpenFlags {
        match self {
            Self::OnDisk(_) => OpenFlags::empty(),
            Self::InMemory(_) => {
                OpenFlags::SQLITE_OPEN_URI
            }
        }
    }
}

/// A path to an SQLite database file and its related files on disk.
#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct OnDiskStorePath<'a> {
    dir: Option<&'a Path>,
    name: Cow<'a, OsStr>,
}

impl<'a> OnDiskStorePath<'a> {
    fn new(dir: Option<&'a Path>, name: Cow<'a, OsStr>) -> Self {
        Self { dir, name }
    }

    /// Returns the path to the temporary WAL file.
    pub fn wal(&self) -> PathBuf {
        let mut name = self.name.clone().into_owned();
        write!(&mut name, "-wal").unwrap();
        self.dir.map(|dir| dir.join(&name)).unwrap_or(name.into())
    }

    /// Returns the path to the temporary shared-memory file.
    pub fn shm(&self) -> PathBuf {
        let mut name = self.name.clone().into_owned();
        write!(&mut name, "-shm").unwrap();
        self.dir.map(|dir| dir.join(&name)).unwrap_or(name.into())
    }

    /// Returns the path to use for backing up a corrupt database file
    /// and its related files.
    pub fn to_corrupt(&self) -> OnDiskStorePath<'a> {
        let now = DateTime::<Utc>::from(SystemTime::now());
        let mut name = self.name.clone().into_owned();
        write!(&mut name, ".corrupt-{}", now.format("%Y%m%d%H%M%S")).unwrap();
        Self::new(self.dir, name.into())
    }
}

impl<'a> ConnectionPath for OnDiskStorePath<'a> {
    fn as_path(&self) -> Cow<'_, Path> {
        match self.dir {
            Some(dir) => Cow::Owned(dir.join(&self.name)),
            None => Cow::Borrowed(Path::new(&self.name)),
        }
    }

    fn flags(&self) -> OpenFlags {
        OpenFlags::empty()
    }
}

/// Backs up a corrupt SQLite database file and its related files.
fn rename_corrupt_database_file(source: &OnDiskStorePath<'_>) {
    let destination = source.to_corrupt();

    let _ = std::fs::rename(source.as_path(), destination.as_path());
    let _ = std::fs::rename(source.wal(), destination.wal());
    let _ = std::fs::rename(source.shm(), destination.shm());
}

/// A strong reference to an open store.
struct OpenStoreGuard<'a> {
    store: Arc<OpenStore>,
    _guard: OperationGuard<'a>,
}

impl<'a> OpenStoreGuard<'a> {
    fn new(store: Arc<OpenStore>, guard: OperationGuard<'a>) -> Self {
        Self {
            store,
            _guard: guard,
        }
    }
}

/// A read-write connection to an SQLite database.
pub struct Writer<'a>(OpenStoreGuard<'a>);

impl<'a> Deref for Writer<'a> {
    type Target = Connection;

    fn deref(&self) -> &Self::Target {
        &self.0.store.writer
    }
}

/// A read-only connection to an SQLite database.
pub struct Reader<'a>(OpenStoreGuard<'a>);

impl<'a> Deref for Reader<'a> {
    type Target = Connection;

    fn deref(&self) -> &Self::Target {
        &self.0.store.reader
    }
}

/// The internal state of a [`Store`].
///
/// ## State diagram
///
/// ```custom
/// +---------+
/// | Created +-----------------------------------+
/// +--+------+                                   |
///    |                                          |
/// +--v---+    +-------------+    +---------+    |
/// | Open +----> Maintenance +----> Corrupt |    |
/// +-+--^-+    +---v--v------+    +----+----+    |
///   |  |          |  |                |         |
///   |  +----------+  |                |         |
///   |                |                |         |
///   | +--------------+                |         |
///   | |                               |         |
///   | | +-----------------------------+         |
///   | | |                                       |
/// +-v-v-v--+                                    |
/// | Closed <------------------------------------+
/// +--------+
/// ```
#[derive(Debug)]
enum StoreState {
    Created,
    Open(Arc<OpenStore>),
    Maintenance(Arc<OpenStore>),
    Corrupt,
    Closed,
}

#[derive(Debug)]
struct OpenStore {
    writer: Connection,
    reader: Connection,
}

impl OpenStore {
    fn new(path: &StorePath) -> Result<Self, StoreError> {
        Ok(match Self::open(path) {
            Ok(store) => store,
            Err(StoreError::Sqlite(err)) => {
                let (Some(code), Some(path)) = (err.sqlite_error_code(), path.on_disk()) else {
                    return Err(err.into());
                };
                match code {
                    rusqlite::ErrorCode::NotADatabase | rusqlite::ErrorCode::DatabaseCorrupt => {
                        rename_corrupt_database_file(&path);
                        Self::open(&path)?
                    }
                    _ => return Err(err.into()),
                }
            }
            Err(err) => return Err(err),
        })
    }

    fn open(path: &impl ConnectionPath) -> Result<Self, StoreError> {
        let writer = Connection::new::<Schema>(path, ConnectionType::ReadWrite)?;
        let reader = Connection::new::<Schema>(path, ConnectionType::ReadOnly)?;
        Ok(Self { writer, reader })
    }

    fn close(self) {
        self.reader.close();
        self.writer.close();
    }
}

/// A temporarily out-of-service store.
enum UnhealthyStore<'a, C> {
    Check(Writer<'a>, C),
    Replace(Arc<OpenStore>),
}

/// An out-of-service store that was checked for corruption.
enum CheckedStore<'a> {
    Healthy(OpenStoreGuard<'a>),
    Corrupt(Arc<OpenStore>, StoreError),
}

#[derive(Debug)]
struct OperationWaiter {
    count: Mutex<usize>,
    cvar: Condvar,
}

impl OperationWaiter {
    fn new() -> Self {
        Self {
            count: Mutex::new(0),
            cvar: Condvar::new(),
        }
    }

    /// Increments the pending operation count, and returns a guard
    /// that decrements the count when dropped.
    fn guard(&self) -> OperationGuard<'_> {
        *self.count.lock().unwrap() += 1;
        OperationGuard(self)
    }

    /// Waits for the pending operation count to reach zero.
    fn wait(&self) {
        let mut count = self.count.lock().unwrap();
        while *count > 0 {
            count = self.cvar.wait(count).unwrap();
        }
    }
}

struct OperationGuard<'a>(&'a OperationWaiter);

impl<'a> Drop for OperationGuard<'a> {
    fn drop(&mut self) {
        let mut count = self.0.count.lock().unwrap();
        *count -= 1;
        if *count == 0 {
            self.0.cvar.notify_all();
        }
    }
}

#[derive(thiserror::Error, Debug)]
pub enum StoreError {
    #[error("schema: {0}")]
    Schema(#[from] SchemaError),
    #[error("busy")]
    Busy,
    #[error("maintenance: {0}")]
    Maintenance(#[source] Box<dyn std::error::Error + Send + Sync + 'static>),
    #[error("closed")]
    Closed,
    #[error("corrupt")]
    Corrupt,
    #[error("sqlite: {0}")]
    Sqlite(#[from] rusqlite::Error),
    #[error("storage dir: {0}")]
    StorageDir(#[source] io::Error),
}

impl ToConnectionIncident for StoreError {
    fn to_incident(&self) -> Option<ConnectionIncident> {
        match self {
            Self::Sqlite(err) => err.to_incident(),
            _ => None,
        }
    }
}
