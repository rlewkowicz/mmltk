/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Semantics-free import channel between the host application and this shell.
//!
//! The host owns the allocation. It describes one over this socket together
//! with the descriptor backing it and the frame edge that says when it changed,
//! and later withdraws that description. Nothing here knows what a workspace,
//! workflow, generation, lease, activation, or retirement is: those are host
//! concepts and travel on the host's own transports.
//!
//! The shell bridge dispatcher services this socket independently of page
//! texture creation. Texture creation only claims an already admitted record;
//! it is not responsible for channel progress.
//!
//! The record layout below is the frozen Firefox mirror of the application-owned
//! import ABI. It is versioned by [`ABI_VERSION`], which changes only when the
//! normative layout or an opcode meaning changes.

use std::collections::{HashMap, HashSet, VecDeque};
use std::ffi::OsStr;
use std::fmt::{self, Write};
use std::mem;
use std::os::fd::{AsRawFd, FromRawFd, IntoRawFd, OwnedFd, RawFd};
use std::os::unix::ffi::OsStrExt;
use std::sync::{Mutex, OnceLock};

fn diagnostics_enabled_for(value: Option<&OsStr>) -> bool {
    value.is_some_and(|value| !value.is_empty())
}

static DIAGNOSTICS_ENABLED: OnceLock<bool> = OnceLock::new();
static PIXEL_PROBES_ENABLED: OnceLock<bool> = OnceLock::new();

pub(super) fn initialize_diagnostics() {
    let diagnostics = *DIAGNOSTICS_ENABLED.get_or_init(|| {
        let value = std::env::var_os("MMLTK_GUI_TRACE_FILE");
        diagnostics_enabled_for(value.as_deref())
    });
    PIXEL_PROBES_ENABLED.get_or_init(|| {
        diagnostics && std::env::var_os("MMLTK_GUI_PIXEL_TRACE").is_some_and(|value| value == "1")
    });
}

pub(super) fn workspace_diagnostics_enabled() -> bool {
    DIAGNOSTICS_ENABLED.get().copied().unwrap_or(false)
}

pub(super) fn workspace_pixel_probes_enabled() -> bool {
    PIXEL_PROBES_ENABLED.get().copied().unwrap_or(false)
}

pub(super) fn write_diagnostic(format: impl FnOnce(&mut String) -> fmt::Result) {
    if !workspace_diagnostics_enabled() {
        return;
    }
    let mut line = String::new();
    if format(&mut line).is_err() {
        return;
    }
    line.push('\n');
    let mut offset = 0;
    while offset < line.len() {
        let written = unsafe {
            libc::write(
                libc::STDERR_FILENO,
                line.as_ptr().add(offset).cast::<libc::c_void>(),
                line.len() - offset,
            )
        };
        if written > 0 {
            offset += written as usize;
        } else if written == 0 || std::io::Error::last_os_error().raw_os_error() != Some(libc::EINTR) {
            return;
        }
    }
}

pub fn trace_state(event: &str, id: SurfaceId, detail: &str) {
    write_diagnostic(|line| write!(line,
        "{{\"event\":\"firefox.workspace.{event}\",\"surface\":\"{id}\",\"detail\":\"{detail}\"}}"
    ));
}

/// Bumped only when the record layout or opcode meaning changes. Both sides
/// reject a record that does not carry the exact value they were built with.
pub const ABI_VERSION: u32 = 9;

/// Host to shell: the three descriptors in the ancillary data back the described
/// allocation, carry its frame edge, and expose its generic frame identity,
/// admitted under `id`.
const OPCODE_IMPORT: u32 = 1;
/// Host to shell: `id` is withdrawn. A live capability completes through the
/// exact Retired terminal after page sampling and mirror cleanup.
const OPCODE_DROP: u32 = 2;
/// Shell to host: `id` is imported and the page's texture exists.
const OPCODE_READY: u32 = 3;
/// Shell to host: `id` could not be imported, with a reason in `code`.
const OPCODE_FAILED: u32 = 4;
/// Shell to host: a capacity-exhausted generic layer mailbox regained a writable slot.
const OPCODE_AVAILABLE: u32 = 5;
/// Shell to host: one exact private mailbox sample was occupied and offered to the page.
const OPCODE_PRESENTED: u32 = 6;
/// Shell to host: the page released that exact sample after rejection or final GPU use.
const OPCODE_COMPLETED: u32 = 7;
/// Shell to host: all page samples and the private mirror are released.
const OPCODE_RETIRED: u32 = 8;

/// Why this shell could not produce a texture for an admitted allocation. The
/// codes describe what happened here, not what the host should do about it.
pub const FAILED_NOT_ADMITTED: u32 = 1;
pub const FAILED_UNSUPPORTED_DESCRIPTOR: u32 = 2;
pub const FAILED_IMPORT: u32 = 3;
/// The descriptor is importable but its row pitch or size does not match what
/// this device requires for a linear image of the described extent. The
/// accompanying `stride` and `size` are the ones that would be accepted; the
/// host may describe a fresh allocation using them. This is the only reply that
/// carries a layout, and it exists because the required pitch is a property of
/// the importing driver that the exporting side cannot compute.
pub const FAILED_LAYOUT: u32 = 4;

/// The only modifier this shell imports. The host exports a linear CUDA range,
/// and a linear-tiled image is the only layout both sides agree on without a
/// modifier negotiation neither side needs.
pub const MODIFIER_LINEAR: u64 = 0;

/// Descriptor positions for the two record kinds that carry `SCM_RIGHTS`:
/// `Import` carries memory, its edge, and a read-only frame-identity mapping;
/// `Ready` carries the timeline.
///
/// The frame edge is an eventfd the host signals once per completed mirror of
/// the admitted allocation and never reads or waits on. This shell watches it
/// and blits the allocation into the texture the page samples; an eventfd read
/// returns the number of signals since the last read, so a backlog coalesces
/// into one blit.
const IMPORT_MEMORY_DESCRIPTOR: usize = 0;
const IMPORT_FRAME_EDGE_DESCRIPTOR: usize = 1;
const IMPORT_FRAME_SIGNAL_DESCRIPTOR: usize = 2;
const IMPORT_DESCRIPTOR_COUNT: usize = 3;
const READY_TIMELINE_DESCRIPTOR: usize = 0;
const READY_DESCRIPTOR_COUNT: usize = 1;

