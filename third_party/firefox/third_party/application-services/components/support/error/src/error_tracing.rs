/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use std::{
    collections::HashMap,
    time::{Duration, Instant, SystemTime},
};

use parking_lot::Mutex;

static GLOBALS: Mutex<Globals> = Mutex::new(Globals::new());

pub fn report_error_to_app(type_name: String, message: String) {
    let breadcrumbs = {
        let mut globals = GLOBALS.lock();
        if !globals
            .rate_limiter
            .should_send_report(&type_name, Instant::now())
        {
            return;
        }
        globals.breadcrumbs.get_breadcrumbs()
    };
    let breadcrumbs = breadcrumbs.join("\n");
    let message = truncate_message(message);
    tracing_support::error!(target: "app-services-error-reporter::error", message, type_name, breadcrumbs);
}

pub fn report_breadcrumb(message: String, module: String, line: u32, column: u32) {
    GLOBALS
        .lock()
        .breadcrumbs
        .push(format_breadcrumb_and_timestamp(&message, SystemTime::now()));
    tracing_support::info!(target: "app-services-error-reporter::breadcrumb", message, module, line, column);
}

fn format_breadcrumb_and_timestamp(message: &str, time: SystemTime) -> String {
    let timestamp = match time.duration_since(SystemTime::UNIX_EPOCH) {
        Ok(n) => n.as_secs(),
        Err(_) => 0,
    };
    format!("{message} ({timestamp})")
}

struct Globals {
    breadcrumbs: BreadcrumbRingBuffer,
    rate_limiter: RateLimiter,
}

impl Globals {
    const fn new() -> Self {
        Self {
            breadcrumbs: BreadcrumbRingBuffer::new(),
            rate_limiter: RateLimiter::new(),
        }
    }
}

/// Ring buffer implementation that we use to store the most recent 20 breadcrumbs
#[derive(Default)]
struct BreadcrumbRingBuffer {
    breadcrumbs: Vec<String>,
    pos: usize,
}

impl BreadcrumbRingBuffer {
    const MAX_ITEMS: usize = 20;

    const fn new() -> Self {
        Self {
            breadcrumbs: Vec::new(),
            pos: 0,
        }
    }

    fn push(&mut self, breadcrumb: impl Into<String>) {
        let breadcrumb = truncate_breadcrumb(breadcrumb.into());
        if self.breadcrumbs.len() < Self::MAX_ITEMS {
            self.breadcrumbs.push(breadcrumb);
        } else {
            self.breadcrumbs[self.pos] = breadcrumb;
            self.pos = (self.pos + 1) % Self::MAX_ITEMS;
        }
    }

    fn get_breadcrumbs(&self) -> Vec<String> {
        let mut breadcrumbs = Vec::from(&self.breadcrumbs[self.pos..]);
        breadcrumbs.extend(self.breadcrumbs[..self.pos].iter().map(|s| s.to_string()));
        breadcrumbs
    }
}

fn truncate_message(details: String) -> String {
    truncate_string(details, 255)
}

fn truncate_breadcrumb(breadcrumb: String) -> String {
    truncate_string(breadcrumb, 100)
}

fn truncate_string(value: String, max_len: usize) -> String {
    if value.len() <= max_len {
        return value;
    }
    let split_point = (0..=max_len)
        .rev()
        .find(|i| value.is_char_boundary(*i))
        .unwrap_or(0);
    value[0..split_point].to_string()
}

/// Rate-limits error reports per component by type to a max of 20/hr
///
/// This uses the simplest algorithm possible.  We could use something like a token bucket to allow
/// for a small burst of errors, but that doesn't seem so useful.  In that scenario, the first
/// error report is the one we want to fix.
struct RateLimiter {
    last_report: Option<HashMap<String, Instant>>,
}

impl RateLimiter {
    const INTERVAL: Duration = Duration::from_secs(180);

    const fn new() -> Self {
        Self { last_report: None }
    }

    fn should_send_report(&mut self, error_type: &str, now: Instant) -> bool {
        let component = error_type.split("-").next().unwrap();
        let last_report = self.last_report.get_or_insert_with(HashMap::default);

        if let Some(last_report) = last_report.get(component) {
            match now.checked_duration_since(*last_report) {
                Some(elapsed) if elapsed < Self::INTERVAL => {
                    return false;
                }
                // For all other cases, fall through and allow the report to be sent.
                _ => (),
            }
        }
        last_report.insert(component.to_string(), now);
        true
    }
}
