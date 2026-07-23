//! Filtering for log records.
//!
//! This module contains the log filtering used by `env_logger` to match records.
//! You can use the `Filter` type in your own logger implementation to use the same
//! filter parsing and matching as `env_logger`. For more details about the format
//! for directive strings see [Enabling Logging].
//!
//! ## Using `env_logger` in your own logger
//!
//! You can use `env_logger`'s filtering functionality with your own logger.
//! Call [`Builder::parse`] to parse directives from a string when constructing
//! your logger. Call [`Filter::matches`] to check whether a record should be
//! logged based on the parsed filters when log records are received.
//!
//! ```
//! extern crate log;
//! extern crate env_logger;
//! use env_logger::filter::Filter;
//! use log::{Log, Metadata, Record};
//!
//! struct MyLogger {
//!     filter: Filter
//! }
//!
//! impl MyLogger {
//!     fn new() -> MyLogger {
//!         use env_logger::filter::Builder;
//!         let mut builder = Builder::new();
//!
//!         // Parse a directives string from an environment variable
//!         if let Ok(ref filter) = std::env::var("MY_LOG_LEVEL") {
//!            builder.parse(filter);
//!         }
//!
//!         MyLogger {
//!             filter: builder.build()
//!         }
//!     }
//! }
//!
//! impl Log for MyLogger {
//!     fn enabled(&self, metadata: &Metadata) -> bool {
//!         self.filter.enabled(metadata)
//!     }
//!
//!     fn log(&self, record: &Record) {
//!         // Check if the record is matched by the filter
//!         if self.filter.matches(record) {
//!             println!("{:?}", record);
//!         }
//!     }
//!
//!     fn flush(&self) {}
//! }
//! ```
//!
//! [Enabling Logging]: ../index.html#enabling-logging
//! [`Builder::parse`]: struct.Builder.html#method.parse
//! [`Filter::matches`]: struct.Filter.html#method.matches

use log::{Level, LevelFilter, Metadata, Record};
use std::collections::HashMap;
use std::env;
use std::fmt;
use std::mem;

#[cfg(feature = "regex")]
#[path = "regex.rs"]
mod inner;

#[cfg(not(feature = "regex"))]
#[path = "string.rs"]
mod inner;

/// A log filter.
///
/// This struct can be used to determine whether or not a log record
/// should be written to the output.
/// Use the [`Builder`] type to parse and construct a `Filter`.
///
/// [`Builder`]: struct.Builder.html
pub struct Filter {
    directives: Vec<Directive>,
    filter: Option<inner::Filter>,
}

/// A builder for a log filter.
///
/// It can be used to parse a set of directives from a string before building
/// a [`Filter`] instance.
///
/// ## Example
///
/// ```
/// # #[macro_use] extern crate log;
/// # use std::env;
/// use env_logger::filter::Builder;
///
/// let mut builder = Builder::new();
///
/// // Parse a logging filter from an environment variable.
/// if let Ok(rust_log) = env::var("RUST_LOG") {
///     builder.parse(&rust_log);
/// }
///
/// let filter = builder.build();
/// ```
///
/// [`Filter`]: struct.Filter.html
pub struct Builder {
    directives: HashMap<Option<String>, LevelFilter>,
    filter: Option<inner::Filter>,
    built: bool,
}

#[derive(Debug)]
struct Directive {
    name: Option<String>,
    level: LevelFilter,
}

impl Filter {
    /// Returns the maximum `LevelFilter` that this filter instance is
    /// configured to output.
    ///
    /// # Example
    ///
    /// ```rust
    /// use log::LevelFilter;
    /// use env_logger::filter::Builder;
    ///
    /// let mut builder = Builder::new();
    /// builder.filter(Some("module1"), LevelFilter::Info);
    /// builder.filter(Some("module2"), LevelFilter::Error);
    ///
    /// let filter = builder.build();
    /// assert_eq!(filter.filter(), LevelFilter::Info);
    /// ```
    pub fn filter(&self) -> LevelFilter {
        self.directives
            .iter()
            .map(|d| d.level)
            .max()
            .unwrap_or(LevelFilter::Off)
    }

    /// Checks if this record matches the configured filter.
    pub fn matches(&self, record: &Record) -> bool {
        if !self.enabled(record.metadata()) {
            return false;
        }

        if let Some(filter) = self.filter.as_ref() {
            if !filter.is_match(&record.args().to_string()) {
                return false;
            }
        }

        true
    }

    /// Determines if a log message with the specified metadata would be logged.
    pub fn enabled(&self, metadata: &Metadata) -> bool {
        let level = metadata.level();
        let target = metadata.target();

        enabled(&self.directives, level, target)
    }
}

impl Builder {
    /// Initializes the filter builder with defaults.
    pub fn new() -> Builder {
        Builder {
            directives: HashMap::new(),
            filter: None,
            built: false,
        }
    }