/// An `Import`'s descriptors are the most any record carries.
const CONTROL_BYTES: usize = 32;
const PENDING_RECORD_CAPACITY: usize = 256;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Record {
    abi_version: u32,
    opcode: u32,
    id_high: u64,
    id_low: u64,
    width: u32,
    height: u32,
    stride: u64,
    size: u64,
    modifier: u64,
    code: u32,
    /// How many descriptors accompany this record. A record whose ancillary
    /// data does not carry exactly this many is a framing violation, which is
    /// what keeps the two independent declarations of this ABI honest about the
    /// one message that carries more than a payload.
    descriptors: u32,
    /// Exact generation-scoped arena publication copied into this mailbox
    /// sample. Meaningful only for mailbox lifecycle records.
    presentation_revision: u64,
}

const RECORD_BYTES: usize = mem::size_of::<Record>();

const _: () = assert!(RECORD_BYTES == 72);
const _: () = assert!(mem::align_of::<Record>() == 8);
const _: () = assert!(mem::offset_of!(Record, abi_version) == 0);
const _: () = assert!(mem::offset_of!(Record, opcode) == 4);
const _: () = assert!(mem::offset_of!(Record, id_high) == 8);
const _: () = assert!(mem::offset_of!(Record, id_low) == 16);
const _: () = assert!(mem::offset_of!(Record, width) == 24);
const _: () = assert!(mem::offset_of!(Record, height) == 28);
const _: () = assert!(mem::offset_of!(Record, stride) == 32);
const _: () = assert!(mem::offset_of!(Record, size) == 40);
const _: () = assert!(mem::offset_of!(Record, modifier) == 48);
const _: () = assert!(mem::offset_of!(Record, code) == 56);
const _: () = assert!(mem::offset_of!(Record, descriptors) == 60);
const _: () = assert!(mem::offset_of!(Record, presentation_revision) == 64);
const _: () = assert!(IMPORT_MEMORY_DESCRIPTOR < IMPORT_DESCRIPTOR_COUNT);
const _: () = assert!(IMPORT_FRAME_EDGE_DESCRIPTOR < IMPORT_DESCRIPTOR_COUNT);
const _: () = assert!(IMPORT_FRAME_SIGNAL_DESCRIPTOR < IMPORT_DESCRIPTOR_COUNT);
const _: () = assert!(IMPORT_MEMORY_DESCRIPTOR != IMPORT_FRAME_EDGE_DESCRIPTOR);
const _: () = assert!(IMPORT_MEMORY_DESCRIPTOR != IMPORT_FRAME_SIGNAL_DESCRIPTOR);
const _: () = assert!(IMPORT_FRAME_EDGE_DESCRIPTOR != IMPORT_FRAME_SIGNAL_DESCRIPTOR);
const _: () = assert!(READY_TIMELINE_DESCRIPTOR < READY_DESCRIPTOR_COUNT);

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct SurfaceId {
    pub high: u64,
    pub low: u64,
}

pub enum LiveRelease {
    AwaitingDrop,
    Withdrawn,
    NotLive,
}

impl SurfaceId {
    fn valid(self) -> bool {
        self.high != 0 || self.low != 0
    }

    pub fn parse(value: &str) -> Option<Self> {
        if value.len() != 32 || !value.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return None;
        }
        let id = Self {
            high: u64::from_str_radix(&value[..16], 16).ok()?,
            low: u64::from_str_radix(&value[16..], 16).ok()?,
        };
        id.valid().then_some(id)
    }
}

impl fmt::Display for SurfaceId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{:016x}{:016x}", self.high, self.low)
    }
}

/// How many descriptors an opcode carries.
fn descriptor_count(opcode: u32) -> usize {
    match opcode {
        OPCODE_IMPORT => IMPORT_DESCRIPTOR_COUNT,
        OPCODE_READY => READY_DESCRIPTOR_COUNT,
        _ => 0,
    }
}

fn valid_import_shape(record: &Record) -> bool {
    let row_bytes = u64::from(record.width).checked_mul(4);
    let described = record.stride.checked_mul(u64::from(record.height));
    record.width != 0
        && record.height != 0
        && record.code == 0
        && record.modifier == MODIFIER_LINEAR
        && record.presentation_revision == 0
        && row_bytes.is_some_and(|bytes| record.stride >= bytes)
        && described.is_some_and(|bytes| record.size >= bytes)
}

struct PendingRecord {
    record: Record,
    descriptor: Option<OwnedFd>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ChannelTerminal {
    Open,
    OrderlyBridgeClose,
    PeerHup,
    ProtocolFailure,
}

#[derive(Clone, Copy)]
enum ChannelCloseObservation {
    Eof,
    Hup,
    Protocol,
}

/// An allocation the host has described but the page has not yet claimed.
pub struct Admission {
    pub width: u32,
    pub height: u32,
    pub stride: u64,
    pub size: u64,
    pub modifier: u64,
    memory: Option<OwnedFd>,
    frame_edge: Option<OwnedFd>,
    frame_signal: Option<OwnedFd>,
}

impl Admission {
    /// The descriptor backing this allocation, still owned here.
    pub fn descriptor(&self) -> RawFd {
        self.memory.as_ref().map_or(-1, AsRawFd::as_raw_fd)
    }

    /// Gives up ownership once an import has consumed the descriptor. Calling
    /// this without a consuming import leaks it, which is why the only caller
    /// is the success arm of that import.
    pub fn release_descriptor(&mut self) {
        if let Some(memory) = self.memory.take() {
            let _ = memory.into_raw_fd();
        }
    }

    /// Takes the frame edge. The caller owns it afterwards and is the only
    /// thing that ever reads it; dropping this admission without taking it
    /// closes it, which is what an import that produced no texture wants.
    pub fn take_frame_edge(&mut self) -> Option<OwnedFd> {
        self.frame_edge.take()
    }

