// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{
    cell::RefCell,
    fmt::{self, Display},
    fs::OpenOptions,
    io::BufWriter,
    path::PathBuf,
    rc::Rc,
    time::{Instant, SystemTime},
};

use qlog::{
    CommonFields, Configuration, TraceSeq, VantagePoint, VantagePointType, streamer::QlogStreamer,
};

use crate::Role;

#[derive(Debug, Clone, Default)]
pub struct Qlog {
    /// Both the inner and the outer `Option` are set to `None`
    /// on failure. The inner `None` will disable qlog for all other
    /// references (correctness). The outer `None` will prevent
    /// the local instance from de-referencing the `Rc` again
    /// (performance).
    inner: Option<Rc<RefCell<Option<SharedStreamer>>>>,
}

pub struct SharedStreamer {
    qlog_path: PathBuf,
    streamer: QlogStreamer,
}

impl Qlog {
    /// Create an enabled `Qlog` configuration backed by a file.
    ///
    /// # Errors
    ///
    /// Will return `qlog::Error` if it cannot write to the new file.
    pub fn enabled_with_file<D: Display>(
        mut qlog_path: PathBuf,
        role: Role,
        title: Option<String>,
        description: Option<String>,
        file_prefix: D,
        now: Instant,
    ) -> Result<Self, qlog::Error> {
        qlog_path.push(format!("{file_prefix}.sqlog"));

        let file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&qlog_path)?;

        let streamer = QlogStreamer::new(
            qlog::QLOG_VERSION.to_string(),
            title,
            description,
            None,
            now,
            new_trace(role),
            qlog::events::EventImportance::Extra,
            Box::new(BufWriter::new(file)),
        );
        Self::enabled(streamer, qlog_path)
    }

    /// Create an enabled `Qlog` configuration.
    ///
    /// This needs to be called before the connection is used, because otherwise `Qlog`-logging will
    /// remain disabled (for performance reasons).
    ///
    /// # Errors
    ///
    /// Will return `qlog::Error` if it cannot write to the new log.
    pub fn enabled(mut streamer: QlogStreamer, qlog_path: PathBuf) -> Result<Self, qlog::Error> {
        streamer.start_log()?;

        Ok(Self {
            inner: Some(Rc::new(RefCell::new(Some(SharedStreamer {
                qlog_path,
                streamer,
            })))),
        })
    }

    /// Create a disabled `Qlog` configuration.
    #[must_use]
    pub fn disabled() -> Self {
        Self::default()
    }

    /// Returns true if qlog is enabled.
    #[must_use]
    pub fn is_enabled(&self) -> bool {
        self.inner.as_ref().is_some_and(|rc| rc.borrow().is_some())
    }

    /// If logging enabled, closure may generate an event to be logged.
    pub fn add_event_at<F>(&mut self, f: F, now: Instant)
    where
        F: FnOnce() -> Option<qlog::events::EventData>,
    {
        self.add_event_with_stream(|s| {
            if let Some(ev_data) = f() {
                s.add_event_data_with_instant(ev_data, now)?;
            }
            Ok(())
        });
    }

    /// If logging enabled, closure is given the Qlog stream to write events and
    /// frames to.
    pub fn add_event_with_stream<F>(&mut self, f: F)
    where
        F: FnOnce(&mut QlogStreamer) -> Result<(), qlog::Error>,
    {
        let Some(inner) = self.inner.as_mut() else {
            return;
        };

        let mut borrow = inner.borrow_mut();

        let Some(shared_streamer) = borrow.as_mut() else {
            drop(borrow);
            self.inner = None;
            return;
        };

        match f(&mut shared_streamer.streamer) {
            Ok(()) | Err(qlog::Error::Done) => (),
            Err(e) => {
                log::error!("Qlog event generation failed with error {e}; closing qlog.");
                *borrow = None;
                drop(borrow);
                self.inner = None;
            }
        }
    }
}

impl fmt::Debug for SharedStreamer {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "Qlog writing to {}", self.qlog_path.display())
    }
}

impl Drop for SharedStreamer {
    fn drop(&mut self) {
        if let Err(e) = self.streamer.finish_log() {
            log::error!("Error dropping Qlog: {e}");
        }
    }
}

#[must_use]
pub fn new_trace(role: Role) -> TraceSeq {
    TraceSeq {
        vantage_point: VantagePoint {
            name: Some(format!("neqo-{role}")),
            ty: match role {
                Role::Client => VantagePointType::Client,
                Role::Server => VantagePointType::Server,
            },
            flow: None,
        },
        title: Some(format!("neqo-{role} trace")),
        description: Some(format!("neqo-{role} trace")),
        configuration: Some(Configuration {
            time_offset: Some(0.0),
            original_uris: None,
        }),
        common_fields: Some(CommonFields {
            group_id: None,
            protocol_type: None,
            reference_time: SystemTime::now()
                .duration_since(SystemTime::UNIX_EPOCH)
                .map(|d| d.as_secs_f64() * 1_000.0)
                .ok(),
            time_format: Some("relative".to_string()),
        }),
    }
}
