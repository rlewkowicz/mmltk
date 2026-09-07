//! Prepared statements cache for faster execution.

use crate::raw_statement::RawStatement;
use crate::{Connection, PrepFlags, Result, Statement};
use hashlink::LruCache;
use std::cell::RefCell;
use std::ops::{Deref, DerefMut};
use std::sync::Arc;

impl Connection {
    /// Prepare a SQL statement for execution, returning a previously prepared
    /// (but not currently in-use) statement if one is available. The
    /// returned statement will be cached for reuse by future calls to
    /// [`prepare_cached`](Connection::prepare_cached) once it is dropped.
    ///
    /// ```rust,no_run
    /// # use rusqlite::{Connection, Result};
    /// fn insert_new_people(conn: &Connection) -> Result<()> {
    ///     {
    ///         let mut stmt = conn.prepare_cached("INSERT INTO People (name) VALUES (?1)")?;
    ///         stmt.execute(["Joe Smith"])?;
    ///     }
    ///     {
    ///         // This will return the same underlying SQLite statement handle without
    ///         // having to prepare it again.
    ///         let mut stmt = conn.prepare_cached("INSERT INTO People (name) VALUES (?1)")?;
    ///         stmt.execute(["Bob Jones"])?;
    ///     }
    ///     Ok(())
    /// }
    /// ```
    ///
    /// # Failure
    ///
    /// Will return `Err` if `sql` cannot be converted to a C-compatible string
    /// or if the underlying SQLite call fails.
    #[inline]
    pub fn prepare_cached(&self, sql: &str) -> Result<CachedStatement<'_>> {
        self.cache.get(self, sql)
    }

    /// Set the maximum number of cached prepared statements this connection
    /// will hold. By default, a connection will hold a relatively small
    /// number of cached statements. If you need more, or know that you
    /// will not use cached statements, you
    /// can set the capacity manually using this method.
    #[inline]
    pub fn set_prepared_statement_cache_capacity(&self, capacity: usize) {
        self.cache.set_capacity(capacity);
    }

    /// Remove/finalize all prepared statements currently in the cache.
    #[inline]
    pub fn flush_prepared_statement_cache(&self) {
        self.cache.flush();
    }
}

/// Prepared statements LRU cache.
#[derive(Debug)]
pub struct StatementCache(RefCell<LruCache<Arc<str>, RawStatement>>);

unsafe impl Send for StatementCache {}

/// Cacheable statement.
///
/// Statement will return automatically to the cache by default.
/// If you want the statement to be discarded, call
/// [`discard()`](CachedStatement::discard) on it.
pub struct CachedStatement<'conn> {
    stmt: Option<Statement<'conn>>,
    cache: &'conn StatementCache,
}

impl<'conn> Deref for CachedStatement<'conn> {
    type Target = Statement<'conn>;

    #[inline]
    fn deref(&self) -> &Statement<'conn> {
        self.stmt.as_ref().unwrap()
    }
}

impl<'conn> DerefMut for CachedStatement<'conn> {
    #[inline]
    fn deref_mut(&mut self) -> &mut Statement<'conn> {
        self.stmt.as_mut().unwrap()
    }
}

impl Drop for CachedStatement<'_> {
    #[inline]
    fn drop(&mut self) {
        if let Some(stmt) = self.stmt.take() {
            self.cache.cache_stmt(unsafe { stmt.into_raw() });
        }
    }
}

impl CachedStatement<'_> {
    #[inline]
    fn new<'conn>(stmt: Statement<'conn>, cache: &'conn StatementCache) -> CachedStatement<'conn> {
        CachedStatement {
            stmt: Some(stmt),
            cache,
        }
    }

    /// Discard the statement, preventing it from being returned to its
    /// [`Connection`]'s collection of cached statements.
    #[inline]
    pub fn discard(mut self) {
        self.stmt = None;
    }
}

impl StatementCache {
    /// Create a statement cache.
    #[inline]
    pub fn with_capacity(capacity: usize) -> Self {
        Self(RefCell::new(LruCache::new(capacity)))
    }

    #[inline]
    fn set_capacity(&self, capacity: usize) {
        self.0.borrow_mut().set_capacity(capacity);
    }

    fn get<'conn>(
        &'conn self,
        conn: &'conn Connection,
        sql: &str,
    ) -> Result<CachedStatement<'conn>> {
        let trimmed = sql.trim();
        let mut cache = self.0.borrow_mut();
        let stmt = match cache.remove(trimmed) {
            Some(raw_stmt) => Ok(Statement::new(conn, raw_stmt)),
            None => conn.prepare_with_flags(trimmed, PrepFlags::SQLITE_PREPARE_PERSISTENT),
        };
        stmt.map(|mut stmt| {
            stmt.stmt.set_statement_cache_key(trimmed);
            CachedStatement::new(stmt, self)
        })
    }

    fn cache_stmt(&self, mut stmt: RawStatement) {
        if stmt.is_null() {
            return;
        }
        let mut cache = self.0.borrow_mut();
        stmt.clear_bindings();
        if let Some(sql) = stmt.statement_cache_key() {
            cache.insert(sql, stmt);
        } else {
            debug_assert!(
                false,
                "bug in statement cache code, statement returned to cache that without key"
            );
        }
    }

    #[inline]
    fn flush(&self) {
        let mut cache = self.0.borrow_mut();
        cache.clear();
    }
}
