// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

extern crate geckoservo;

extern crate abridged_certs;
extern crate app_collator_glue;
#[cfg(feature = "cubeb-remoting")]
extern crate audioipc2_client;
#[cfg(feature = "cubeb-remoting")]
extern crate audioipc2_server;
extern crate buildid_reader_ffi;
extern crate cascade_bloom_filter;
extern crate cert_storage;
extern crate chardetng_c;
extern crate crypto_hash;
#[cfg(feature = "cubeb_pulse_rust")]
extern crate cubeb_pulse;
extern crate data_storage;
extern crate dom_fragmentdirectives;
extern crate dom_speculationrules;
extern crate encoding_glue;
extern crate gkrust_utils;
extern crate harfbuzz_glue;
extern crate http_sfv;
extern crate idna_glue;
extern crate ipdl_utils;
extern crate jsrust_shared;
#[cfg(feature = "jxl_decoder")]
extern crate jxl_decoder;
extern crate kvstore;
extern crate mozurl;
extern crate mp4parse_capi;
extern crate netwerk_helper;
extern crate nserror;
extern crate nsstring;
extern crate prefs_parser;
extern crate processtools;
extern crate signature_cache;
extern crate static_prefs;
extern crate storage;
extern crate webrender_bindings;
extern crate xpcom;

extern crate audio_thread_priority;

extern crate neqo_glue;
extern crate wgpu_bindings;

extern crate aa_stroke;
extern crate qcms;
extern crate wpf_gpu_raster;

extern crate locale_service_glue;

extern crate unic_langid;
extern crate unic_langid_ffi;

extern crate fluent_langneg;
extern crate fluent_langneg_ffi;

extern crate fluent;
extern crate fluent_ffi;

extern crate oxilangtag_ffi;
extern crate unicode_bidi_ffi;

extern crate rure;

extern crate fluent_fallback;
extern crate l10nregistry_ffi;
extern crate localization_ffi;

extern crate ipcclientcerts;
extern crate qwac_trust_anchors;
extern crate ssl_tokens_cache;
extern crate trust_anchors;

extern crate gecko_logger;
extern crate gecko_tracing;

extern crate origin_trials_ffi;

extern crate data_encoding_ffi;

extern crate binary_http;
extern crate happy_eyeballs_glue;
extern crate oblivious_http;

extern crate mime_guess_ffi;

extern crate uritemplate_glue;
extern crate urlpattern;
extern crate urlpattern_glue;

extern crate adblock;

#[cfg(feature = "libz-rs-sys")]
extern crate libz_rs_sys;

extern crate log;
use log::info;

use std::{ffi::CStr, os::raw::c_char};

use gecko_logger::GeckoLogger;

#[no_mangle]
pub extern "C" fn GkRust_Init() {
    let _ = GeckoLogger::init();
    gecko_tracing::initialize_tracing();
}

#[no_mangle]
pub extern "C" fn GkRust_Shutdown() {}

/// Used to implement `nsIDebug2::RustPanic` for testing purposes.
#[no_mangle]
pub unsafe extern "C" fn intentional_panic(message: *const c_char) {
    panic!("{}", CStr::from_ptr(message).to_string_lossy());
}

/// Used to implement `nsIDebug2::rustLog` for testing purposes.
#[no_mangle]
pub unsafe extern "C" fn debug_log(target: *const c_char, message: *const c_char) {
    info!(target: CStr::from_ptr(target).to_str().unwrap(), "{}", CStr::from_ptr(message).to_str().unwrap());
}
