//! Run-Time Limits

use crate::{ffi, Connection, Result};
use std::ffi::c_int;

/// Run-Time limit categories, for use with [`Connection::limit`] and
/// [`Connection::set_limit`].
///
/// See the official documentation for more information:
/// - <https://www.sqlite.org/c3ref/c_limit_attached.html>
/// - <https://www.sqlite.org/limits.html>
#[derive(Copy, Clone, Debug)]
#[repr(i32)]
#[non_exhaustive]
#[expect(non_camel_case_types)]
pub enum Limit {
    /// The maximum size of any string or BLOB or table row, in bytes.
    SQLITE_LIMIT_LENGTH = ffi::SQLITE_LIMIT_LENGTH,
    /// The maximum length of an SQL statement, in bytes.
    SQLITE_LIMIT_SQL_LENGTH = ffi::SQLITE_LIMIT_SQL_LENGTH,
    /// The maximum number of columns in a table definition or in the result set
    /// of a SELECT or the maximum number of columns in an index or in an
    /// ORDER BY or GROUP BY clause.
    SQLITE_LIMIT_COLUMN = ffi::SQLITE_LIMIT_COLUMN,
    /// The maximum depth of the parse tree on any expression.
    SQLITE_LIMIT_EXPR_DEPTH = ffi::SQLITE_LIMIT_EXPR_DEPTH,
    /// The maximum number of terms in a compound SELECT statement.
    SQLITE_LIMIT_COMPOUND_SELECT = ffi::SQLITE_LIMIT_COMPOUND_SELECT,
    /// The maximum number of instructions in a virtual machine program used to
    /// implement an SQL statement.
    SQLITE_LIMIT_VDBE_OP = ffi::SQLITE_LIMIT_VDBE_OP,
    /// The maximum number of arguments on a function.
    SQLITE_LIMIT_FUNCTION_ARG = ffi::SQLITE_LIMIT_FUNCTION_ARG,
    /// The maximum number of attached databases.
    SQLITE_LIMIT_ATTACHED = ffi::SQLITE_LIMIT_ATTACHED,
    /// The maximum length of the pattern argument to the LIKE or GLOB
    /// operators.
    SQLITE_LIMIT_LIKE_PATTERN_LENGTH = ffi::SQLITE_LIMIT_LIKE_PATTERN_LENGTH,
    /// The maximum index number of any parameter in an SQL statement.
    SQLITE_LIMIT_VARIABLE_NUMBER = ffi::SQLITE_LIMIT_VARIABLE_NUMBER,
    /// The maximum depth of recursion for triggers.
    SQLITE_LIMIT_TRIGGER_DEPTH = ffi::SQLITE_LIMIT_TRIGGER_DEPTH,
    /// The maximum number of auxiliary worker threads that a single prepared
    /// statement may start.
    SQLITE_LIMIT_WORKER_THREADS = ffi::SQLITE_LIMIT_WORKER_THREADS,
    /// Only used for testing
#[cfg(any())]









    INVALID = -1,
}

impl Connection {
    /// Returns the current value of a [`Limit`].
    #[inline]
    pub fn limit(&self, limit: Limit) -> Result<i32> {
        let c = self.db.borrow();
        let rc = unsafe { ffi::sqlite3_limit(c.db(), limit as c_int, -1) };
        if rc < 0 {
            return Err(err!(ffi::SQLITE_RANGE, "{limit:?} is invalid"));
        }
        Ok(rc)
    }

    /// Changes the [`Limit`] to `new_val`, returning the prior
    /// value of the limit.
    #[inline]
    pub fn set_limit(&self, limit: Limit, new_val: i32) -> Result<i32> {
        if new_val < 0 {
            return Err(err!(ffi::SQLITE_RANGE, "{new_val} is invalid"));
        }
        let c = self.db.borrow_mut();
        let rc = unsafe { ffi::sqlite3_limit(c.db(), limit as c_int, new_val) };
        if rc < 0 {
            return Err(err!(ffi::SQLITE_RANGE, "{limit:?} is invalid"));
        }
        Ok(rc)
    }
}
