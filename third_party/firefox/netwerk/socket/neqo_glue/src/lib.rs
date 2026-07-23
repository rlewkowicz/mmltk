/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#![expect(clippy::missing_panics_doc, reason = "OK here")]

use std::{
    borrow::Cow,
    cell::RefCell,
    cmp::min,
    ffi::c_void,
    io,
    net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr},
    path::PathBuf,
    ptr,
    rc::Rc,
    slice, str,
    time::{Duration, Instant},
};


use libc::{c_int, AF_INET, AF_INET6};
use libc::{c_uchar, size_t};
use log::debug;
use neqo_common::{
    datagram, event::Provider as _, qdebug, qerror, qlog::Qlog, qwarn, Datagram, Decoder, Encoder,
    Header, Role, Tos,
};
use neqo_http3::{
    features::extended_connect::session, ConnectUdpEvent, Error as Http3Error, Http3Client,
    Http3ClientEvent, Http3Parameters, Http3State, Priority, WebTransportEvent,
};
use neqo_transport::{
    stream_id::StreamType, CongestionControl, Connection, ConnectionParameters,
    Error as TransportError, HyStartCssBaseline, Output, OutputBatch, RandomConnectionIdGenerator,
    SlowStart, StreamId, Version,
};
use nserror::{
    nsresult, NS_BASE_STREAM_WOULD_BLOCK, NS_ERROR_CONNECTION_REFUSED,
    NS_ERROR_DOM_INVALID_HEADER_NAME, NS_ERROR_FILE_ALREADY_EXISTS, NS_ERROR_ILLEGAL_VALUE,
    NS_ERROR_INVALID_ARG, NS_ERROR_NET_HTTP3_PROTOCOL_ERROR, NS_ERROR_NET_INTERRUPT,
    NS_ERROR_NET_RESET, NS_ERROR_NET_TIMEOUT, NS_ERROR_NOT_AVAILABLE, NS_ERROR_NOT_CONNECTED,
    NS_ERROR_OUT_OF_MEMORY, NS_ERROR_SOCKET_ADDRESS_IN_USE, NS_ERROR_UNEXPECTED, NS_OK,
};
use nss_rs::{agent::CertificateCompressor, init, PRErrorCode};
use nsstring::{nsACString, nsCString};
use thin_vec::ThinVec;
use uuid::Uuid;
use xpcom::{AtomicRefcnt, RefCounted, RefPtr};
use zlib_rs::{decompress_slice, InflateConfig, ReturnCode};

std::thread_local! {
    static RECV_BUF: RefCell<neqo_udp::RecvBuf> = RefCell::new(neqo_udp::RecvBuf::default());
}



#[allow(clippy::cast_possible_truncation, reason = "see check below")]
const AF_INET_U16: u16 = AF_INET as u16;
static_assertions::const_assert_eq!(AF_INET_U16 as c_int, AF_INET);

#[allow(clippy::cast_possible_truncation, reason = "see check below")]
const AF_INET6_U16: u16 = AF_INET6 as u16;
static_assertions::const_assert_eq!(AF_INET6_U16 as c_int, AF_INET6);

#[repr(C)]
pub struct WouldBlockCounter {
    rx: usize,
    tx: usize,
}

impl WouldBlockCounter {
    pub fn new() -> Self {
        Self { rx: 0, tx: 0 }
    }

    pub fn increment_rx(&mut self) {
        self.rx += 1;
    }

    pub fn increment_tx(&mut self) {
        self.tx += 1;
    }

    pub fn rx_count(&self) -> usize {
        self.rx
    }

    pub fn tx_count(&self) -> usize {
        self.tx
    }
}

#[repr(C)]
pub struct NeqoHttp3Conn {
    conn: Http3Client,
    local_addr: SocketAddr,
    refcnt: AtomicRefcnt,
    /// Socket to use for IO.
    ///
    /// When [`None`], NSPR is used for IO.
    socket: Option<neqo_udp::Socket<BorrowedSocket>>,
    /// Buffered outbound datagram from previous send that failed with
    /// WouldBlock. To be sent once UDP socket has write-availability again.
    buffered_outbound_datagram: Option<datagram::Batch>,

    would_block_counter: WouldBlockCounter,
}

#[repr(C)]
pub union NetAddr {
    private: [u8; 0],
}

extern "C" {
    pub fn moz_netaddr_get_family(arg: *const NetAddr) -> u16;
    pub fn moz_netaddr_get_network_order_ip(arg: *const NetAddr) -> u32;
    pub fn moz_netaddr_get_ipv6(arg: *const NetAddr) -> *const u8;
    pub fn moz_netaddr_get_network_order_port(arg: *const NetAddr) -> u16;
}

fn netaddr_to_socket_addr(arg: *const NetAddr) -> Result<SocketAddr, nsresult> {
    if arg.is_null() {
        return Err(NS_ERROR_INVALID_ARG);
    }

    unsafe {
        let family = i32::from(moz_netaddr_get_family(arg));
        if family == AF_INET {
            let port = u16::from_be(moz_netaddr_get_network_order_port(arg));
            let ipv4 = Ipv4Addr::from(u32::from_be(moz_netaddr_get_network_order_ip(arg)));
            return Ok(SocketAddr::new(IpAddr::V4(ipv4), port));
        }

        if family == AF_INET6 {
            let port = u16::from_be(moz_netaddr_get_network_order_port(arg));
            let ipv6_slice: [u8; 16] = slice::from_raw_parts(moz_netaddr_get_ipv6(arg), 16)
                .try_into()
                .expect("slice with incorrect length");
            let ipv6 = Ipv6Addr::from(ipv6_slice);
            return Ok(SocketAddr::new(IpAddr::V6(ipv6), port));
        }
    }

    Err(NS_ERROR_UNEXPECTED)
}

fn enable_zlib_decoder(c: &mut Connection) -> neqo_transport::Res<()> {
    struct ZlibCertDecoder {}

    impl CertificateCompressor for ZlibCertDecoder {
        const ID: u16 = 0x1;
        const NAME: &std::ffi::CStr = c"zlib";

        fn decode(input: &[u8], output: &mut [u8]) -> nss_rs::Res<()> {
            let (output_slice, error) = decompress_slice(output, &input, InflateConfig::default());
            if error != ReturnCode::Ok {
                return Err(nss_rs::Error::CertificateDecoding);
            }
            if output_slice.len() != output.len() {
                return Err(nss_rs::Error::CertificateDecoding);
            }

            Ok(())
        }
    }

    c.set_certificate_compression::<ZlibCertDecoder>()
}

extern "C" {
    pub fn ZSTD_decompress(
        dst: *mut ::core::ffi::c_void,
        dstCapacity: usize,
        src: *const ::core::ffi::c_void,
        compressedSize: usize,
    ) -> usize;
}

extern "C" {
    pub fn ZSTD_isError(result: usize) -> ::core::ffi::c_uint;
}

fn enable_zstd_decoder(c: &mut Connection) -> neqo_transport::Res<()> {
    struct ZstdCertDecoder {}

    impl CertificateCompressor for ZstdCertDecoder {
        const ID: u16 = 0x3;
        const NAME: &std::ffi::CStr = c"zstd";

        fn decode(input: &[u8], output: &mut [u8]) -> nss_rs::Res<()> {
            if input.is_empty() {
                return Err(nss_rs::Error::CertificateDecoding);
            }
            if output.is_empty() {
                return Err(nss_rs::Error::CertificateDecoding);
            }

            let output_len = unsafe {
                ZSTD_decompress(
                    output.as_mut_ptr() as *mut c_void,
                    output.len(),
                    input.as_ptr() as *const c_void,
                    input.len(),
                )
            };

            if unsafe { ZSTD_isError(output_len) != 0 } {
                qdebug!("zstd compression failed with {output_len}");
                return Err(nss_rs::Error::CertificateDecoding);
            }

            if output.len() != output_len {
                qdebug!("zstd compression `output_len` {output_len} doesn't match expected `output.len()` {}", output.len());
                return Err(nss_rs::Error::CertificateDecoding);
            }

            Ok(())
        }
    }

    c.set_certificate_compression::<ZstdCertDecoder>()
}

#[repr(C)]
#[derive(Debug, PartialEq)]
pub enum BrotliDecoderResult {
    Error = 0,
    Success = 1,
    NeedsMoreInput = 2,
    NeedsMoreOutput = 3,
}

extern "C" {
    pub fn BrotliDecoderDecompress(
        encoded_size: size_t,
        encoded_buffer: *const c_uchar,
        decoded_size: *mut size_t,
        decoded_buffer: *mut c_uchar,
    ) -> BrotliDecoderResult;
}

fn enable_brotli_decoder(c: &mut Connection) -> neqo_transport::Res<()> {
    struct BrotliCertDecoder {}

    impl CertificateCompressor for BrotliCertDecoder {
        const ID: u16 = 0x2;
        const NAME: &std::ffi::CStr = c"brotli";

        fn decode(input: &[u8], output: &mut [u8]) -> nss_rs::Res<()> {
            if input.is_empty() {
                return Err(nss_rs::Error::CertificateDecoding);
            }
            if output.is_empty() {
                return Err(nss_rs::Error::CertificateDecoding);
            }

            let mut uncompressed_size = output.len();
            let result = unsafe {
                BrotliDecoderDecompress(
                    input.len(),
                    input.as_ptr(),
                    &mut uncompressed_size as *mut usize,
                    output.as_mut_ptr(),
                )
            };

            if result != BrotliDecoderResult::Success {
                return Err(nss_rs::Error::CertificateDecoding);
            }

            if uncompressed_size != output.len() {
                return Err(nss_rs::Error::CertificateDecoding);
            }

            Ok(())
        }
    }

    c.set_certificate_compression::<BrotliCertDecoder>()
}