    pub fn take_frame_signal(&mut self) -> Option<OwnedFd> {
        self.frame_signal.take()
    }
}

struct Channel {
    fd: RawFd,
    admitted: HashMap<SurfaceId, Admission>,
    seen: HashSet<SurfaceId>,
    claimed: HashSet<SurfaceId>,
    live: HashSet<SurfaceId>,
    /// Mirrors whose page texture retired after the dispatcher completed its
    /// destination copy while host Drop is still in flight. The dispatcher
    /// keeps a release-only GPU mirror for each identity in this set.
    released: HashSet<SurfaceId>,
    /// Host Drops that completed a released capability. The shell dispatcher
    /// drains these identities and destroys their release-only GPU mirrors.
    settled_releases: VecDeque<SurfaceId>,
    /// An unclaimed capability whose host Drop already produced the one native
    /// Failed terminal. One late page texture creation consumes this marker
    /// locally because its Prepare travelled on the independent page stream.
    settled_unclaimed: HashSet<SurfaceId>,
    replied: HashSet<SurfaceId>,
    withdrawn: HashSet<SurfaceId>,
    pending: VecDeque<PendingRecord>,
    dispatch_wake: Option<OwnedFd>,
    terminal: ChannelTerminal,
}

impl Channel {
    fn connect() -> Option<Self> {
        let path = std::env::var_os("MMLTK_WORKSPACE_IMPORT_SOCKET")?;
        let bytes = OsStr::as_bytes(path.as_os_str());
        let mut address: libc::sockaddr_un = unsafe { mem::zeroed() };
        if bytes.is_empty() || bytes.len() >= mem::size_of_val(&address.sun_path) {
            return None;
        }
        address.sun_family = libc::AF_UNIX as libc::sa_family_t;
        for (slot, byte) in address.sun_path.iter_mut().zip(bytes) {
            *slot = *byte as libc::c_char;
        }

        let fd =
            unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC, 0) };
        if fd < 0 {
            return None;
        }
        loop {
            let connected = unsafe {
                libc::connect(
                    fd,
                    &address as *const libc::sockaddr_un as *const libc::sockaddr,
                    mem::size_of::<libc::sockaddr_un>() as libc::socklen_t,
                )
            };
            if connected == 0 {
                break;
            }
            let error = std::io::Error::last_os_error().raw_os_error();
            if error == Some(libc::EISCONN) {
                break;
            }
            if error != Some(libc::EINTR) {
                unsafe { libc::close(fd) };
                return None;
            }
        }
        let flags = loop {
            let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
            if flags >= 0 {
                break flags;
            }
            if std::io::Error::last_os_error().raw_os_error() != Some(libc::EINTR) {
                unsafe { libc::close(fd) };
                return None;
            }
        };
        loop {
            if unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) } == 0 {
                break;
            }
            if std::io::Error::last_os_error().raw_os_error() != Some(libc::EINTR) {
                unsafe { libc::close(fd) };
                return None;
            }
        }
        Some(Self {
            fd,
            admitted: HashMap::new(),
            seen: HashSet::new(),
            claimed: HashSet::new(),
            live: HashSet::new(),
            released: HashSet::new(),
            settled_releases: VecDeque::new(),
            settled_unclaimed: HashSet::new(),
            replied: HashSet::new(),
            withdrawn: HashSet::new(),
            pending: VecDeque::new(),
            dispatch_wake: None,
            terminal: ChannelTerminal::Open,
        })
    }

    fn terminalize(&mut self, terminal: ChannelTerminal) {
        if self.terminal != ChannelTerminal::Open {
            return;
        }
        self.terminal = terminal;
        write_diagnostic(|line| {
            let terminal_name = match terminal {
                ChannelTerminal::Open => "open",
                ChannelTerminal::OrderlyBridgeClose => "orderly_bridge_close",
                ChannelTerminal::PeerHup => "peer_hup",
                ChannelTerminal::ProtocolFailure => "protocol_failure",
            };
            write!(line,
                "{{\"event\":\"firefox.workspace.channel_terminal\",\"terminal\":\"{terminal_name}\",\"admitted\":{},\"claimed\":{},\"live\":{},\"released\":{},\"withdrawn\":{}}}",
                self.admitted.len(),
                self.claimed.len(),
                self.live.len(),
                self.released.len(),
                self.withdrawn.len()
            )
        });
        self.pending.clear();
        self.admitted.clear();
        self.seen.clear();
        self.claimed.clear();
        self.live.clear();
        self.released.clear();
        self.settled_releases.clear();
        self.settled_unclaimed.clear();
        self.replied.clear();
        self.withdrawn.clear();
        unsafe { libc::shutdown(self.fd, libc::SHUT_RDWR) };
    }

    fn has_physical_obligations(&self) -> bool {
        !self.admitted.is_empty()
            || !self.claimed.is_empty()
            || !self.live.is_empty()
            || !self.released.is_empty()
            || !self.pending.is_empty()
            || !self.withdrawn.is_empty()
            || !self.replied.is_empty()
            || !self.settled_releases.is_empty()
            || !self.settled_unclaimed.is_empty()
    }

    fn observe_close(&mut self, observation: ChannelCloseObservation) {
        let terminal = match observation {
            ChannelCloseObservation::Protocol => ChannelTerminal::ProtocolFailure,
            ChannelCloseObservation::Hup => ChannelTerminal::PeerHup,
            ChannelCloseObservation::Eof if self.has_physical_obligations() => {
                ChannelTerminal::PeerHup
            }
            ChannelCloseObservation::Eof => ChannelTerminal::OrderlyBridgeClose,
        };
        self.terminalize(terminal);
    }

    #[track_caller]
    fn fail(&mut self) {
        if workspace_diagnostics_enabled() {
            let caller = std::panic::Location::caller();
            write_diagnostic(|line| write!(line,
                "{{\"event\":\"firefox.workspace.protocol_failure_source\",\"file\":\"{}\",\"line\":{},\"column\":{}}}",
                caller.file(),
                caller.line(),
                caller.column()
            ));
        }
        self.observe_close(ChannelCloseObservation::Protocol);
    }

    fn close_after_resource_shutdown(&mut self) {
        self.terminalize(ChannelTerminal::OrderlyBridgeClose);
    }

    fn service_ready(&mut self, revents: u32) {
        if revents & (libc::EPOLLERR | libc::EPOLLHUP) as u32 != 0 {
            self.observe_close(ChannelCloseObservation::Hup);
            return;
        }
        if revents & libc::EPOLLIN as u32 != 0 {
            self.drain();
        }
        if revents & libc::EPOLLOUT as u32 != 0 {
            self.flush();
        }
    }

    fn accept_new_capability(&mut self, id: SurfaceId) -> bool {
        if self.seen.insert(id) {
            return true;
        }
        self.fail();
        false
    }

    fn claim(&mut self, id: SurfaceId, width: u32, height: u32) -> Option<Admission> {
        // The dispatcher normally owns progress. This reconciliation handles the
        // narrow race where texture creation follows the host write before epoll
        // has scheduled the dispatcher, without making page creation the progress
        // mechanism.
        self.drain();
        let Some(admission) = self.admitted.get(&id) else {
            write_diagnostic(|line| write!(line,
                "{{\"event\":\"firefox.workspace.claim_outcome\",\"surface\":\"{id}\",\"outcome\":\"missing\",\"width\":{width},\"height\":{height},\"admitted\":{},\"claimed\":{},\"live\":{}}}",
                self.admitted.len(), self.claimed.len(), self.live.len()
            ));
            return None;
        };
        if admission.width != width || admission.height != height {
            write_diagnostic(|line| write!(line,
                "{{\"event\":\"firefox.workspace.claim_outcome\",\"surface\":\"{id}\",\"outcome\":\"mismatch\",\"width\":{width},\"height\":{height},\"admitted_width\":{},\"admitted_height\":{}}}",
                admission.width, admission.height
            ));
            return None;
        }
        let admission = self.admitted.remove(&id)?;
        self.claimed.insert(id);
        write_diagnostic(|line| write!(line,
            "{{\"event\":\"firefox.workspace.claim_outcome\",\"surface\":\"{id}\",\"outcome\":\"claimed\",\"width\":{width},\"height\":{height},\"admitted\":{},\"claimed\":{},\"live\":{}}}",
            self.admitted.len(), self.claimed.len(), self.live.len()
        ));
        Some(admission)
    }

    fn apply_drop(&mut self, id: SurfaceId) {
        if self.admitted.remove(&id).is_some() {
            trace_state("withdrawal", id, "before_claim");
            // An unclaimed import will never otherwise produce its one
            // outcome. Settle the host's withdrawal tombstone and release
            // both descriptors immediately.
            self.settled_unclaimed.insert(id);
            self.send(OPCODE_FAILED, id, FAILED_NOT_ADMITTED, 0, 0, 0, None);
        } else if self.claimed.contains(&id) || self.live.contains(&id) {
            trace_state("withdrawal", id, "claimed_or_live");
            self.withdrawn.insert(id);
        } else if self.released.remove(&id) {
            trace_state("withdrawal", id, "page_released");
            self.replied.remove(&id);
            self.withdrawn.insert(id);
            self.settled_releases.push_back(id);
            self.wake_dispatcher();
        } else if self.replied.remove(&id) {
            self.send(OPCODE_RETIRED, id, 0, 0, 0, 0, None);
        } else {
            self.fail();
        }
    }

    /// Reads every record the host has already written. Returns once the socket
    /// would block, which is the steady state.
    fn drain(&mut self) {
        while self.terminal == ChannelTerminal::Open {
            let mut record = Record::default();
            let mut payload = libc::iovec {
                iov_base: &mut record as *mut Record as *mut libc::c_void,
                iov_len: RECORD_BYTES,
            };
            let mut control = [0u8; CONTROL_BYTES];
            let mut message: libc::msghdr = unsafe { mem::zeroed() };
            message.msg_iov = &mut payload;
            message.msg_iovlen = 1;
            message.msg_control = control.as_mut_ptr() as *mut libc::c_void;
            message.msg_controllen = CONTROL_BYTES;

            let read = unsafe {
                libc::recvmsg(
                    self.fd,
                    &mut message,
                    libc::MSG_DONTWAIT | libc::MSG_CMSG_CLOEXEC,
                )
            };
            if read < 0 {
                match std::io::Error::last_os_error().raw_os_error() {
                    Some(libc::EINTR) => continue,
                    Some(libc::EAGAIN) => return,
                    _ => {
                        self.observe_close(ChannelCloseObservation::Hup);
                        return;
                    }
                }
            }
            if read == 0 {
                self.observe_close(ChannelCloseObservation::Eof);
                return;
            }
            let Some(descriptors) = take_descriptors(&message) else {
                self.fail();
                return;
            };
            let id = SurfaceId {
                high: record.id_high,
                low: record.id_low,
            };
            if read as usize != RECORD_BYTES
                || message.msg_flags & (libc::MSG_TRUNC | libc::MSG_CTRUNC) != 0
                || record.abi_version != ABI_VERSION
                || record.descriptors as usize != descriptor_count(record.opcode)
                || descriptors.len() != record.descriptors as usize
                || !id.valid()
            {
                self.fail();
                return;
            }
            match record.opcode {
                OPCODE_IMPORT => {
                    if !valid_import_shape(&record) {
                        self.fail();
                        return;
                    }
                    let mut memory = None;
                    let mut frame_edge = None;
                    let mut frame_signal = None;
                    for (index, descriptor) in descriptors.into_iter().enumerate() {
                        match index {
                            IMPORT_MEMORY_DESCRIPTOR => memory = Some(descriptor),
                            IMPORT_FRAME_EDGE_DESCRIPTOR => frame_edge = Some(descriptor),
                            IMPORT_FRAME_SIGNAL_DESCRIPTOR => frame_signal = Some(descriptor),
                            _ => {}
                        }
                    }
                    let (Some(memory), Some(frame_edge), Some(frame_signal)) =
                        (memory, frame_edge, frame_signal)
                    else {
                        self.fail();
                        return;
                    };
                    if !self.accept_new_capability(id) {
                        return;
                    }
                    self.admitted.insert(
                        id,
                        Admission {
                            width: record.width,
                            height: record.height,
                            stride: record.stride,
                            size: record.size,
                            modifier: record.modifier,
                            memory: Some(memory),
                            frame_edge: Some(frame_edge),
                            frame_signal: Some(frame_signal),
                        },
                    );
                    write_diagnostic(|line| write!(line,
                        "{{\"event\":\"firefox.workspace.admitted\",\"surface\":\"{id}\",\"width\":{},\"height\":{},\"admitted\":{},\"claimed\":{},\"live\":{}}}",
                        record.width, record.height, self.admitted.len(), self.claimed.len(), self.live.len()
                    ));
                }
                OPCODE_DROP => {
                    trace_state("drop_received", id, "native_withdrawal");
                    if record.width != 0
                        || record.height != 0
                        || record.stride != 0
                        || record.size != 0
                        || record.modifier != MODIFIER_LINEAR
                        || record.code != 0
                        || record.presentation_revision != 0
                        || !self.seen.contains(&id)
                        || self.withdrawn.contains(&id)
                    {
                        self.fail();
                        return;
                    }
                    self.apply_drop(id);
                }
                _ => {
                    self.fail();
                    return;
                }
            }
        }
    }

    fn send(
        &mut self,
        opcode: u32,
        id: SurfaceId,
        code: u32,
        stride: u64,
        size: u64,
        presentation_revision: u64,
        descriptor: Option<OwnedFd>,
    ) {
        if self.terminal != ChannelTerminal::Open {
            return;
        }
        if self.pending.len() >= PENDING_RECORD_CAPACITY {
            self.fail();
            return;
        }
        let descriptors = usize::from(descriptor.is_some());
        if descriptors != descriptor_count(opcode) {
            self.fail();
            return;
        }
        let record = Record {
            abi_version: ABI_VERSION,
            opcode,
            id_high: id.high,
            id_low: id.low,
            code,
            stride,
            size,
            presentation_revision,
            descriptors: descriptors as u32,
            ..Record::default()
        };
        self.pending.push_back(PendingRecord { record, descriptor });
        self.flush();
        if !self.pending.is_empty() {
            self.wake_dispatcher();
        }
    }

    fn send_outcome(
        &mut self,
        opcode: u32,
        id: SurfaceId,
        code: u32,
        stride: u64,
        size: u64,
        descriptor: Option<OwnedFd>,
    ) {
        let was_claimed = self.claimed.remove(&id);
        let was_admitted = self.admitted.remove(&id).is_some();
        if !was_claimed && !was_admitted {
            if opcode == OPCODE_FAILED
                && code == FAILED_NOT_ADMITTED
                && self.settled_unclaimed.remove(&id)
            {
                return;
            }
            // An arbitrary page label has no channel identity. A second local
            // terminal for an identity the channel does know is an internal
            // protocol violation and closes the boundary.
            if self.seen.contains(&id) {
                self.fail();
            }
            return;
        }
        if opcode == OPCODE_READY {
            if !was_claimed || was_admitted || !self.live.insert(id) {
                self.fail();
                return;
            }
        } else if opcode != OPCODE_FAILED {
            self.fail();
            return;
        }
        self.send(opcode, id, code, stride, size, 0, descriptor);
        if opcode == OPCODE_READY {
            if !self.withdrawn.contains(&id) {
                self.replied.insert(id);
            }
        } else {
            if !self.withdrawn.remove(&id) {
                self.replied.insert(id);
            }
        }
    }

    fn release_live(&mut self, id: SurfaceId) -> LiveRelease {
        if self.terminal != ChannelTerminal::Open {
            return LiveRelease::NotLive;
        }
        if !self.live.remove(&id) {
            // The mirror is registered before create_texture_from_hal can
            // finish. Its rollback drops the mirror while the capability is
            // still claimed; the caller immediately publishes the one Failed
            // terminal that consumes this claim.
            if self.claimed.contains(&id) {
                return LiveRelease::NotLive;
            }
            if self.seen.contains(&id) {
                self.fail();
            }
            return LiveRelease::NotLive;
        }
        if self.withdrawn.contains(&id) {
            LiveRelease::Withdrawn
        } else {
            // Page texture ownership may end as soon as a replacement is
            // promoted. The dispatcher has replaced its destination copy with
            // a release-only command buffer, which continues returning every
            // later odd value until the host's Drop closes the capability.
            self.released.insert(id);
            LiveRelease::AwaitingDrop
        }
    }

    fn take_settled_releases(&mut self) -> Vec<SurfaceId> {
        self.settled_releases.drain(..).collect()
    }

    fn complete_retirement(&mut self, id: SurfaceId) {
        if self.terminal != ChannelTerminal::Open
            || !self.withdrawn.remove(&id)
            || self.live.contains(&id)
            || self.released.contains(&id)
        {
            self.fail();
            return;
        }
        self.replied.remove(&id);
        trace_state("retired", id, "resources_released");
        self.send(OPCODE_RETIRED, id, 0, 0, 0, 0, None);
    }

    fn wake_dispatcher(&self) {
        let Some(wake) = self.dispatch_wake.as_ref() else {
            return;
        };
        let edge: u64 = 1;
        loop {
            let written = unsafe {
                libc::write(
                    wake.as_raw_fd(),
                    &edge as *const u64 as *const libc::c_void,
                    mem::size_of::<u64>(),
                )
            };
            if written >= 0 || std::io::Error::last_os_error().raw_os_error() != Some(libc::EINTR) {
                return;
            }
        }
    }

    fn flush(&mut self) {
        while self.terminal == ChannelTerminal::Open {
            let Some(pending) = self.pending.front() else {
                return;
            };
            let mut payload = libc::iovec {
                iov_base: &pending.record as *const Record as *mut libc::c_void,
                iov_len: RECORD_BYTES,
            };
            let mut control = [0u8; CONTROL_BYTES];
            let mut message: libc::msghdr = unsafe { mem::zeroed() };
            message.msg_iov = &mut payload;
            message.msg_iovlen = 1;
            if let Some(descriptor) = pending.descriptor.as_ref() {
                message.msg_control = control.as_mut_ptr() as *mut libc::c_void;
                message.msg_controllen =
                    unsafe { libc::CMSG_SPACE(mem::size_of::<RawFd>() as _) } as usize;
                let header = unsafe { libc::CMSG_FIRSTHDR(&message) };
                if header.is_null() {
                    self.fail();
                    return;
                }
                unsafe {
                    (*header).cmsg_level = libc::SOL_SOCKET;
                    (*header).cmsg_type = libc::SCM_RIGHTS;
                    (*header).cmsg_len = libc::CMSG_LEN(mem::size_of::<RawFd>() as _) as usize;
                    (libc::CMSG_DATA(header) as *mut RawFd).write_unaligned(descriptor.as_raw_fd());
                }
            }
            let sent = unsafe { libc::sendmsg(self.fd, &message, libc::MSG_NOSIGNAL) };
            if sent < 0 {
                match std::io::Error::last_os_error().raw_os_error() {
                    Some(libc::EINTR) => continue,
                    Some(libc::EAGAIN) => return,
                    _ => {
                        self.observe_close(ChannelCloseObservation::Hup);
                        return;
                    }
                }
            }
            if sent as usize != RECORD_BYTES {
                self.fail();
                return;
            }
            self.pending.pop_front();
        }
    }
}

