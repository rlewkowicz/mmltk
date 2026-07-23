/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use std::io;
use std::sync::{Arc, Mutex, Weak};
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::{Builder, JoinHandle};
use std::time::{Duration, Instant};

struct Canary {
    alive: AtomicBool,
    thread: Mutex<Option<JoinHandle<()>>>,
}

impl Canary {
    fn new() -> Self {
        Self {
            alive: AtomicBool::new(true),
            thread: Mutex::new(None),
        }
    }
}

pub struct RunLoop {
    flag: Weak<Canary>,
}

impl RunLoop {
    pub fn new<F, T>(fun: F) -> io::Result<Self>
    where
        F: FnOnce(&Fn() -> bool) -> T,
        F: Send + 'static,
    {
        Self::new_with_timeout(fun, 0 )
    }

    pub fn new_with_timeout<F, T>(fun: F, timeout_ms: u64) -> io::Result<Self>
    where
        F: FnOnce(&Fn() -> bool) -> T,
        F: Send + 'static,
    {
        let flag = Arc::new(Canary::new());
        let flag_ = flag.clone();

        let thread = Builder::new().spawn(move || {
            let timeout = Duration::from_millis(timeout_ms);
            let start = Instant::now();

            let still_alive = || {
                flag.alive.load(Ordering::Relaxed) &&
                (timeout_ms == 0 || start.elapsed() < timeout)
            };

            let _ = fun(&still_alive);
        })?;

        let mut guard = (*flag_).thread.lock().map_err(|_| {
            io::Error::new(io::ErrorKind::Other, "failed to lock")
        })?;

        *guard = Some(thread);

        Ok(Self { flag: Arc::downgrade(&flag_) })
    }

    pub fn cancel(&self) {
        if let Some(flag) = self.flag.upgrade() {
            flag.alive.store(false, Ordering::Relaxed);

            if let Ok(mut guard) = flag.thread.lock() {
                if let Some(handle) = (*guard).take() {
                    let _ = handle.join();
                }
            }
        }
    }

    pub fn alive(&self) -> bool {
        if let Some(flag) = self.flag.upgrade() {
            flag.alive.load(Ordering::Relaxed)
        } else {
            false
        }
    }
}