    /// Initializes the filter builder from an environment.
    pub fn from_env(env: &str) -> Builder {
        let mut builder = Builder::new();

        if let Ok(s) = env::var(env) {
            builder.parse(&s);
        }

        builder
    }

    /// Adds a directive to the filter for a specific module.
    pub fn filter_module(&mut self, module: &str, level: LevelFilter) -> &mut Self {
        self.filter(Some(module), level)
    }

    /// Adds a directive to the filter for all modules.
    pub fn filter_level(&mut self, level: LevelFilter) -> &mut Self {
        self.filter(None, level)
    }

    /// Adds a directive to the filter.
    ///
    /// The given module (if any) will log at most the specified level provided.
    /// If no module is provided then the filter will apply to all log messages.
    pub fn filter(&mut self, module: Option<&str>, level: LevelFilter) -> &mut Self {
        self.directives.insert(module.map(|s| s.to_string()), level);
        self
    }

    /// Parses the directives string.
    ///
    /// See the [Enabling Logging] section for more details.
    ///
    /// [Enabling Logging]: ../index.html#enabling-logging
    pub fn parse(&mut self, filters: &str) -> &mut Self {
        let (directives, filter) = parse_spec(filters);

        self.filter = filter;

        for directive in directives {
            self.directives.insert(directive.name, directive.level);
        }
        self
    }

    /// Build a log filter.
    pub fn build(&mut self) -> Filter {
        assert!(!self.built, "attempt to re-use consumed builder");
        self.built = true;

        let mut directives = Vec::new();
        if self.directives.is_empty() {
            directives.push(Directive {
                name: None,
                level: LevelFilter::Error,
            });
        } else {
            let directives_map = mem::take(&mut self.directives);
            directives = directives_map
                .into_iter()
                .map(|(name, level)| Directive { name, level })
                .collect();
            directives.sort_by(|a, b| {
                let alen = a.name.as_ref().map(|a| a.len()).unwrap_or(0);
                let blen = b.name.as_ref().map(|b| b.len()).unwrap_or(0);
                alen.cmp(&blen)
            });
        }

        Filter {
            directives: mem::take(&mut directives),
            filter: mem::replace(&mut self.filter, None),
        }
    }
}

impl Default for Builder {
    fn default() -> Self {
        Builder::new()
    }
}

impl fmt::Debug for Filter {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.debug_struct("Filter")
            .field("filter", &self.filter)
            .field("directives", &self.directives)
            .finish()
    }
}

impl fmt::Debug for Builder {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        if self.built {
            f.debug_struct("Filter").field("built", &true).finish()
        } else {
            f.debug_struct("Filter")
                .field("filter", &self.filter)
                .field("directives", &self.directives)
                .finish()
        }
    }
}

/// Parse a logging specification string (e.g: "crate1,crate2::mod3,crate3::x=error/foo")
/// and return a vector with log directives.
fn parse_spec(spec: &str) -> (Vec<Directive>, Option<inner::Filter>) {
    let mut dirs = Vec::new();

    let mut parts = spec.split('/');
    let mods = parts.next();
    let filter = parts.next();
    if parts.next().is_some() {
        eprintln!(
            "warning: invalid logging spec '{}', \
             ignoring it (too many '/'s)",
            spec
        );
        return (dirs, None);
    }
    if let Some(m) = mods {
        for s in m.split(',').map(|ss| ss.trim()) {
            if s.is_empty() {
                continue;
            }
            let mut parts = s.split('=');
            let (log_level, name) =
                match (parts.next(), parts.next().map(|s| s.trim()), parts.next()) {
                    (Some(part0), None, None) => {
                        match part0.parse() {
                            Ok(num) => (num, None),
                            Err(_) => (LevelFilter::max(), Some(part0)),
                        }
                    }
                    (Some(part0), Some(""), None) => (LevelFilter::max(), Some(part0)),
                    (Some(part0), Some(part1), None) => match part1.parse() {
                        Ok(num) => (num, Some(part0)),
                        _ => {
                            eprintln!(
                                "warning: invalid logging spec '{}', \
                                 ignoring it",
                                part1
                            );
                            continue;
                        }
                    },
                    _ => {
                        eprintln!(
                            "warning: invalid logging spec '{}', \
                             ignoring it",
                            s
                        );
                        continue;
                    }
                };
            dirs.push(Directive {
                name: name.map(|s| s.to_string()),
                level: log_level,
            });
        }
    }

    let filter = filter.and_then(|filter| match inner::Filter::new(filter) {
        Ok(re) => Some(re),
        Err(e) => {
            eprintln!("warning: invalid regex filter - {}", e);
            None
        }
    });

    (dirs, filter)
}

fn enabled(directives: &[Directive], level: Level, target: &str) -> bool {
    for directive in directives.iter().rev() {
        match directive.name {
            Some(ref name) if !target.starts_with(&**name) => {}
            Some(..) | None => return level <= directive.level,
        }
    }
    false
}