/// Claims every descriptor a record carries, in the order the host wrote them.
/// Ownership is taken unconditionally so that a record with the wrong shape
/// closes what it brought instead of leaking it; the caller rejects the record
/// by comparing the count against the one the record declares.
fn take_descriptors(message: &libc::msghdr) -> Option<Vec<OwnedFd>> {
    let mut claimed = Vec::new();
    let mut valid = true;
    let mut header = unsafe { libc::CMSG_FIRSTHDR(message) };
    while !header.is_null() {
        let level = unsafe { (*header).cmsg_level };
        let kind = unsafe { (*header).cmsg_type };
        let base = unsafe { libc::CMSG_LEN(0) } as usize;
        let length = unsafe { (*header).cmsg_len };
        if level != libc::SOL_SOCKET || kind != libc::SCM_RIGHTS {
            valid = false;
            header = unsafe { libc::CMSG_NXTHDR(message, header) };
            continue;
        }
        if length < base {
            valid = false;
            header = unsafe { libc::CMSG_NXTHDR(message, header) };
            continue;
        }
        valid &= (length - base) % mem::size_of::<RawFd>() == 0;
        let count = (length - base) / mem::size_of::<RawFd>();
        for index in 0..count {
            let raw = unsafe {
                (libc::CMSG_DATA(header) as *const RawFd)
                    .add(index)
                    .read_unaligned()
            };
            claimed.push(unsafe { OwnedFd::from_raw_fd(raw) });
        }
        header = unsafe { libc::CMSG_NXTHDR(message, header) };
    }
    valid.then_some(claimed)
}

