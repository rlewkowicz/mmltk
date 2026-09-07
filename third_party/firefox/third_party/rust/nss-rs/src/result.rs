// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::ffi::CStr;

use crate::{
    SECStatus,
    err::{Error, PR_ErrorToName, PR_ErrorToString, PR_GetError, PR_LANGUAGE_I_DEFAULT, Res, nspr},
    nss_prelude::SECSuccess,
    ssl,
};

pub fn result(rv: ssl::SECStatus) -> Res<()> {
    _ = result_helper(rv, false)?;
    Ok(())
}

pub fn result_or_blocked(rv: SECStatus) -> Res<bool> {
    result_helper(rv, true)
}

fn wrap_str_fn<F>(f: F, dflt: &str) -> String
where
    F: FnOnce() -> *const i8,
{
    unsafe {
        let p = f();
        if p.is_null() {
            return dflt.to_string();
        }
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

fn result_helper(rv: SECStatus, allow_blocked: bool) -> Res<bool> {
    if rv == SECSuccess {
        return Ok(false);
    }

    let code = unsafe { PR_GetError() };
    if allow_blocked && code == nspr::PR_WOULD_BLOCK_ERROR {
        return Ok(true);
    }

    let name = wrap_str_fn(|| unsafe { PR_ErrorToName(code) }, "UNKNOWN_ERROR");
    let desc = wrap_str_fn(
        || unsafe { PR_ErrorToString(code, PR_LANGUAGE_I_DEFAULT) },
        "...",
    );
    Err(Error::NssError { name, code, desc })
}