type SendFunc = extern "C" fn(
    context: *mut c_void,
    addr_family: u16,
    addr: *const u8,
    port: u16,
    data: *const u8,
    size: u32,
) -> nsresult;

type SetTimerFunc = extern "C" fn(context: *mut c_void, timeout: u64);


type BorrowedSocket = std::os::fd::BorrowedFd<'static>;

impl NeqoHttp3Conn {
    /// Create a new [`NeqoHttp3Conn`].
    ///
    /// Note that [`NeqoHttp3Conn`] works under the assumption that the UDP
    /// socket of the connection, i.e. the one provided to
    /// [`NeqoHttp3Conn::new`], does not change throughout the lifetime of
    /// [`NeqoHttp3Conn`].
    #[expect(
        clippy::too_many_arguments,
        clippy::too_many_lines,
        reason = "Nothing to be done about it."
    )]
    fn new(
        origin: &nsACString,
        alpn: &nsACString,
        local_addr: *const NetAddr,
        remote_addr: *const NetAddr,
        max_table_size: u64,
        max_blocked_streams: u16,
        max_data: u64,
        max_stream_data: u64,
        version_negotiation: bool,
        webtransport: bool,
        qlog_dir: &nsACString,
        idle_timeout: u32,
        fast_pto: u32,
        pmtud_enabled: bool,
        socket: Option<i64>,
    ) -> Result<RefPtr<Self>, nsresult> {
        init().map_err(|_| NS_ERROR_UNEXPECTED)?;

        let socket = socket
            .map(|socket| {

                let borrowed = {
                    use std::os::fd::{BorrowedFd, RawFd};
                    if socket == -1 {
                        qerror!("got invalid socked {}", socket);
                        return Err(NS_ERROR_INVALID_ARG);
                    }
                    let raw: RawFd = socket.try_into().map_err(|e| {
                        qerror!("got invalid socked {}: {}", socket, e);
                        NS_ERROR_INVALID_ARG
                    })?;
                    unsafe { BorrowedFd::borrow_raw(raw) }
                };
                let s = neqo_udp::Socket::new(borrowed).map_err(|e| {
                    qerror!("failed to initialize socket {}: {}", socket, e);
                    into_nsresult(&e)
                })?;
Ok(s)
            })
            .transpose()?;

        let origin_conv = str::from_utf8(origin).map_err(|_| NS_ERROR_INVALID_ARG)?;

        let alpn_conv = str::from_utf8(alpn).map_err(|_| NS_ERROR_INVALID_ARG)?;

        let local: SocketAddr = netaddr_to_socket_addr(local_addr)?;

        let remote: SocketAddr = netaddr_to_socket_addr(remote_addr)?;

        let quic_version = match alpn_conv {
            "h3" => Version::Version1,
            _ => return Err(NS_ERROR_INVALID_ARG),
        };

        let version_list = if version_negotiation {
            Version::all()
        } else {
            vec![quic_version]
        };

        let cc_algorithm = match static_prefs::pref!("network.http.http3.cc_algorithm") {
            0 => CongestionControl::NewReno,
            1 => CongestionControl::Cubic,
            _ => {
                CongestionControl::Cubic
            }
        };

        let slow_start = match static_prefs::pref!("network.http.http3.slow_start_algorithm") {
            0 => SlowStart::Classic,
            1 => SlowStart::HyStart,
            2 => SlowStart::Search,
            _ => {
                debug!("Unknown http3.slow_start_algorithm pref, defaulting to SlowStart::Classic");
                SlowStart::Classic
            }
        };

        let pmtud_enabled =
            pmtud_enabled
            || static_prefs::pref!("network.http.http3.pmtud")
            && socket.as_ref().map_or(false, |s| !s.may_fragment());

        let spurious_recovery = static_prefs::pref!("network.http.http3.spurious_recovery");

        let css_baseline =
            if static_prefs::pref!("network.http.http3.hystart_alternative_css_baseline") {
                HyStartCssBaseline::EntryThreshold
            } else {
                HyStartCssBaseline::CurrentRoundMinRtt
            };

        let mut params = ConnectionParameters::default()
            .versions(quic_version, version_list)
            .congestion_control(cc_algorithm)
            .slow_start(slow_start)
            .max_data(max_data)
            .max_stream_data(StreamType::BiDi, false, max_stream_data)
            .grease(static_prefs::pref!("security.tls.grease_http3_enable"))
            .sni_slicing(static_prefs::pref!("network.http.http3.sni-slicing"))
            .idle_timeout(Duration::from_secs(idle_timeout.into()))
            .pmtud_iface_mtu(cfg!(not(target_os = "openbsd")))
            .mlkem(false)
            .pmtud(pmtud_enabled)
            .spurious_recovery(spurious_recovery)
            .hystart_css_baseline(css_baseline);

        if fast_pto > 0 {
            if let Ok(v) = u8::try_from(fast_pto) {
                params = params.fast_pto(v);
            } else {
                debug_assert!(false, "fast_pto value {fast_pto} exceeds u8::MAX");
            }
        }

        let http3_settings = Http3Parameters::default()
            .max_table_size_encoder(max_table_size)
            .max_table_size_decoder(max_table_size)
            .max_blocked_streams(max_blocked_streams)
            .max_concurrent_push_streams(0)
            .connection_parameters(params)
            .webtransport(webtransport)
            .connect(true)
            .http3_datagram(true);

        let Ok(mut conn) = Connection::new_client(
            origin_conv,
            &[alpn_conv],
            Rc::new(RefCell::new(RandomConnectionIdGenerator::new(3))),
            local,
            remote,
            http3_settings.get_connection_parameters().clone(),
            Instant::now(),
        ) else {
            return Err(NS_ERROR_INVALID_ARG);
        };

        let mut additional_shares = usize::from(static_prefs::pref!(
            "security.tls.client_hello.send_p256_keyshare"
        ));
        if static_prefs::pref!("security.tls.enable_kyber")
            && static_prefs::pref!("network.http.http3.enable_kyber")
        {
            conn.set_groups(&[
                nss_rs::TLS_GRP_KEM_MLKEM768X25519,
                nss_rs::TLS_GRP_EC_X25519,
                nss_rs::TLS_GRP_EC_SECP256R1,
                nss_rs::TLS_GRP_EC_SECP384R1,
                nss_rs::TLS_GRP_EC_SECP521R1,
            ])
            .map_err(|_| NS_ERROR_UNEXPECTED)?;
            additional_shares += 1;
        }
        conn.send_additional_key_shares(additional_shares)
            .map_err(|_| NS_ERROR_UNEXPECTED)?;

        if static_prefs::pref!("security.tls.enable_certificate_compression_zlib")
            && static_prefs::pref!("network.http.http3.enable_certificate_compression_zlib")
        {
            enable_zlib_decoder(&mut conn).map_err(|_| NS_ERROR_UNEXPECTED)?;
        }

        if static_prefs::pref!("security.tls.enable_certificate_compression_zstd")
            && static_prefs::pref!("network.http.http3.enable_certificate_compression_zstd")
        {
            enable_zstd_decoder(&mut conn).map_err(|_| NS_ERROR_UNEXPECTED)?;
        }

        if static_prefs::pref!("security.tls.enable_certificate_compression_brotli")
            && static_prefs::pref!("network.http.http3.enable_certificate_compression_brotli")
        {
            enable_brotli_decoder(&mut conn).map_err(|_| NS_ERROR_UNEXPECTED)?;
        }

        let mut conn = Http3Client::new_with_conn(conn, http3_settings);

        if !qlog_dir.is_empty() {
            let qlog_dir_conv = str::from_utf8(qlog_dir).map_err(|_| NS_ERROR_INVALID_ARG)?;
            let qlog_path = PathBuf::from(qlog_dir_conv);

            match Qlog::enabled_with_file(
                qlog_path.clone(),
                Role::Client,
                Some("Firefox Client qlog".to_string()),
                Some("Firefox Client qlog".to_string()),
                format!("{}_{}.qlog", origin, Uuid::new_v4()),
                Instant::now(),
            ) {
                Ok(qlog) => conn.set_qlog(qlog),
                Err(e) => {
                    qwarn!("failed to create Qlog at {}: {}", qlog_path.display(), e);
                }
            }
        }

        let conn = Box::into_raw(Box::new(Self {
            conn,
            local_addr: local,
            refcnt: unsafe { AtomicRefcnt::new() },
            socket,
            buffered_outbound_datagram: None,
            would_block_counter: WouldBlockCounter::new(),
        }));
        unsafe { RefPtr::from_raw(conn).ok_or(NS_ERROR_NOT_CONNECTED) }
    }

    fn increment_would_block_rx(&mut self) {
        self.would_block_counter.increment_rx();
    }

    fn would_block_rx_count(&self) -> usize {
        self.would_block_counter.rx_count()
    }

    fn increment_would_block_tx(&mut self) {
        self.would_block_counter.increment_tx();
    }

    fn would_block_tx_count(&self) -> usize {
        self.would_block_counter.tx_count()
    }
}

/// # Safety
///
/// See [`AtomicRefcnt::inc`].
#[no_mangle]
pub unsafe extern "C" fn neqo_http3conn_addref(conn: &NeqoHttp3Conn) {
    conn.refcnt.inc();
}