impl Drop for Channel {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd) };
    }
}

fn channel() -> Option<&'static Mutex<Channel>> {
    static CHANNEL: OnceLock<Option<Mutex<Channel>>> = OnceLock::new();
    CHANNEL
        .get_or_init(|| Channel::connect().map(Mutex::new))
        .as_ref()
}

/// Gives the bridge dispatcher the eventfd that wakes it when a nonblocking
/// response remains queued after an immediate send attempt.
pub fn install_dispatch_wake(wake: OwnedFd) {
    if let Some(channel) = channel() {
        if let Ok(mut channel) = channel.lock() {
            channel.dispatch_wake = Some(wake);
        }
    }
}

/// The import socket and the epoll events currently needed for progress.
pub fn poll_state() -> Option<(RawFd, u32)> {
    let channel = channel()?;
    let channel = channel.lock().ok()?;
    if channel.terminal != ChannelTerminal::Open {
        return None;
    }
    let mut events = libc::EPOLLIN as u32;
    if !channel.pending.is_empty() {
        events |= libc::EPOLLOUT as u32;
    }
    Some((channel.fd, events))
}

/// Services readiness reported by the single bridge dispatcher.
pub fn service(revents: u32) {
    let Some(channel) = channel() else {
        return;
    };
    let Ok(mut channel) = channel.lock() else {
        return;
    };
    channel.service_ready(revents);
}

