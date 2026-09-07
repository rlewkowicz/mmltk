/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

mod layer;

mod filters;

pub use filters::{
    init_for_tests, init_for_tests_with_level, init_from_env, init_from_env_with_default,
    init_from_env_with_level,
};

pub use layer::{
    register_event_sink, register_event_sink_box, simple_event_layer, unregister_event_sink,
    EventSinkId, EventSinkSpecification, EventTarget,
};
pub use tracing;


#[macro_export]
macro_rules! trace {
    (target: $target:expr, $($tt:tt)*) => {
        $crate::tracing::trace!(
        target: $target,
        tracing_support = true,
        $($tt)*)
    };
    ($($tt:tt)*) => {
        $crate::tracing::trace!(
        tracing_support = true,
        $($tt)*)
    };
}

#[macro_export]
macro_rules! debug {
    (target: $target:expr, $($tt:tt)*) => {
        $crate::tracing::debug!(
        target: $target,
        tracing_support = true,
        $($tt)*)
    };
    ($($tt:tt)*) => {
        $crate::tracing::debug!(
        tracing_support = true,
        $($tt)*)
    };
}

#[macro_export]
macro_rules! info {
    (target: $target:expr, $($tt:tt)*) => {
        $crate::tracing::info!(
        target: $target,
        tracing_support = true,
        $($tt)*)
    };
    ($($tt:tt)*) => {
        $crate::tracing::info!(
        tracing_support = true,
        $($tt)*)
    };
}

#[macro_export]
macro_rules! warn {
    (target: $target:expr, $($tt:tt)*) => {
        $crate::tracing::warn!(
        target: $target,
        tracing_support = true,
        $($tt)*)
    };
    ($($tt:tt)*) => {
        $crate::tracing::warn!(
        tracing_support = true,
        $($tt)*)
    };
}

#[macro_export]
macro_rules! error {
    (target: $target:expr, $($tt:tt)*) => {
        $crate::tracing::error!(
        target: $target,
        tracing_support = true,
        $($tt)*)
    };
    ($($tt:tt)*) => {
        $crate::tracing::error!(
        tracing_support = true,
        $($tt)*)
    };
}

pub type Level = TracingLevel;

#[derive(uniffi::Enum, Copy, Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum TracingLevel {
    Error,
    Warn,
    Info,
    Debug,
    Trace,
}

impl From<tracing::Level> for Level {
    fn from(level: tracing::Level) -> Level {
        if level == tracing::Level::ERROR {
            Level::Error
        } else if level == tracing::Level::WARN {
            Level::Warn
        } else if level == tracing::Level::INFO {
            Level::Info
        } else if level == tracing::Level::DEBUG {
            Level::Debug
        } else if level == tracing::Level::TRACE {
            Level::Trace
        } else {
            unreachable!();
        }
    }
}

impl From<Level> for tracing::Level {
    fn from(level: Level) -> Self {
        match level {
            Level::Error => tracing::Level::ERROR,
            Level::Warn => tracing::Level::WARN,
            Level::Info => tracing::Level::INFO,
            Level::Debug => tracing::Level::DEBUG,
            Level::Trace => tracing::Level::TRACE,
        }
    }
}

pub type Event = TracingEvent;

#[derive(uniffi::Record, Clone, Debug, PartialEq, Eq)]
pub struct TracingEvent {
    pub level: Level,
    pub target: String,
    pub name: String,
    pub message: String,
    pub fields: TracingJsonValue,
}

#[uniffi::export(callback_interface)]
pub trait EventSink: Send + Sync {
    fn on_event(&self, event: Event);
}

use serde_json::Value as TracingJsonValue;

uniffi::custom_type!(TracingJsonValue, String, {
    remote,
    lower: |s| s.to_string(),
    try_lift: |s| {
        Ok(serde_json::from_str(s.as_str()).unwrap())
    },
});

uniffi::setup_scaffolding!("tracing");
