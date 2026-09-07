/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


extern crate libc;

extern crate log;

extern crate nserror;
extern crate xpcom;

use std::convert::TryInto;

use nserror::{nsresult, NS_ERROR_FAILURE, NS_ERROR_NOT_AVAILABLE, NS_OK};
use xpcom::{interfaces::nsIProcessToolsService, xpcom, xpcom_method, RefPtr};


use log::error;

use nserror::{NS_ERROR_CANNOT_CONVERT_DATA, NS_ERROR_UNEXPECTED};


#[no_mangle]
pub unsafe extern "C" fn new_process_tools_service(result: *mut *const nsIProcessToolsService) {
    let service: RefPtr<ProcessToolsService> = ProcessToolsService::new();
    RefPtr::new(service.coerce::<nsIProcessToolsService>()).forget(&mut *result);
}


#[xpcom(implement(nsIProcessToolsService), atomic)]
pub struct ProcessToolsService {}

impl ProcessToolsService {
    pub fn new() -> RefPtr<ProcessToolsService> {
        ProcessToolsService::allocate(InitProcessToolsService {})
    }


    xpcom_method!(
        kill => Kill(id: u64)
    );


    fn do_kill(&self, pid: u64, signal: i32) -> Result<(), nsresult> {
        let pid = pid.try_into().or(Err(NS_ERROR_CANNOT_CONVERT_DATA))?;
        let result = unsafe { libc::kill(pid, signal) };
        if result == 0 {
            Ok(())
        } else {
            match std::io::Error::last_os_error().raw_os_error() {
                Some(libc::ESRCH) => Err(NS_ERROR_NOT_AVAILABLE),
                Some(errno_value) => {
                    error!("kill({}) failed: errno={}", pid, errno_value);
                    Err(NS_ERROR_FAILURE)
                }
                None => Err(NS_ERROR_UNEXPECTED),
            }
        }
    }


    pub fn kill(&self, pid: u64) -> Result<(), nsresult> {
        self.do_kill(pid, libc::SIGKILL)
    }


    xpcom_method!(
        crash => Crash(id: u64)
    );


    pub fn crash(&self, pid: u64) -> Result<(), nsresult> {
        self.do_kill(pid, libc::SIGABRT)
    }


    xpcom_method!(
        get_pid => GetPid() -> u64
    );


    pub fn get_pid(&self) -> Result<u64, nsresult> {
        let pid = unsafe { libc::getpid() } as u64;
        Ok(pid)
    }

}