/// Makes native bridge failure observable without involving the page stream.
/// Shutting down the socket wakes the host, which owns browser replacement and
/// peer-loss timeline rescue.
#[track_caller]
pub fn fail() {
    let Some(channel) = channel() else {
        return;
    };
    let Ok(mut channel) = channel.lock() else {
        return;
    };
    channel.fail();
}

/// Closes the import boundary after its owning WebGPU server has synchronously
/// retired every imported GPU resource. This is a local lifecycle completion,
/// not evidence of a malformed record or failed transfer.
pub fn close_after_resource_shutdown() {
    let Some(channel) = channel() else {
        return;
    };
    let Ok(mut channel) = channel.lock() else {
        return;
    };
    channel.close_after_resource_shutdown();
}

/// Takes the allocation the dispatcher admitted under `id`.
/// The extents the host described are authoritative: a page asking for anything
/// else is not describing an admitted allocation.
pub fn take_admission(id: SurfaceId, width: u32, height: u32) -> Option<Admission> {
    let channel = channel()?;
    let mut channel = channel.lock().ok()?;
    channel.claim(id, width, height)
}

pub fn send_ready(id: SurfaceId, timeline: OwnedFd) {
    if let Some(channel) = channel() {
        if let Ok(mut channel) = channel.lock() {
            trace_state("ready", id, "vulkan_import_complete");
            channel.send_outcome(OPCODE_READY, id, 0, 0, 0, Some(timeline));
        }
    }
}

/// Reports why `id` produced no texture. `stride` and `size` are meaningful
/// only for [`FAILED_LAYOUT`], where they carry the layout this device would
/// accept for the same extent.
pub fn send_failed(id: SurfaceId, code: u32, stride: u64, size: u64) {
    if let Some(channel) = channel() {
        if let Ok(mut channel) = channel.lock() {
            write_diagnostic(|line| write!(line,
                "{{\"event\":\"firefox.workspace.import_failed\",\"surface\":\"{id}\",\"code\":{code},\"required_stride\":{stride},\"required_size\":{size}}}"
            ));
            channel.send_outcome(OPCODE_FAILED, id, code, stride, size, None);
        }
    }
}

pub fn send_mailbox_available(
    id: SurfaceId,
    layer: u32,
    slot: u32,
    content_session: u64,
    content_sequence: u64,
    presentation_revision: u64,
) {
    if layer >= 3
        || slot >= 2
        || (content_session == 0 && content_sequence == 0)
        || presentation_revision == 0
    {
        return;
    }
    if let Some(channel) = channel() {
        if let Ok(mut channel) = channel.lock() {
            if channel.live.contains(&id) && !channel.withdrawn.contains(&id) {
                channel.send(
                    OPCODE_AVAILABLE,
                    id,
                    layer * 2 + slot + 1,
                    content_session,
                    content_sequence,
                    presentation_revision,
                    None,
                );
            }
        }
    }
}

fn send_mailbox_lifecycle(
    opcode: u32,
    id: SurfaceId,
    layer: u32,
    slot: u32,
    content_session: u64,
    content_sequence: u64,
    presentation_revision: u64,
) {
    if layer >= 3
        || slot >= 2
        || (content_session == 0 && content_sequence == 0)
        || presentation_revision == 0
    {
        return;
    }
    if let Some(channel) = channel() {
        if let Ok(mut channel) = channel.lock() {
            if channel.live.contains(&id)
                && (opcode == OPCODE_COMPLETED || !channel.withdrawn.contains(&id))
            {
                channel.send(
                    opcode,
                    id,
                    layer * 2 + slot + 1,
                    content_session,
                    content_sequence,
                    presentation_revision,
                    None,
                );
            }
        }
    }
}

pub fn send_mailbox_presented(
    id: SurfaceId,
    layer: u32,
    slot: u32,
    content_session: u64,
    content_sequence: u64,
    presentation_revision: u64,
) {
    send_mailbox_lifecycle(
        OPCODE_PRESENTED,
        id,
        layer,
        slot,
        content_session,
        content_sequence,
        presentation_revision,
    );
}

pub fn send_mailbox_completed(
    id: SurfaceId,
    layer: u32,
    slot: u32,
    content_session: u64,
    content_sequence: u64,
    presentation_revision: u64,
) {
    send_mailbox_lifecycle(
        OPCODE_COMPLETED,
        id,
        layer,
        slot,
        content_session,
        content_sequence,
        presentation_revision,
    );
}

