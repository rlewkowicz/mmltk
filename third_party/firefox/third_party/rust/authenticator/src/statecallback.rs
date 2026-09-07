/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use std::sync::{Arc, Condvar, Mutex};

pub struct StateCallback<T> {
    callback: Arc<Mutex<Option<Box<dyn FnOnce(T) + Send>>>>,
    observer: Arc<Mutex<Option<Box<dyn FnOnce() + Send>>>>,
    condition: Arc<(Mutex<bool>, Condvar)>,
}

impl<T> StateCallback<T> {
    #[allow(clippy::mutex_atomic)]
    pub fn new(cb: Box<dyn FnOnce(T) + Send>) -> Self {
        Self {
            callback: Arc::new(Mutex::new(Some(cb))),
            observer: Arc::new(Mutex::new(None)),
            condition: Arc::new((Mutex::new(true), Condvar::new())),
        }
    }

    pub fn add_uncloneable_observer(&mut self, obs: Box<dyn FnOnce() + Send>) {
        let mut opt = self.observer.lock().unwrap();
        if opt.is_some() {
            error!("Replacing an already-set observer.")
        }
        opt.replace(obs);
    }

    pub fn call(&self, rv: T) {
        if let Some(cb) = self.callback.lock().unwrap().take() {
            cb(rv);

            if let Some(obs) = self.observer.lock().unwrap().take() {
                obs();
            }
        }

        let (lock, cvar) = &*self.condition;
        let mut pending = lock.lock().unwrap();
        *pending = false;
        cvar.notify_all();
    }

    pub fn wait(&self) {
        let (lock, cvar) = &*self.condition;
        let _useless_guard = cvar
            .wait_while(lock.lock().unwrap(), |pending| *pending)
            .unwrap();
    }
}

impl<T> Clone for StateCallback<T> {
    fn clone(&self) -> Self {
        Self {
            callback: self.callback.clone(),
            observer: Arc::new(Mutex::new(None)),
            condition: self.condition.clone(),
        }
    }
}