/// # Safety
///
/// Manually drops a pointer without consuming pointee. The caller needs to
/// ensure no other referenecs remain. In addition safety conditions of
/// [`AtomicRefcnt::dec`] apply.
#[no_mangle]
pub unsafe extern "C" fn neqo_http3conn_release(conn: &NeqoHttp3Conn) {
    let rc = conn.refcnt.dec();
    if rc == 0 {
        drop(Box::from_raw(ptr::from_ref(conn).cast_mut()));
    }
}

unsafe impl RefCounted for NeqoHttp3Conn {
    unsafe fn addref(&self) {
        neqo_http3conn_addref(self);
    }
    unsafe fn release(&self) {
        neqo_http3conn_release(self);
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_new(
    origin: &nsACString,
    alpn: &nsACString,
    local_addr: *const NetAddr,
    remote_addr: *const NetAddr,
    max_table_size: u64,
    max_blocked_streams: u16,
    max_data: u64,
    max_stream_data: u64,
    version_negotiation: bool,
    webtransport: bool,
    qlog_dir: &nsACString,
    idle_timeout: u32,
    fast_pto: u32,
    socket: i64,
    pmtud_enabled: bool,
    result: &mut *const NeqoHttp3Conn,
) -> nsresult {
    *result = ptr::null_mut();

    match NeqoHttp3Conn::new(
        origin,
        alpn,
        local_addr,
        remote_addr,
        max_table_size,
        max_blocked_streams,
        max_data,
        max_stream_data,
        version_negotiation,
        webtransport,
        qlog_dir,
        idle_timeout,
        fast_pto,
        pmtud_enabled,
        Some(socket),
    ) {
        Ok(http3_conn) => {
            http3_conn.forget(result);
            NS_OK
        }
        Err(e) => e,
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_new_use_nspr_for_io(
    origin: &nsACString,
    alpn: &nsACString,
    local_addr: *const NetAddr,
    remote_addr: *const NetAddr,
    max_table_size: u64,
    max_blocked_streams: u16,
    max_data: u64,
    max_stream_data: u64,
    version_negotiation: bool,
    webtransport: bool,
    qlog_dir: &nsACString,
    idle_timeout: u32,
    fast_pto: u32,
    result: &mut *const NeqoHttp3Conn,
) -> nsresult {
    *result = ptr::null_mut();

    match NeqoHttp3Conn::new(
        origin,
        alpn,
        local_addr,
        remote_addr,
        max_table_size,
        max_blocked_streams,
        max_data,
        max_stream_data,
        version_negotiation,
        webtransport,
        qlog_dir,
        idle_timeout,
        fast_pto,
        false,
        None,
    ) {
        Ok(http3_conn) => {
            http3_conn.forget(result);
            NS_OK
        }
        Err(e) => e,
    }
}

/// Process a packet.
/// packet holds packet data.
///
/// # Safety
///
/// Use of raw (i.e. unsafe) pointers as arguments.
#[no_mangle]
pub unsafe extern "C" fn neqo_http3conn_process_input_use_nspr_for_io(
    conn: &mut NeqoHttp3Conn,
    remote_addr: *const NetAddr,
    packet: *const ThinVec<u8>,
) -> nsresult {
    assert!(conn.socket.is_none(), "NSPR IO path");

    let remote = match netaddr_to_socket_addr(remote_addr) {
        Ok(addr) => addr,
        Err(result) => return result,
    };
    let d = Datagram::new(
        remote,
        conn.local_addr,
        Tos::default(),
        (*packet).as_slice(),
    );
    conn.conn.process_input(d, Instant::now());
    NS_OK
}

#[repr(C)]
pub struct ProcessInputResult {
    pub result: nsresult,
    pub bytes_read: u32,
}

/// Process input, reading incoming datagrams from the socket and passing them
/// to the Neqo state machine.
///
/// # Safety
///
/// Marked as unsafe given exposition via FFI i.e. `extern "C"`.
#[no_mangle]
pub unsafe extern "C" fn neqo_http3conn_process_input(
    conn: &mut NeqoHttp3Conn,
) -> ProcessInputResult {
    let mut bytes_read = 0;

    RECV_BUF.with_borrow_mut(|recv_buf| {
        loop {
            let dgrams = match conn
                .socket
                .as_mut()
                .expect("non NSPR IO")
                .recv(conn.local_addr, recv_buf)
            {
                Ok(dgrams) => dgrams,
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                    conn.increment_would_block_rx();
                    break;
                }
                Err(e) => {
                    qwarn!("failed to receive datagrams: {}", e);
                    return ProcessInputResult {
                        result: into_nsresult(&e),
                        bytes_read: 0,
                    };
                }
            };

            let ecn_enabled = static_prefs::pref!("network.http.http3.ecn_report");
            let dgrams = dgrams.map(|mut d| {
                bytes_read += d.len();
                if !ecn_enabled {
                    d.set_tos(Tos::default());
                }
                d
            });

            conn.conn.process_multiple_input(dgrams, Instant::now());

        }

        ProcessInputResult {
            result: NS_OK,
            bytes_read: bytes_read.try_into().unwrap_or(u32::MAX),
        }
    })
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_process_output_and_send_use_nspr_for_io(
    conn: &mut NeqoHttp3Conn,
    context: *mut c_void,
    send_func: SendFunc,
    set_timer_func: SetTimerFunc,
) -> nsresult {
    assert!(conn.socket.is_none(), "NSPR IO path");

    loop {
        match conn.conn.process_output(Instant::now()) {
            Output::Datagram(dg) => {
                let Ok(len) = u32::try_from(dg.len()) else {
                    return NS_ERROR_UNEXPECTED;
                };
                let rv = match dg.destination().ip() {
                    IpAddr::V4(v4) => send_func(
                        context,
                        AF_INET_U16,
                        v4.octets().as_ptr(),
                        dg.destination().port(),
                        dg.as_ptr(),
                        len,
                    ),
                    IpAddr::V6(v6) => send_func(
                        context,
                        AF_INET6_U16,
                        v6.octets().as_ptr(),
                        dg.destination().port(),
                        dg.as_ptr(),
                        len,
                    ),
                };
                if rv != NS_OK {
                    return rv;
                }
            }
            Output::Callback(to) => {
                let timeout = if to.is_zero() {
                    Duration::from_millis(1)
                } else {
                    to
                };
                let Ok(timeout) = u64::try_from(timeout.as_millis()) else {
                    return NS_ERROR_UNEXPECTED;
                };
                set_timer_func(context, timeout);
                break;
            }
            Output::None => {
                set_timer_func(context, u64::MAX);
                break;
            }
        }
    }
    NS_OK
}

#[repr(C)]
pub struct ProcessOutputAndSendResult {
    pub result: nsresult,
    pub bytes_written: u32,
}

/// Process output, retrieving outgoing datagrams from the Neqo state machine
/// and writing them to the socket.
#[no_mangle]
pub extern "C" fn neqo_http3conn_process_output_and_send(
    conn: &mut NeqoHttp3Conn,
    context: *mut c_void,
    set_timer_func: SetTimerFunc,
) -> ProcessOutputAndSendResult {
    let mut bytes_written: usize = 0;
    loop {
        let Ok(max_gso_segments) = min(
            static_prefs::pref!("network.http.http3.max_gso_segments")
                .try_into()
                .expect("u32 fit usize"),
            conn.socket
                .as_mut()
                .expect("non NSPR IO")
                .max_gso_segments(),
        )
        .try_into() else {
            qerror!("Socket return GSO size of 0");
            return ProcessOutputAndSendResult {
                result: NS_ERROR_UNEXPECTED,
                bytes_written: 0,
            };
        };

        let output = conn
            .buffered_outbound_datagram
            .take()
            .map(OutputBatch::DatagramBatch)
            .unwrap_or_else(|| {
                conn.conn
                    .process_multiple_output(Instant::now(), max_gso_segments)
            });
        match output {
            OutputBatch::DatagramBatch(mut dg) => {
                if !static_prefs::pref!("network.http.http3.ecn_mark") {
                    dg.set_tos(Tos::default());
                }

                if static_prefs::pref!("network.http.http3.block_loopback_ipv6_addr")
                    && matches!(dg.destination(), SocketAddr::V6(addr) if addr.ip().is_loopback())
                {
                    qdebug!("network.http.http3.block_loopback_ipv6_addr is set, returning NS_ERROR_CONNECTION_REFUSED for localhost IPv6");
                    return ProcessOutputAndSendResult {
                        result: NS_ERROR_CONNECTION_REFUSED,
                        bytes_written: 0,
                    };
                }

                match conn.socket.as_mut().expect("non NSPR IO").send(&dg) {
                    Ok(()) => {}
                    Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                        conn.increment_would_block_tx();
                        if static_prefs::pref!("network.http.http3.pr_poll_write") {
                            qdebug!("Buffer outbound datagram to be sent once UDP socket has write-availability.");
                            conn.buffered_outbound_datagram = Some(dg);
                            return ProcessOutputAndSendResult {
                                result: NS_BASE_STREAM_WOULD_BLOCK,
                                bytes_written: bytes_written.try_into().unwrap_or(u32::MAX),
                            };
                        } else {
                            qwarn!("dropping datagram as socket would block");
                            break;
                        }
                    }
                    Err(e) if e.raw_os_error() == Some(libc::EIO) && dg.num_datagrams() > 1 => {
                        qdebug!("Failed to send datagram batch size {} with error {e}. Missing GSO support? Resending as individual datagrams.", dg.num_datagrams());
                        let socket = conn.socket.as_mut().expect("non NSPR IO");
                        for single in dg.iter() {
                            let single = datagram::Batch::from(single.to_owned());
                            if let Err(e) = socket.send(&single) {
                                qwarn!("failed to resend datagram without GSO: {e}");
                                break;
                            }
                        }
                    }
                    Err(e) => {
                        qwarn!("failed to send datagram: {}", e);
                        return ProcessOutputAndSendResult {
                            result: into_nsresult(&e),
                            bytes_written: 0,
                        };
                    }
                }
                bytes_written += dg.data().len();

            }
            OutputBatch::Callback(to) => {
                let timeout = if to.is_zero() {
                    Duration::from_millis(1)
                } else {
                    to
                };
                let Ok(timeout) = u64::try_from(timeout.as_millis()) else {
                    return ProcessOutputAndSendResult {
                        result: NS_ERROR_UNEXPECTED,
                        bytes_written: 0,
                    };
                };
                set_timer_func(context, timeout);
                break;
            }
            OutputBatch::None => {
                set_timer_func(context, u64::MAX);
                break;
            }
        }
    }

    ProcessOutputAndSendResult {
        result: NS_OK,
        bytes_written: bytes_written.try_into().unwrap_or(u32::MAX),
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_close(conn: &mut NeqoHttp3Conn, error: u64) {
    conn.conn.close(Instant::now(), error, "");
}

fn is_excluded_header(name: &str) -> bool {
    matches!(
        name,
        "connection"
            | "host"
            | "keep-alive"
            | "proxy-connection"
            | "te"
            | "transfer-encoding"
            | "upgrade"
            | "sec-websocket-key"
    )
}

fn parse_headers(headers: &nsACString) -> Result<Vec<Header>, nsresult> {
    let mut hdrs = Vec::new();

    let headers_bytes: &[u8] = headers;

    for elem in headers_bytes.split(|&b| b == b'\r' || b == b'\n').skip(1) {
        if elem.is_empty() {
            continue;
        }
        if elem.starts_with(b":") {
            continue;
        }

        let colon_pos = match elem.iter().position(|&b| b == b':') {
            Some(pos) => pos,
            None => continue, 
        };

        let name_bytes = &elem[..colon_pos];
        let value_bytes = &elem[colon_pos + 1..];

        let name = match str::from_utf8(name_bytes) {
            Ok(n) => n.trim().to_lowercase(),
            Err(_) => return Err(NS_ERROR_DOM_INVALID_HEADER_NAME),
        };

        if is_excluded_header(&name) {
            continue;
        }

        let value = value_bytes
            .iter()
            .position(|&b| b != b' ' && b != b'\t')
            .map_or(&value_bytes[0..0], |start| {
                let end = value_bytes
                    .iter()
                    .rposition(|&b| b != b' ' && b != b'\t')
                    .map_or(value_bytes.len(), |pos| pos + 1);
                &value_bytes[start..end]
            })
            .to_vec();

        hdrs.push(Header::new(name, value));
    }
    Ok(hdrs)
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_fetch(
    conn: &mut NeqoHttp3Conn,
    method: &nsACString,
    scheme: &nsACString,
    host: &nsACString,
    path: &nsACString,
    headers: &nsACString,
    stream_id: &mut u64,
    urgency: u8,
    incremental: bool,
) -> nsresult {
    let hdrs = match parse_headers(headers) {
        Err(e) => {
            return e;
        }
        Ok(h) => h,
    };
    let Ok(method_tmp) = str::from_utf8(method) else {
        return NS_ERROR_INVALID_ARG;
    };
    let Ok(scheme_tmp) = str::from_utf8(scheme) else {
        return NS_ERROR_INVALID_ARG;
    };
    let Ok(host_tmp) = str::from_utf8(host) else {
        return NS_ERROR_INVALID_ARG;
    };
    let Ok(path_tmp) = str::from_utf8(path) else {
        return NS_ERROR_INVALID_ARG;
    };
    if urgency >= 8 {
        return NS_ERROR_INVALID_ARG;
    }
    let priority = Priority::new(urgency, incremental);
    match conn.conn.fetch(
        Instant::now(),
        method_tmp,
        (scheme_tmp, host_tmp, path_tmp),
        &hdrs,
        priority,
    ) {
        Ok(id) => {
            *stream_id = id.as_u64();
            NS_OK
        }
        Err(Http3Error::StreamLimit) => NS_BASE_STREAM_WOULD_BLOCK,
        Err(_) => NS_ERROR_UNEXPECTED,
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_connect(
    conn: &mut NeqoHttp3Conn,
    host: &nsACString,
    headers: &nsACString,
    stream_id: &mut u64,
    urgency: u8,
    incremental: bool,
) -> nsresult {
    let hdrs = match parse_headers(headers) {
        Err(e) => {
            return e;
        }
        Ok(h) => h,
    };
    let Ok(host_tmp) = str::from_utf8(host) else {
        return NS_ERROR_INVALID_ARG;
    };
    if urgency >= 8 {
        return NS_ERROR_INVALID_ARG;
    }
    let priority = Priority::new(urgency, incremental);
    match conn.conn.connect(Instant::now(), host_tmp, &hdrs, priority) {
        Ok(id) => {
            *stream_id = id.as_u64();
            NS_OK
        }
        Err(Http3Error::StreamLimit) => NS_BASE_STREAM_WOULD_BLOCK,
        Err(_) => NS_ERROR_UNEXPECTED,
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_priority_update(
    conn: &mut NeqoHttp3Conn,
    stream_id: u64,
    urgency: u8,
    incremental: bool,
) -> nsresult {
    if urgency >= 8 {
        return NS_ERROR_INVALID_ARG;
    }
    let priority = Priority::new(urgency, incremental);
    match conn
        .conn
        .priority_update(StreamId::from(stream_id), priority)
    {
        Ok(_) => NS_OK,
        Err(_) => NS_ERROR_UNEXPECTED,
    }
}

/// # Safety
///
/// Use of raw (i.e. unsafe) pointers as arguments.
#[no_mangle]
pub unsafe extern "C" fn neqo_htttp3conn_send_request_body(
    conn: &mut NeqoHttp3Conn,
    stream_id: u64,
    buf: *const u8,
    len: u32,
    read: &mut u32,
) -> nsresult {
    let array = slice::from_raw_parts(buf, len as usize);
    conn.conn
        .send_data(StreamId::from(stream_id), array, Instant::now())
        .map_or(NS_ERROR_UNEXPECTED, |amount| {
            let Ok(amount) = u32::try_from(amount) else {
                return NS_ERROR_UNEXPECTED;
            };
            *read = amount;
            if amount == 0 {
                NS_BASE_STREAM_WOULD_BLOCK
            } else {
                NS_OK
            }
        })
}

const fn crypto_error_code(err: &nss_rs::Error) -> u64 {
    match err {
        nss_rs::Error::Aead => 1,
        nss_rs::Error::CertificateLoading => 2,
        nss_rs::Error::CreateSslSocket => 3,
        nss_rs::Error::Hkdf => 4,
        nss_rs::Error::Internal => 5,
        nss_rs::Error::IntegerOverflow => 6,
        nss_rs::Error::InvalidEpoch => 7,
        nss_rs::Error::MixedHandshakeMethod => 8,
        nss_rs::Error::NoDataAvailable => 9,
        nss_rs::Error::Nss { .. } => 10,
        nss_rs::Error::SelfEncrypt => 12,
        nss_rs::Error::TimeTravel => 13,
        nss_rs::Error::UnsupportedCipher => 14,
        nss_rs::Error::UnsupportedVersion => 15,
        nss_rs::Error::String => 16,
        nss_rs::Error::EchRetry(_) => 17,
        nss_rs::Error::CipherInit => 18,
        nss_rs::Error::CertificateDecoding => 19,
        nss_rs::Error::CertificateEncoding => 20,
        nss_rs::Error::InvalidCertificateCompressionID => 21,
        nss_rs::Error::InvalidAlpn => 22,
        nss_rs::Error::AeadTruncated => 23,
        nss_rs::Error::InvalidInput => 24,
        nss_rs::Error::UnsupportedCurve => 25,
        nss_rs::Error::UnsupportedHash => 26,
        nss_rs::Error::InvalidState => 27,
    }
}

#[repr(C)]
pub enum CloseError {
    TransportInternalError,
    TransportInternalErrorOther(u16),
    TransportError(u64),
    CryptoError(u64),
    CryptoAlert(u8),
    PeerAppError(u64),
    PeerError(u64),
    AppError(u64),
    EchRetry,
}

impl From<TransportError> for CloseError {
    fn from(error: TransportError) -> Self {
        #[expect(clippy::match_same_arms, reason = "It's cleaner this way.")]
        match error {
            TransportError::Internal => Self::TransportInternalError,
            TransportError::Crypto(nss_rs::Error::EchRetry(_)) => Self::EchRetry,
            TransportError::Crypto(c) => Self::CryptoError(crypto_error_code(&c)),
            TransportError::CryptoAlert(c) => Self::CryptoAlert(c),
            TransportError::PeerApplication(c) => Self::PeerAppError(c),
            TransportError::Peer(c) => Self::PeerError(c),
            TransportError::None
            | TransportError::IdleTimeout
            | TransportError::ConnectionRefused
            | TransportError::FlowControl
            | TransportError::StreamLimit
            | TransportError::StreamState
            | TransportError::FinalSize
            | TransportError::FrameEncoding
            | TransportError::TransportParameter
            | TransportError::ProtocolViolation
            | TransportError::InvalidToken
            | TransportError::KeysExhausted
            | TransportError::Application
            | TransportError::NoAvailablePath
            | TransportError::CryptoBufferExceeded => Self::TransportError(error.code()),
            TransportError::EchRetry(_) => Self::EchRetry,
            TransportError::AckedUnsentPacket => Self::TransportInternalErrorOther(0),
            TransportError::ConnectionIdLimitExceeded => Self::TransportInternalErrorOther(1),
            TransportError::ConnectionIdsExhausted => Self::TransportInternalErrorOther(2),
            TransportError::ConnectionState => Self::TransportInternalErrorOther(3),
            TransportError::Decrypt => Self::TransportInternalErrorOther(5),
            TransportError::IntegerOverflow => Self::TransportInternalErrorOther(7),
            TransportError::InvalidInput => Self::TransportInternalErrorOther(8),
            TransportError::InvalidMigration => Self::TransportInternalErrorOther(9),
            TransportError::InvalidPacket => Self::TransportInternalErrorOther(10),
            TransportError::InvalidResumptionToken => Self::TransportInternalErrorOther(11),
            TransportError::InvalidRetry => Self::TransportInternalErrorOther(12),
            TransportError::InvalidStreamId => Self::TransportInternalErrorOther(13),
            TransportError::KeysDiscarded(_) => Self::TransportInternalErrorOther(14),
            TransportError::KeysPending(_) => Self::TransportInternalErrorOther(15),
            TransportError::KeyUpdateBlocked => Self::TransportInternalErrorOther(16),
            TransportError::NoMoreData => Self::TransportInternalErrorOther(17),
            TransportError::NotConnected => Self::TransportInternalErrorOther(18),
            TransportError::PacketNumberOverlap => Self::TransportInternalErrorOther(19),
            TransportError::StatelessReset => Self::TransportInternalErrorOther(20),
            TransportError::TooMuchData => Self::TransportInternalErrorOther(21),
            TransportError::UnexpectedMessage => Self::TransportInternalErrorOther(22),
            TransportError::UnknownConnectionId => Self::TransportInternalErrorOther(23),
            TransportError::UnknownFrameType => Self::TransportInternalErrorOther(24),
            TransportError::VersionNegotiation => Self::TransportInternalErrorOther(25),
            TransportError::WrongRole => Self::TransportInternalErrorOther(26),
            TransportError::Qlog => Self::TransportInternalErrorOther(27),
            TransportError::NotAvailable => Self::TransportInternalErrorOther(28),
            TransportError::DisabledVersion => Self::TransportInternalErrorOther(29),
            TransportError::UnknownTransportParameter => Self::TransportInternalErrorOther(30),
        }
    }
}

impl From<neqo_transport::CloseReason> for CloseError {
    fn from(error: neqo_transport::CloseReason) -> Self {
        match error {
            neqo_transport::CloseReason::Transport(c) => c.into(),
            neqo_transport::CloseReason::Application(c) => Self::AppError(c),
        }
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_cancel_fetch(
    conn: &mut NeqoHttp3Conn,
    stream_id: u64,
    error: u64,
) -> nsresult {
    match conn.conn.cancel_fetch(StreamId::from(stream_id), error) {
        Ok(()) => NS_OK,
        Err(_) => NS_ERROR_INVALID_ARG,
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_reset_stream(
    conn: &mut NeqoHttp3Conn,
    stream_id: u64,
    error: u64,
) -> nsresult {
    match conn
        .conn
        .stream_reset_send(StreamId::from(stream_id), error)
    {
        Ok(()) => NS_OK,
        Err(_) => NS_ERROR_INVALID_ARG,
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_stream_stop_sending(
    conn: &mut NeqoHttp3Conn,
    stream_id: u64,
    error: u64,
) -> nsresult {
    match conn
        .conn
        .stream_stop_sending(StreamId::from(stream_id), error)
    {
        Ok(()) => NS_OK,
        Err(_) => NS_ERROR_INVALID_ARG,
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_close_stream(
    conn: &mut NeqoHttp3Conn,
    stream_id: u64,
) -> nsresult {
    match conn
        .conn
        .stream_close_send(StreamId::from(stream_id), Instant::now())
    {
        Ok(()) => NS_OK,
        Err(_) => NS_ERROR_INVALID_ARG,
    }
}

#[repr(C)]
pub enum WebTransportStreamType {
    BiDi,
    UniDi,
}

impl From<StreamType> for WebTransportStreamType {
    fn from(t: StreamType) -> Self {
        match t {
            StreamType::BiDi => Self::BiDi,
            StreamType::UniDi => Self::UniDi,
        }
    }
}

impl From<WebTransportStreamType> for StreamType {
    fn from(t: WebTransportStreamType) -> Self {
        match t {
            WebTransportStreamType::BiDi => Self::BiDi,
            WebTransportStreamType::UniDi => Self::UniDi,
        }
    }
}

#[repr(C)]
pub enum SessionCloseReasonExternal {
    Error(u64),
    Status(u16),
    Clean(u32),
}

impl SessionCloseReasonExternal {
    fn new(reason: session::CloseReason, data: &mut ThinVec<u8>) -> Self {
        match reason {
            session::CloseReason::Error(e) => Self::Error(e),
            session::CloseReason::Status(s) => Self::Status(s),
            session::CloseReason::Clean { error, message } => {
                data.extend_from_slice(message.as_ref());
                Self::Clean(error)
            }
        }
    }
}

#[repr(C)]
pub enum WebTransportEventExternal {
    Negotiated(bool),
    Session(u64),
    SessionClosed {
        stream_id: u64,
        reason: SessionCloseReasonExternal,
    },
    NewStream {
        stream_id: u64,
        stream_type: WebTransportStreamType,
        session_id: u64,
    },
    Datagram {
        session_id: u64,
    },
}
#[repr(C)]
pub enum ConnectUdpEventExternal {
    Negotiated(bool),
    Session(u64),
    SessionClosed {
        stream_id: u64,
        reason: SessionCloseReasonExternal,
    },
    Datagram {
        session_id: u64,
    },
}

impl WebTransportEventExternal {
    fn new(event: WebTransportEvent, data: &mut ThinVec<u8>) -> Self {
        match event {
            WebTransportEvent::Negotiated(n) => Self::Negotiated(n),
            WebTransportEvent::NewSession {
                stream_id, status, ..
            } => {
                data.extend_from_slice(b"HTTP/3 ");
                data.extend_from_slice(status.to_string().as_bytes());
                data.extend_from_slice(b"\r\n\r\n");
                Self::Session(stream_id.as_u64())
            }
            WebTransportEvent::SessionClosed {
                stream_id, reason, ..
            } => match reason {
                session::CloseReason::Status(status) => {
                    data.extend_from_slice(b"HTTP/3 ");
                    data.extend_from_slice(status.to_string().as_bytes());
                    data.extend_from_slice(b"\r\n\r\n");
                    Self::Session(stream_id.as_u64())
                }
                _ => Self::SessionClosed {
                    stream_id: stream_id.as_u64(),
                    reason: SessionCloseReasonExternal::new(reason, data),
                },
            },
            WebTransportEvent::NewStream {
                stream_id,
                session_id,
            } => Self::NewStream {
                stream_id: stream_id.as_u64(),
                stream_type: stream_id.stream_type().into(),
                session_id: session_id.as_u64(),
            },
            WebTransportEvent::Datagram {
                session_id,
                datagram,
            } => {
                data.extend_from_slice(datagram.as_ref());
                Self::Datagram {
                    session_id: session_id.as_u64(),
                }
            }
        }
    }
}
impl ConnectUdpEventExternal {
    fn new(event: ConnectUdpEvent, data: &mut ThinVec<u8>) -> Self {
        match event {
            ConnectUdpEvent::Negotiated(n) => Self::Negotiated(n),
            ConnectUdpEvent::NewSession {
                stream_id, status, ..
            } => {
                data.extend_from_slice(b"HTTP/3 ");
                data.extend_from_slice(status.to_string().as_bytes());
                data.extend_from_slice(b"\r\n\r\n");
                Self::Session(stream_id.as_u64())
            }
            ConnectUdpEvent::SessionClosed {
                stream_id, reason, ..
            } => match reason {
                session::CloseReason::Status(status) => {
                    data.extend_from_slice(b"HTTP/3 ");
                    data.extend_from_slice(status.to_string().as_bytes());
                    data.extend_from_slice(b"\r\n\r\n");
                    Self::Session(stream_id.as_u64())
                }
                _ => Self::SessionClosed {
                    stream_id: stream_id.as_u64(),
                    reason: SessionCloseReasonExternal::new(reason, data),
                },
            },
            ConnectUdpEvent::Datagram {
                session_id,
                datagram,
            } => {
                data.extend_from_slice(datagram.as_ref());
                Self::Datagram {
                    session_id: session_id.as_u64(),
                }
            }
        }
    }
}

#[repr(C)]
pub enum Http3Event {
    /// A request stream has space for more data to be sent.
    DataWritable {
        stream_id: u64,
    },
    /// A server has sent a `STOP_SENDING` frame.
    StopSending {
        stream_id: u64,
        error: u64,
    },
    HeaderReady {
        stream_id: u64,
        fin: bool,
        interim: bool,
    },
    /// New bytes available for reading.
    DataReadable {
        stream_id: u64,
    },
    /// Peer reset the stream.
    Reset {
        stream_id: u64,
        error: u64,
        local: bool,
    },
    /// A `PushPromise`
    PushPromise {
        push_id: u64,
        request_stream_id: u64,
    },
    /// A push response headers are ready.
    PushHeaderReady {
        push_id: u64,
        fin: bool,
    },
    /// New bytes are available on a push stream for reading.
    PushDataReadable {
        push_id: u64,
    },
    /// A push has been canceled.
    PushCanceled {
        push_id: u64,
    },
    PushReset {
        push_id: u64,
        error: u64,
    },
    RequestsCreatable,
    AuthenticationNeeded,
    ZeroRttRejected,
    ConnectionConnected,
    GoawayReceived,
    ConnectionClosing {
        error: CloseError,
    },
    ConnectionClosed {
        error: CloseError,
    },
    ResumptionToken {
        expire_in: u64, 
    },
    EchFallbackAuthenticationNeeded,
    WebTransport(WebTransportEventExternal),
    ConnectUdp(ConnectUdpEventExternal),
    NoEvent,
}

fn sanitize_header(mut y: Cow<[u8]>) -> Cow<[u8]> {
    for i in 0..y.len() {
        if matches!(y[i], b'\n' | b'\r' | b'\0') {
            y.to_mut()[i] = b' ';
        }
    }
    y
}

fn convert_h3_to_h1_headers(headers: &[Header], ret_headers: &mut ThinVec<u8>) -> nsresult {
    if headers.iter().filter(|&h| h.name() == ":status").count() != 1 {
        return NS_ERROR_ILLEGAL_VALUE;
    }

    let status_val = headers
        .iter()
        .find(|&h| h.name() == ":status")
        .expect("must be one")
        .value();

    ret_headers.extend_from_slice(b"HTTP/3 ");
    ret_headers.extend_from_slice(status_val);
    ret_headers.extend_from_slice(b"\r\n");

    for hdr in headers.iter().filter(|&h| h.name() != ":status") {
        ret_headers.extend_from_slice(&sanitize_header(Cow::from(hdr.name().as_bytes())));
        ret_headers.extend_from_slice(b": ");
        ret_headers.extend_from_slice(&sanitize_header(Cow::from(hdr.value())));
        ret_headers.extend_from_slice(b"\r\n");
    }
    ret_headers.extend_from_slice(b"\r\n");
    NS_OK
}

#[expect(clippy::too_many_lines, reason = "Nothing to be done about it.")]
#[no_mangle]
pub extern "C" fn neqo_http3conn_event(
    conn: &mut NeqoHttp3Conn,
    ret_event: &mut Http3Event,
    data: &mut ThinVec<u8>,
) -> nsresult {
    while let Some(evt) = conn.conn.next_event() {
        let fe = match evt {
            Http3ClientEvent::DataWritable { stream_id } => Http3Event::DataWritable {
                stream_id: stream_id.as_u64(),
            },
            Http3ClientEvent::StopSending { stream_id, error } => Http3Event::StopSending {
                stream_id: stream_id.as_u64(),
                error,
            },
            Http3ClientEvent::HeaderReady {
                stream_id,
                headers,
                fin,
                interim,
            } => {
                let res = convert_h3_to_h1_headers(&headers, data);
                if res != NS_OK {
                    return res;
                }
                Http3Event::HeaderReady {
                    stream_id: stream_id.as_u64(),
                    fin,
                    interim,
                }
            }
            Http3ClientEvent::DataReadable { stream_id } => Http3Event::DataReadable {
                stream_id: stream_id.as_u64(),
            },
            Http3ClientEvent::Reset {
                stream_id,
                error,
                local,
            } => Http3Event::Reset {
                stream_id: stream_id.as_u64(),
                error,
                local,
            },
            Http3ClientEvent::PushPromise {
                push_id,
                request_stream_id,
                headers,
            } => {
                let res = convert_h3_to_h1_headers(&headers, data);
                if res != NS_OK {
                    return res;
                }
                Http3Event::PushPromise {
                    push_id: push_id.into(),
                    request_stream_id: request_stream_id.as_u64(),
                }
            }
            Http3ClientEvent::PushHeaderReady {
                push_id,
                headers,
                fin,
                interim,
            } => {
                if interim {
                    Http3Event::NoEvent
                } else {
                    let res = convert_h3_to_h1_headers(&headers, data);
                    if res != NS_OK {
                        return res;
                    }
                    Http3Event::PushHeaderReady {
                        push_id: push_id.into(),
                        fin,
                    }
                }
            }
            Http3ClientEvent::PushDataReadable { push_id } => Http3Event::PushDataReadable {
                push_id: push_id.into(),
            },
            Http3ClientEvent::PushCanceled { push_id } => Http3Event::PushCanceled {
                push_id: push_id.into(),
            },
            Http3ClientEvent::PushReset { push_id, error } => Http3Event::PushReset {
                push_id: push_id.into(),
                error,
            },
            Http3ClientEvent::RequestsCreatable => Http3Event::RequestsCreatable,
            Http3ClientEvent::AuthenticationNeeded => Http3Event::AuthenticationNeeded,
            Http3ClientEvent::ZeroRttRejected => Http3Event::ZeroRttRejected,
            Http3ClientEvent::ResumptionToken(token) => {
                if token.expiration_time() > Instant::now() {
                    let e = (token.expiration_time() - Instant::now()).as_micros();
                    u64::try_from(e).map_or(Http3Event::NoEvent, |expire_in| {
                        data.extend_from_slice(token.as_ref());
                        Http3Event::ResumptionToken { expire_in }
                    })
                } else {
                    Http3Event::NoEvent
                }
            }
            Http3ClientEvent::GoawayReceived => Http3Event::GoawayReceived,
            Http3ClientEvent::StateChange(state) => match state {
                Http3State::Connected => Http3Event::ConnectionConnected,
                Http3State::Closing(reason) => {
                    if let neqo_transport::CloseReason::Transport(
                        TransportError::Crypto(nss_rs::Error::EchRetry(c))
                        | TransportError::EchRetry(c),
                    ) = &reason
                    {
                        data.extend_from_slice(c.as_ref());
                    }

                    Http3Event::ConnectionClosing {
                        error: reason.into(),
                    }
                }
                Http3State::Closed(error_code) => {
                    if let neqo_transport::CloseReason::Transport(
                        TransportError::Crypto(nss_rs::Error::EchRetry(c))
                        | TransportError::EchRetry(c),
                    ) = &error_code
                    {
                        data.extend_from_slice(c.as_ref());
                    }
                    Http3Event::ConnectionClosed {
                        error: error_code.into(),
                    }
                }
                _ => Http3Event::NoEvent,
            },
            Http3ClientEvent::EchFallbackAuthenticationNeeded { public_name } => {
                data.extend_from_slice(public_name.as_ref());
                Http3Event::EchFallbackAuthenticationNeeded
            }
            Http3ClientEvent::WebTransport(e) => {
                Http3Event::WebTransport(WebTransportEventExternal::new(e, data))
            }
            Http3ClientEvent::ConnectUdp(e) => {
                Http3Event::ConnectUdp(ConnectUdpEventExternal::new(e, data))
            }
        };

        if !matches!(fe, Http3Event::NoEvent) {
            *ret_event = fe;
            return NS_OK;
        }
    }

    *ret_event = Http3Event::NoEvent;
    NS_OK
}

///
/// # Safety
///
/// Marked as unsafe given exposition via FFI i.e. `extern "C"`.
#[no_mangle]
pub unsafe extern "C" fn neqo_http3conn_read_response_data(
    conn: &mut NeqoHttp3Conn,
    stream_id: u64,
    buf: *mut u8,
    len: u32,
    read: &mut u32,
    fin: &mut bool,
) -> nsresult {
    let array = slice::from_raw_parts_mut(buf, len as usize);
    match conn
        .conn
        .read_data(Instant::now(), StreamId::from(stream_id), &mut array[..])
    {
        Ok((amount, fin_recvd)) => {
            let Ok(amount) = u32::try_from(amount) else {
                return NS_ERROR_NET_HTTP3_PROTOCOL_ERROR;
            };
            *read = amount;
            *fin = fin_recvd;
            if (amount == 0) && !fin_recvd {
                NS_BASE_STREAM_WOULD_BLOCK
            } else {
                NS_OK
            }
        }
        Err(Http3Error::InvalidStreamId | Http3Error::Transport(TransportError::NoMoreData)) => {
            NS_ERROR_INVALID_ARG
        }
        Err(_) => NS_ERROR_NET_HTTP3_PROTOCOL_ERROR,
    }
}

#[repr(C)]
pub struct NeqoSecretInfo {
    set: bool,
    version: u16,
    cipher: u16,
    group: u16,
    resumed: bool,
    early_data: bool,
    alpn: nsCString,
    signature_scheme: u16,
    ech_accepted: bool,
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_tls_info(
    conn: &mut NeqoHttp3Conn,
    sec_info: &mut NeqoSecretInfo,
) -> nsresult {
    match conn.conn.tls_info() {
        Some(info) => {
            sec_info.set = true;
            sec_info.version = info.version();
            sec_info.cipher = info.cipher_suite();
            sec_info.group = info.key_exchange();
            sec_info.resumed = info.resumed();
            sec_info.early_data = info.early_data_accepted();
            sec_info.alpn = info.alpn().map_or_else(nsCString::new, nsCString::from);
            sec_info.signature_scheme = info.signature_scheme();
            sec_info.ech_accepted = info.ech_accepted();
            NS_OK
        }
        None => NS_ERROR_NOT_AVAILABLE,
    }
}

#[repr(C)]
pub struct NeqoCertificateInfo {
    certs: ThinVec<ThinVec<u8>>,
    stapled_ocsp_responses_present: bool,
    stapled_ocsp_responses: ThinVec<ThinVec<u8>>,
    signed_cert_timestamp_present: bool,
    signed_cert_timestamp: ThinVec<u8>,
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_peer_certificate_info(
    conn: &mut NeqoHttp3Conn,
    neqo_certs_info: &mut NeqoCertificateInfo,
) -> nsresult {
    let Some(certs_info) = conn.conn.peer_certificate() else {
        return NS_ERROR_NOT_AVAILABLE;
    };

    neqo_certs_info.certs = certs_info.iter().map(ThinVec::from).collect();

    match &mut certs_info.stapled_ocsp_responses() {
        Some(ocsp_val) => {
            neqo_certs_info.stapled_ocsp_responses_present = true;
            neqo_certs_info.stapled_ocsp_responses = ocsp_val
                .iter()
                .map(|ocsp| ocsp.iter().copied().collect())
                .collect();
        }
        None => {
            neqo_certs_info.stapled_ocsp_responses_present = false;
        }
    };

    match certs_info.signed_cert_timestamp() {
        Some(sct_val) => {
            neqo_certs_info.signed_cert_timestamp_present = true;
            neqo_certs_info
                .signed_cert_timestamp
                .extend_from_slice(sct_val);
        }
        None => {
            neqo_certs_info.signed_cert_timestamp_present = false;
        }
    };

    NS_OK
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_authenticated(conn: &mut NeqoHttp3Conn, error: PRErrorCode) {
    conn.conn.authenticated(error.into(), Instant::now());
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_set_resumption_token(
    conn: &mut NeqoHttp3Conn,
    token: &mut ThinVec<u8>,
) -> nsresult {
    match conn.conn.enable_resumption(Instant::now(), token) {
        Ok(_) => NS_OK,
        Err(_) => NS_ERROR_NET_HTTP3_PROTOCOL_ERROR,
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_set_ech_config(
    conn: &mut NeqoHttp3Conn,
    ech_config: &mut ThinVec<u8>,
) {
    _ = conn.conn.enable_ech(ech_config);
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_is_zero_rtt(conn: &mut NeqoHttp3Conn) -> bool {
    conn.conn.state() == Http3State::ZeroRtt
}

#[repr(C)]
#[derive(Default)]
pub struct Http3Stats {
    /// Total packets received, including all the bad ones.
    pub packets_rx: usize,
    /// Duplicate packets received.
    pub dups_rx: usize,
    /// Dropped packets or dropped garbage.
    pub dropped_rx: usize,
    /// The number of packet that were saved for later processing.
    pub saved_datagrams: usize,
    /// Total packets sent.
    pub packets_tx: usize,
    /// Total number of packets that are declared lost.
    pub lost: usize,
    /// Late acknowledgments, for packets that were declared lost already.
    pub late_ack: usize,
    /// Acknowledgments for packets that contained data that was marked
    /// for retransmission when the PTO timer popped.
    pub pto_ack: usize,
    /// Count PTOs. Single PTOs, 2 PTOs in a row, 3 PTOs in row, etc. are counted
    /// separately.
    pub pto_counts: [usize; 16],
    /// The count of WouldBlock errors encountered during receive operations on the UDP socket.
    pub would_block_rx: usize,
    /// The count of WouldBlock errors encountered during transmit operations on the UDP socket.
    pub would_block_tx: usize,
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_get_stats(conn: &mut NeqoHttp3Conn, stats: &mut Http3Stats) {
    let t_stats = conn.conn.transport_stats();
    stats.packets_rx = t_stats.packets_rx;
    stats.dups_rx = t_stats.dups_rx;
    stats.dropped_rx = t_stats.dropped_rx;
    stats.saved_datagrams = t_stats.saved_datagrams;
    stats.packets_tx = t_stats.packets_tx;
    stats.lost = t_stats.lost;
    stats.late_ack = t_stats.late_ack;
    stats.pto_ack = t_stats.pto_ack;
    stats.pto_counts = t_stats.pto_counts;
    stats.would_block_rx = conn.would_block_rx_count();
    stats.would_block_tx = conn.would_block_tx_count();
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_webtransport_create_session(
    conn: &mut NeqoHttp3Conn,
    host: &nsACString,
    path: &nsACString,
    headers: &nsACString,
    stream_id: &mut u64,
) -> nsresult {
    let hdrs = match parse_headers(headers) {
        Err(e) => {
            return e;
        }
        Ok(h) => h,
    };
    let Ok(host_tmp) = str::from_utf8(host) else {
        return NS_ERROR_INVALID_ARG;
    };
    let Ok(path_tmp) = str::from_utf8(path) else {
        return NS_ERROR_INVALID_ARG;
    };

    match conn.conn.webtransport_create_session(
        Instant::now(),
        ("https", host_tmp, path_tmp),
        &hdrs,
    ) {
        Ok(id) => {
            *stream_id = id.as_u64();
            NS_OK
        }
        Err(Http3Error::StreamLimit) => NS_BASE_STREAM_WOULD_BLOCK,
        Err(_) => NS_ERROR_UNEXPECTED,
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_connect_udp_create_session(
    conn: &mut NeqoHttp3Conn,
    host: &nsACString,
    path: &nsACString,
    headers: &nsACString,
    stream_id: &mut u64,
) -> nsresult {
    let hdrs = match parse_headers(headers) {
        Err(e) => {
            return e;
        }
        Ok(h) => h,
    };
    let Ok(host_tmp) = str::from_utf8(host) else {
        return NS_ERROR_INVALID_ARG;
    };
    let Ok(path_tmp) = str::from_utf8(path) else {
        return NS_ERROR_INVALID_ARG;
    };

    match conn
        .conn
        .connect_udp_create_session(Instant::now(), ("https", host_tmp, path_tmp), &hdrs)
    {
        Ok(id) => {
            *stream_id = id.as_u64();
            NS_OK
        }
        Err(Http3Error::StreamLimit) => NS_BASE_STREAM_WOULD_BLOCK,
        Err(_) => NS_ERROR_UNEXPECTED,
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_webtransport_close_session(
    conn: &mut NeqoHttp3Conn,
    session_id: u64,
    error: u32,
    message: &nsACString,
) -> nsresult {
    let Ok(message_tmp) = str::from_utf8(message) else {
        return NS_ERROR_INVALID_ARG;
    };
    match conn.conn.webtransport_close_session(
        StreamId::from(session_id),
        error,
        message_tmp,
        Instant::now(),
    ) {
        Ok(()) => NS_OK,
        Err(_) => NS_ERROR_INVALID_ARG,
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_connect_udp_close_session(
    conn: &mut NeqoHttp3Conn,
    session_id: u64,
    error: u32,
    message: &nsACString,
) -> nsresult {
    let Ok(message_tmp) = str::from_utf8(message) else {
        return NS_ERROR_INVALID_ARG;
    };
    match conn.conn.connect_udp_close_session(
        StreamId::from(session_id),
        error,
        message_tmp,
        Instant::now(),
    ) {
        Ok(()) => NS_OK,
        Err(_) => NS_ERROR_INVALID_ARG,
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_webtransport_create_stream(
    conn: &mut NeqoHttp3Conn,
    session_id: u64,
    stream_type: WebTransportStreamType,
    stream_id: &mut u64,
) -> nsresult {
    match conn
        .conn
        .webtransport_create_stream(StreamId::from(session_id), stream_type.into())
    {
        Ok(id) => {
            *stream_id = id.as_u64();
            NS_OK
        }
        Err(Http3Error::StreamLimit) => NS_BASE_STREAM_WOULD_BLOCK,
        Err(_) => NS_ERROR_UNEXPECTED,
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_webtransport_send_datagram(
    conn: &mut NeqoHttp3Conn,
    session_id: u64,
    data: &mut ThinVec<u8>,
    tracking_id: u64,
) -> nsresult {
    let id = if tracking_id == 0 {
        None
    } else {
        Some(tracking_id)
    };
    match conn
        .conn
        .webtransport_send_datagram(StreamId::from(session_id), data, id, Instant::now())
    {
        Ok(()) => NS_OK,
        Err(Http3Error::Transport(TransportError::TooMuchData)) => NS_ERROR_NOT_AVAILABLE,
        Err(_) => NS_ERROR_UNEXPECTED,
    }
}
#[no_mangle]
pub extern "C" fn neqo_http3conn_connect_udp_send_datagram(
    conn: &mut NeqoHttp3Conn,
    session_id: u64,
    data: &mut ThinVec<u8>,
    tracking_id: u64,
) -> nsresult {
    let id = if tracking_id == 0 {
        None
    } else {
        Some(tracking_id)
    };
    match conn
        .conn
        .connect_udp_send_datagram(StreamId::from(session_id), data, id, Instant::now())
    {
        Ok(()) => NS_OK,
        Err(Http3Error::Transport(TransportError::TooMuchData)) => NS_ERROR_NOT_AVAILABLE,
        Err(_) => NS_ERROR_UNEXPECTED,
    }
}

#[no_mangle]
pub extern "C" fn neqo_http3conn_webtransport_max_datagram_size(
    conn: &mut NeqoHttp3Conn,
    session_id: u64,
    result: &mut u64,
) -> nsresult {
    conn.conn
        .webtransport_max_datagram_size(StreamId::from(session_id))
        .map_or(NS_ERROR_UNEXPECTED, |size| {
            *result = size;
            NS_OK
        })
}

/// # Safety
///
/// Use of raw (i.e. unsafe) pointers as arguments.
#[no_mangle]
pub unsafe extern "C" fn neqo_http3conn_webtransport_set_sendorder(
    conn: &mut NeqoHttp3Conn,
    stream_id: u64,
    sendorder: *const i64,
) -> nsresult {
    match conn
        .conn
        .webtransport_set_sendorder(StreamId::from(stream_id), sendorder.as_ref().copied())
    {
        Ok(()) => NS_OK,
        Err(_) => NS_ERROR_UNEXPECTED,
    }
}

/// Convert a [`std::io::Error`] into a [`nsresult`].
///
/// Note that this conversion is specific to `neqo_glue`, i.e. does not aim to
/// implement a general-purpose conversion.
/// Treat NS_ERROR_NET_RESET as a generic retryable error for the upper layer.
///
/// Modeled after
/// [`ErrorAccordingToNSPR`](https://searchfox.org/mozilla-central/rev/a965e3c683ecc035dee1de72bd33a8d91b1203ed/netwerk/base/nsSocketTransport2.cpp#164-168).
fn into_nsresult(e: &io::Error) -> nsresult {
    #[expect(clippy::match_same_arms, reason = "It's cleaner this way.")]
    match e.kind() {
        io::ErrorKind::ConnectionRefused => NS_ERROR_CONNECTION_REFUSED,
        io::ErrorKind::ConnectionReset => NS_ERROR_NET_RESET,

        io::ErrorKind::AddrNotAvailable => NS_ERROR_CONNECTION_REFUSED,

        io::ErrorKind::ConnectionAborted => NS_ERROR_NET_RESET,

        io::ErrorKind::NotConnected => NS_ERROR_NOT_CONNECTED,
        io::ErrorKind::AddrInUse => NS_ERROR_SOCKET_ADDRESS_IN_USE,
        io::ErrorKind::AlreadyExists => NS_ERROR_FILE_ALREADY_EXISTS,
        io::ErrorKind::WouldBlock => NS_BASE_STREAM_WOULD_BLOCK,





        io::ErrorKind::TimedOut => NS_ERROR_NET_TIMEOUT,
        io::ErrorKind::Interrupted => NS_ERROR_NET_INTERRUPT,

        io::ErrorKind::UnexpectedEof => NS_ERROR_NET_INTERRUPT,

        io::ErrorKind::OutOfMemory => NS_ERROR_OUT_OF_MEMORY,


        io::ErrorKind::NotFound
        | io::ErrorKind::PermissionDenied
        | io::ErrorKind::BrokenPipe
        | io::ErrorKind::InvalidData
        | io::ErrorKind::WriteZero
        | io::ErrorKind::Unsupported
        | io::ErrorKind::Other => NS_ERROR_NET_RESET,


        _ => NS_ERROR_NET_RESET,
    }
}

#[repr(C)]
pub struct NeqoEncoder {
    encoder: Encoder,
    refcnt: AtomicRefcnt,
}

impl NeqoEncoder {
    fn new() -> Result<RefPtr<NeqoEncoder>, nsresult> {
        let encoder = Encoder::default();
        let encoder = Box::into_raw(Box::new(NeqoEncoder {
            encoder,
            refcnt: unsafe { AtomicRefcnt::new() },
        }));
        unsafe { Ok(RefPtr::from_raw(encoder).unwrap()) }
    }
}

#[no_mangle]
pub unsafe extern "C" fn neqo_encoder_addref(encoder: &NeqoEncoder) {
    encoder.refcnt.inc();
}

#[no_mangle]
pub unsafe extern "C" fn neqo_encoder_release(encoder: &NeqoEncoder) {
    let rc = encoder.refcnt.dec();
    if rc == 0 {
        drop(Box::from_raw(encoder as *const _ as *mut NeqoEncoder));
    }
}

unsafe impl RefCounted for NeqoEncoder {
    unsafe fn addref(&self) {
        neqo_encoder_addref(self);
    }
    unsafe fn release(&self) {
        neqo_encoder_release(self);
    }
}

#[no_mangle]
pub extern "C" fn neqo_encoder_new(result: &mut *const NeqoEncoder) {
    *result = ptr::null_mut();
    if let Ok(encoder) = NeqoEncoder::new() {
        encoder.forget(result);
    }
}

#[no_mangle]
pub extern "C" fn neqo_encode_byte(encoder: &mut NeqoEncoder, data: u8) {
    encoder.encoder.encode_byte(data);
}

#[no_mangle]
pub extern "C" fn neqo_encode_varint(encoder: &mut NeqoEncoder, data: u64) {
    encoder.encoder.encode_varint(data);
}

#[no_mangle]
pub extern "C" fn neqo_encode_uint(encoder: &mut NeqoEncoder, n: u32, data: u64) {
    encoder.encoder.encode_uint(n as usize, data);
}

#[no_mangle]
pub unsafe extern "C" fn neqo_encode_buffer(encoder: &mut NeqoEncoder, buf: *const u8, len: u32) {
    let array = slice::from_raw_parts(buf, len as usize);
    encoder.encoder.encode(array);
}

#[no_mangle]
pub unsafe extern "C" fn neqo_encode_vvec(encoder: &mut NeqoEncoder, buf: *const u8, len: u32) {
    let array = slice::from_raw_parts(buf, len as usize);
    encoder.encoder.encode_vvec(array);
}

#[no_mangle]
pub unsafe extern "C" fn neqo_encode_get_data(
    encoder: &mut NeqoEncoder,
    buf: *mut *const u8,
    read: &mut u32,
) {
    let data = encoder.encoder.as_ref();
    *read = data.len() as u32;
    unsafe {
        *buf = data.as_ptr();
    }
}

#[no_mangle]
pub extern "C" fn neqo_encode_varint_len(v: u64) -> usize {
    return Encoder::varint_len(v);
}

#[repr(C)]
pub struct NeqoDecoder {
    decoder: *mut Decoder<'static>,
    refcnt: AtomicRefcnt,
}

impl NeqoDecoder {
    fn new(buf: *const u8, len: u32) -> Result<RefPtr<NeqoDecoder>, nsresult> {
        let slice = unsafe { slice::from_raw_parts(buf, len as usize) };
        let decoder = Box::new(Decoder::new(slice));
        let wrapper = Box::into_raw(Box::new(NeqoDecoder {
            decoder: Box::into_raw(decoder),
            refcnt: unsafe { AtomicRefcnt::new() },
        }));

        unsafe { Ok(RefPtr::from_raw(wrapper).unwrap()) }
    }
}

#[no_mangle]
pub unsafe extern "C" fn neqo_decoder_addref(decoder: &NeqoDecoder) {
    decoder.refcnt.inc();
}

#[no_mangle]
pub unsafe extern "C" fn neqo_decoder_release(decoder: &NeqoDecoder) {
    let rc = decoder.refcnt.dec();
    if rc == 0 {
        unsafe {
            drop(Box::from_raw(decoder.decoder));
            drop(Box::from_raw(decoder as *const _ as *mut NeqoDecoder));
        }
    }
}

unsafe impl RefCounted for NeqoDecoder {
    unsafe fn addref(&self) {
        neqo_decoder_addref(self);
    }
    unsafe fn release(&self) {
        neqo_decoder_release(self);
    }
}

#[no_mangle]
pub extern "C" fn neqo_decoder_new(buf: *const u8, len: u32, result: &mut *const NeqoDecoder) {
    *result = ptr::null_mut();
    if let Ok(decoder) = NeqoDecoder::new(buf, len) {
        decoder.forget(result);
    }
}

#[no_mangle]
pub unsafe extern "C" fn neqo_decode_uint32(decoder: &mut NeqoDecoder, result: &mut u32) -> bool {
    let decoder = decoder.decoder.as_mut().unwrap();
    if let Some(v) = decoder.decode_uint::<u32>() {
        *result = v;
        return true;
    }
    false
}

#[no_mangle]
pub unsafe extern "C" fn neqo_decode_varint(decoder: &mut NeqoDecoder, result: &mut u64) -> bool {
    let decoder = decoder.decoder.as_mut().unwrap();
    if let Some(v) = decoder.decode_varint() {
        *result = v;
        return true;
    }
    false
}

#[no_mangle]
pub unsafe extern "C" fn neqo_decode(
    decoder: &mut NeqoDecoder,
    n: u32,
    buf: *mut *const u8,
    read: &mut u32,
) -> bool {
    let decoder = decoder.decoder.as_mut().unwrap();
    if let Some(data) = decoder.decode(n as usize) {
        *buf = data.as_ptr();
        *read = data.len() as u32;
        return true;
    }
    false
}

#[no_mangle]
pub unsafe extern "C" fn neqo_decode_remainder(
    decoder: &mut NeqoDecoder,
    buf: *mut *const u8,
    read: &mut u32,
) {
    let decoder = decoder.decoder.as_mut().unwrap();
    let data = decoder.decode_remainder();
    *buf = data.as_ptr();
    *read = data.len() as u32;
}

#[no_mangle]
pub unsafe extern "C" fn neqo_decoder_remaining(decoder: &mut NeqoDecoder) -> u64 {
    let decoder = decoder.decoder.as_mut().unwrap();
    decoder.remaining() as u64
}

#[no_mangle]
pub unsafe extern "C" fn neqo_decoder_offset(decoder: &mut NeqoDecoder) -> u64 {
    let decoder = decoder.decoder.as_mut().unwrap();
    decoder.offset() as u64
}

/// Enables the Apple fast datapath (`sendmsg_x`/`recvmsg_x`) for all
/// subsequently created QUIC sockets. Must only be called after the caller
/// has verified that these private APIs are available and functional.
type HeaderCallback = extern "C" fn(*mut c_void, *const u8, usize, *const u8, usize);

#[no_mangle]
pub extern "C" fn neqo_glue_test_parse_headers(
    headers_input: &nsACString,
    callback: HeaderCallback,
    user_data: *mut c_void,
) -> bool {
    match parse_headers(headers_input) {
        Ok(headers) => {
            for header in headers {
                let name_bytes = header.name().as_bytes();
                let value_bytes = header.value();
                callback(
                    user_data,
                    name_bytes.as_ptr(),
                    name_bytes.len(),
                    value_bytes.as_ptr(),
                    value_bytes.len(),
                );
            }
            true
        }
        Err(_) => false,
    }
}