/// Releases the capability only after its private texture mirror has stopped
/// and its final queue submission is complete.
pub fn release_live(id: SurfaceId) -> LiveRelease {
    if let Some(channel) = channel() {
        if let Ok(mut channel) = channel.lock() {
            return channel.release_live(id);
        }
    }
    LiveRelease::NotLive
}

/// Returns capabilities whose host Drop now permits the dispatcher's
/// release-only Vulkan resources to be destroyed.
pub fn take_settled_releases() -> Vec<SurfaceId> {
    if let Some(channel) = channel() {
        if let Ok(mut channel) = channel.lock() {
            return channel.take_settled_releases();
        }
    }
    Vec::new()
}

pub fn complete_retirement(id: SurfaceId) {
    if let Some(channel) = channel() {
        if let Ok(mut channel) = channel.lock() {
            channel.complete_retirement(id);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn diagnostics_require_a_non_empty_trace_path() {
        assert!(!diagnostics_enabled_for(None));
        assert!(!diagnostics_enabled_for(Some(OsStr::new(""))));
        assert!(diagnostics_enabled_for(Some(OsStr::new(
            ".mmltk-data/logs/gui-trace.jsonl"
        ))));
    }

    fn surface_id() -> SurfaceId {
        SurfaceId { high: 1, low: 2 }
    }

    fn test_channel() -> (Channel, OwnedFd) {
        let mut descriptors = [-1; 2];
        assert_eq!(
            unsafe {
                libc::socketpair(
                    libc::AF_UNIX,
                    libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK,
                    0,
                    descriptors.as_mut_ptr(),
                )
            },
            0
        );
        let peer = unsafe { OwnedFd::from_raw_fd(descriptors[1]) };
        (
            Channel {
                fd: descriptors[0],
                admitted: HashMap::new(),
                seen: HashSet::new(),
                claimed: HashSet::new(),
                live: HashSet::new(),
                released: HashSet::new(),
                settled_releases: VecDeque::new(),
                settled_unclaimed: HashSet::new(),
                replied: HashSet::new(),
                withdrawn: HashSet::new(),
                pending: VecDeque::new(),
                dispatch_wake: None,
                terminal: ChannelTerminal::Open,
            },
            peer,
        )
    }

    fn event_descriptor() -> OwnedFd {
        let descriptor = unsafe { libc::eventfd(0, libc::EFD_CLOEXEC | libc::EFD_NONBLOCK) };
        assert!(descriptor >= 0);
        unsafe { OwnedFd::from_raw_fd(descriptor) }
    }

    fn send_record(peer: RawFd, record: &Record) {
        assert_eq!(
            unsafe {
                libc::send(
                    peer,
                    record as *const Record as *const libc::c_void,
                    RECORD_BYTES,
                    0,
                )
            },
            RECORD_BYTES as isize
        );
    }

    fn receive_record(peer: RawFd) -> Option<Record> {
        let mut record = Record::default();
        let received = unsafe {
            libc::recv(
                peer,
                &mut record as *mut Record as *mut libc::c_void,
                RECORD_BYTES,
                0,
            )
        };
        (received == RECORD_BYTES as isize).then_some(record)
    }

    #[test]
    fn mailbox_lifecycle_carries_the_exact_sample_identity() {
        let (mut channel, peer) = test_channel();
        let id = surface_id();
        channel.live.insert(id);
        channel.send(OPCODE_AVAILABLE, id, 6, 41, 73, 81, None);
        assert_eq!(channel.terminal, ChannelTerminal::Open);
        let record = receive_record(peer.as_raw_fd()).expect("host receives mailbox availability");
        assert_eq!(record.abi_version, ABI_VERSION);
        assert_eq!(record.opcode, OPCODE_AVAILABLE);
        assert_eq!(record.code, 6);
        assert_eq!(record.stride, 41);
        assert_eq!(record.size, 73);
        assert_eq!(record.presentation_revision, 81);
        assert_eq!(record.descriptors, 0);

        channel.send(OPCODE_PRESENTED, id, 5, 42, 74, 82, None);
        let presented =
            receive_record(peer.as_raw_fd()).expect("host receives mailbox presentation");
        assert_eq!(presented.opcode, OPCODE_PRESENTED);
        assert_eq!(presented.code, 5);
        assert_eq!(presented.stride, 42);
        assert_eq!(presented.size, 74);
        assert_eq!(presented.presentation_revision, 82);

        channel.send(OPCODE_COMPLETED, id, 5, 42, 74, 82, None);
        let completed = receive_record(peer.as_raw_fd()).expect("host receives mailbox completion");
        assert_eq!(completed.opcode, OPCODE_COMPLETED);
        assert_eq!(completed.code, 5);
        assert_eq!(completed.stride, 42);
        assert_eq!(completed.size, 74);
        assert_eq!(completed.presentation_revision, 82);
    }

    #[test]
    fn drop_then_ready_retires_the_live_capability() {
        let (mut channel, peer) = test_channel();
        let id = surface_id();
        channel.seen.insert(id);
        channel.claimed.insert(id);
        channel.withdrawn.insert(id);

        channel.send_outcome(OPCODE_READY, id, 0, 0, 0, Some(event_descriptor()));
        assert_eq!(channel.terminal, ChannelTerminal::Open);
        assert!(channel.live.contains(&id));
        assert!(channel.withdrawn.contains(&id));

        channel.release_live(id);
        assert_eq!(channel.terminal, ChannelTerminal::Open);
        assert!(!channel.live.contains(&id));
        assert!(channel.withdrawn.contains(&id));
        assert!(channel.seen.contains(&id));
        channel.complete_retirement(id);
        assert!(!channel.withdrawn.contains(&id));
        assert_eq!(
            receive_record(peer.as_raw_fd()).unwrap().opcode,
            OPCODE_RETIRED
        );
    }

    #[test]
    fn live_mirror_can_release_before_host_drop() {
        let (mut channel, _peer) = test_channel();
        let id = surface_id();
        channel.seen.insert(id);
        channel.live.insert(id);
        channel.replied.insert(id);

        channel.release_live(id);
        assert_eq!(channel.terminal, ChannelTerminal::Open);
        assert!(!channel.live.contains(&id));
        assert!(channel.released.contains(&id));

        channel.apply_drop(id);
        assert_eq!(channel.terminal, ChannelTerminal::Open);
        assert!(!channel.released.contains(&id));
        assert!(!channel.replied.contains(&id));
        assert!(channel.seen.contains(&id));
        assert_eq!(channel.take_settled_releases(), vec![id]);
        channel.complete_retirement(id);
        assert_eq!(channel.terminal, ChannelTerminal::Open);
    }

    #[test]
    fn physical_channel_classifies_readiness_and_protocol_terminals() {
        let (mut orderly, peer) = test_channel();
        drop(peer);
        orderly.service_ready(libc::EPOLLIN as u32);
        assert_eq!(
            orderly.terminal,
            ChannelTerminal::OrderlyBridgeClose
        );

        let (mut live_eof, peer) = test_channel();
        let id = surface_id();
        live_eof.live.insert(id);
        drop(peer);
        live_eof.service_ready(libc::EPOLLIN as u32);
        assert_eq!(
            live_eof.terminal,
            ChannelTerminal::PeerHup
        );
        assert!(live_eof.live.is_empty());

        let (mut lost, _peer) = test_channel();
        lost.live.insert(id);
        lost.service_ready((libc::EPOLLIN | libc::EPOLLHUP) as u32);
        assert_eq!(lost.terminal, ChannelTerminal::PeerHup);
        assert!(lost.live.is_empty());

        let (mut invalid, peer) = test_channel();
        send_record(
            peer.as_raw_fd(),
            &Record {
                abi_version: ABI_VERSION + 1,
                opcode: OPCODE_DROP,
                id_high: id.high,
                id_low: id.low,
                ..Record::default()
            },
        );
        invalid.service_ready(libc::EPOLLIN as u32);
        assert_eq!(
            invalid.terminal,
            ChannelTerminal::ProtocolFailure
        );
    }

    #[test]
    fn drop_consumed_by_page_reconciliation_wakes_release_cleanup() {
        let (mut channel, peer) = test_channel();
        let wake = event_descriptor();
        let wake_fd = wake.as_raw_fd();
        channel.dispatch_wake = Some(wake);
        let id = surface_id();
        channel.seen.insert(id);
        channel.released.insert(id);
        channel.replied.insert(id);
        send_record(
            peer.as_raw_fd(),
            &Record {
                abi_version: ABI_VERSION,
                opcode: OPCODE_DROP,
                id_high: id.high,
                id_low: id.low,
                ..Record::default()
            },
        );

        channel.drain();
        assert_eq!(channel.terminal, ChannelTerminal::Open);
        assert_eq!(channel.take_settled_releases(), vec![id]);
        let mut edge = 0u64;
        assert_eq!(
            unsafe {
                libc::read(
                    wake_fd,
                    &mut edge as *mut u64 as *mut libc::c_void,
                    mem::size_of::<u64>(),
                )
            },
            mem::size_of::<u64>() as isize
        );
        assert_eq!(edge, 1);
    }

    #[test]
    fn claimed_mirror_rollback_publishes_one_failed_terminal() {
        let (mut channel, _peer) = test_channel();
        let id = surface_id();
        channel.seen.insert(id);
        channel.claimed.insert(id);

        channel.release_live(id);
        assert_eq!(channel.terminal, ChannelTerminal::Open);
        assert!(channel.claimed.contains(&id));
        channel.send_outcome(OPCODE_FAILED, id, FAILED_IMPORT, 0, 0, None);
        assert_eq!(channel.terminal, ChannelTerminal::Open);
        assert!(!channel.claimed.contains(&id));
        assert!(channel.replied.contains(&id));

        channel.send_outcome(OPCODE_FAILED, id, FAILED_IMPORT, 0, 0, None);
        assert_eq!(channel.terminal, ChannelTerminal::ProtocolFailure);
    }

    #[test]
    fn one_live_texture_claim_survives_host_withdrawal_until_its_owner_releases() {
        let (mut channel, peer) = test_channel();
        let id = surface_id();
        channel.seen.insert(id);
        channel.admitted.insert(id, Admission {
            width: 64, height: 32, stride: 256, size: 8192, modifier: MODIFIER_LINEAR,
            memory: Some(event_descriptor()), frame_edge: Some(event_descriptor()),
            frame_signal: Some(event_descriptor()),
        });
        let owner = channel.claim(id, 64, 32).expect("the durable page owner claims once");
        assert!(channel.claim(id, 64, 32).is_none());
        assert!(channel.claimed.contains(&id));
        channel.send_outcome(OPCODE_READY, id, 0, 0, 0, Some(event_descriptor()));
        assert_eq!(receive_record(peer.as_raw_fd()).unwrap().opcode, OPCODE_READY);
        channel.apply_drop(id);
        assert!(channel.live.contains(&id));
        assert!(channel.withdrawn.contains(&id));
        assert!(receive_record(peer.as_raw_fd()).is_none());
        drop(owner);
        assert!(matches!(channel.release_live(id), LiveRelease::Withdrawn));
        channel.complete_retirement(id);
        assert_eq!(receive_record(peer.as_raw_fd()).unwrap().opcode, OPCODE_RETIRED);
    }

    #[test]
    fn host_drop_before_page_claim_settles_one_late_local_failure() {
        let (mut channel, peer) = test_channel();
        let id = surface_id();
        channel.seen.insert(id);
        channel.admitted.insert(
            id,
            Admission {
                width: 64,
                height: 32,
                stride: 256,
                size: 8192,
                modifier: MODIFIER_LINEAR,
                memory: Some(event_descriptor()),
                frame_edge: Some(event_descriptor()),
                frame_signal: Some(event_descriptor()),
            },
        );

        channel.apply_drop(id);
        assert_eq!(channel.terminal, ChannelTerminal::Open);
        assert!(channel.settled_unclaimed.contains(&id));
        let native_terminal =
            receive_record(peer.as_raw_fd()).expect("host receives one Failed terminal");
        assert_eq!(native_terminal.opcode, OPCODE_FAILED);
        assert_eq!(native_terminal.code, FAILED_NOT_ADMITTED);

        channel.send_outcome(OPCODE_FAILED, id, FAILED_NOT_ADMITTED, 0, 0, None);
        assert_eq!(channel.terminal, ChannelTerminal::Open);
        assert!(!channel.settled_unclaimed.contains(&id));
        assert!(receive_record(peer.as_raw_fd()).is_none());

        channel.send_outcome(OPCODE_FAILED, id, FAILED_NOT_ADMITTED, 0, 0, None);
        assert_eq!(channel.terminal, ChannelTerminal::ProtocolFailure);
    }

    #[test]
    fn retired_capability_replay_is_terminal() {
        let (mut channel, _peer) = test_channel();
        let id = surface_id();
        channel.seen.insert(id);

        assert!(!channel.accept_new_capability(id));
        assert_eq!(channel.terminal, ChannelTerminal::ProtocolFailure);
    }
}
