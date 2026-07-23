use crate::fmt::format::Writer;
use crate::fmt::time::FormatTime;

use std::sync::Arc;

/// Formats [local time]s and [UTC time]s with `FormatTime` implementations
/// that use the [`chrono` crate].
///
/// [local time]: [`chrono::offset::Local`]
/// [UTC time]: [`chrono::offset::Utc`]
/// [`chrono` crate]: [`chrono`]

/// Formats the current [local time] using a [formatter] from the [`chrono`] crate.
///
/// [local time]: chrono::Local::now()
/// [formatter]: chrono::format
#[cfg_attr(docsrs, doc(cfg(feature = "chrono")))]
#[derive(Debug, Clone, Eq, PartialEq, Default)]
pub struct ChronoLocal {
    format: Arc<ChronoFmtType>,
}

impl ChronoLocal {
    /// Format the time using the [`RFC 3339`] format
    /// (a subset of [`ISO 8601`]).
    ///
    /// [`RFC 3339`]: https://tools.ietf.org/html/rfc3339
    /// [`ISO 8601`]: https://en.wikipedia.org/wiki/ISO_8601
    pub fn rfc_3339() -> Self {
        Self {
            format: Arc::new(ChronoFmtType::Rfc3339),
        }
    }

    /// Format the time using the given format string.
    ///
    /// See [`chrono::format::strftime`] for details on the supported syntax.
    pub fn new(format_string: String) -> Self {
        Self {
            format: Arc::new(ChronoFmtType::Custom(format_string)),
        }
    }
}

impl FormatTime for ChronoLocal {
    fn format_time(&self, w: &mut Writer<'_>) -> alloc::fmt::Result {
        let t = chrono::Local::now();
        match self.format.as_ref() {
            ChronoFmtType::Rfc3339 => {
                use chrono::format::{Fixed, Item};
                write!(
                    w,
                    "{}",
                    t.format_with_items(core::iter::once(Item::Fixed(Fixed::RFC3339)))
                )
            }
            ChronoFmtType::Custom(fmt) => {
                write!(w, "{}", t.format(fmt))
            }
        }
    }
}

/// Formats the current [UTC time] using a [formatter] from the [`chrono`] crate.
///
/// [UTC time]: chrono::Utc::now()
/// [formatter]: chrono::format
#[cfg_attr(docsrs, doc(cfg(feature = "chrono")))]
#[derive(Debug, Clone, Eq, PartialEq, Default)]
pub struct ChronoUtc {
    format: Arc<ChronoFmtType>,
}

impl ChronoUtc {
    /// Format the time using the [`RFC 3339`] format
    /// (a subset of [`ISO 8601`]).
    ///
    /// [`RFC 3339`]: https://tools.ietf.org/html/rfc3339
    /// [`ISO 8601`]: https://en.wikipedia.org/wiki/ISO_8601
    pub fn rfc_3339() -> Self {
        Self {
            format: Arc::new(ChronoFmtType::Rfc3339),
        }
    }

    /// Format the time using the given format string.
    ///
    /// See [`chrono::format::strftime`] for details on the supported syntax.
    pub fn new(format_string: String) -> Self {
        Self {
            format: Arc::new(ChronoFmtType::Custom(format_string)),
        }
    }
}

impl FormatTime for ChronoUtc {
    fn format_time(&self, w: &mut Writer<'_>) -> alloc::fmt::Result {
        let t = chrono::Utc::now();
        match self.format.as_ref() {
            ChronoFmtType::Rfc3339 => w.write_str(&t.to_rfc3339()),
            ChronoFmtType::Custom(fmt) => w.write_str(&format!("{}", t.format(fmt))),
        }
    }
}

/// The RFC 3339 format is used by default but a custom format string
/// can be used. See [`chrono::format::strftime`]for details on
/// the supported syntax.
///
/// [`chrono::format::strftime`]: https://docs.rs/chrono/0.4.9/chrono/format/strftime/index.html
#[derive(Debug, Clone, Eq, PartialEq)]
#[derive(Default)]
enum ChronoFmtType {
    /// Format according to the RFC 3339 convention.
    #[default]
    Rfc3339,
    /// Format according to a custom format string.
    Custom(String),
}
