// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{
    cell::{RefCell, RefMut},
    fmt::{self, Display, Formatter},
    num::NonZeroUsize,
    path::PathBuf,
    rc::Rc,
    time::Instant,
};

use neqo_common::{Datagram, qtrace};
use neqo_transport::{
    ConnectionIdGenerator, Output, OutputBatch,
    server::{ConnectionRef, Server, ValidateAddress},
};
use nss::{AntiReplay, Cipher, PrivateKey, PublicKey, ZeroRttChecker};
use rustc_hash::FxHashMap as HashMap;

use crate::{
    Http3Parameters, Http3StreamInfo, Res,
    connection::Http3State,
    connection_server::Http3ServerHandler,
    server_connection_events::{ConnectUdpEvent, Http3ServerConnEvent, WebTransportEvent},
    server_events::{
        ConnectUdpRequest, Http3OrWebTransportStream, Http3ServerEvent, Http3ServerEvents,
        WebTransportRequest,
    },
    settings::HttpZeroRttChecker,
};

type HandlerRef = Rc<RefCell<Http3ServerHandler>>;

const MAX_EVENT_DATA_SIZE: usize = 1024;

pub struct Http3Server {
    server: Server,
    http3_parameters: Http3Parameters,
    http3_handlers: HashMap<ConnectionRef, HandlerRef>,
    events: Http3ServerEvents,
}

impl Display for Http3Server {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "Http3 server ")
    }
}

impl Http3Server {
    /// # Errors
    ///
    /// Making a `neqo_transport::Server` may produce an error. This can only be a crypto error if
    /// the socket can't be created or configured.
    pub fn new<A: AsRef<str>, A1: AsRef<str>>(
        now: Instant,
        certs: &[A],
        protocols: &[A1],
        anti_replay: AntiReplay,
        cid_manager: Rc<RefCell<dyn ConnectionIdGenerator>>,
        http3_parameters: Http3Parameters,
        zero_rtt_checker: Option<Box<dyn ZeroRttChecker>>,
    ) -> Res<Self> {
        Ok(Self {
            server: Server::new(
                now,
                certs,
                protocols,
                anti_replay,
                zero_rtt_checker
                    .unwrap_or_else(|| Box::new(HttpZeroRttChecker::new(http3_parameters.clone()))),
                cid_manager,
                http3_parameters.get_connection_parameters().clone(),
            )?,
            http3_parameters,
            http3_handlers: HashMap::default(),
            events: Http3ServerEvents::default(),
        })
    }

    pub fn set_qlog_dir(&mut self, dir: Option<PathBuf>) {
        self.server.set_qlog_dir(dir);
    }

    pub fn set_validation(&self, v: ValidateAddress) {
        self.server.set_validation(v);
    }

    pub fn set_ciphers<A: AsRef<[Cipher]>>(&mut self, ciphers: A) {
        self.server.set_ciphers(ciphers);
    }

    /// Enable encrypted client hello (ECH).
    ///
    /// # Errors
    ///
    /// Only when NSS can't serialize a configuration.
    pub fn enable_ech(
        &mut self,
        config: u8,
        public_name: &str,
        sk: &PrivateKey,
        pk: &PublicKey,
    ) -> Res<()> {
        self.server.enable_ech(config, public_name, sk, pk)?;
        Ok(())
    }

    #[must_use]
    pub fn ech_config(&self) -> &[u8] {
        self.server.ech_config()
    }

    /// Short-hand for [`Http3Server::process`] with no input datagram.
    pub fn process_output(&mut self, now: Instant) -> Output {
        self.process(None::<Datagram>, now)
    }

    /// Wrapper around [`Http3Server::process_multiple`] that processes a single
    /// output datagram only.
    #[expect(clippy::missing_panics_doc, reason = "see expect()")]
    pub fn process<A: AsRef<[u8]> + AsMut<[u8]>, I: IntoIterator<Item = Datagram<A>>>(
        &mut self,
        dgrams: I,
        now: Instant,
    ) -> Output {
        self.process_multiple(dgrams, now, 1.try_into().expect(">0"))
            .try_into()
            .expect("max_datagrams is 1")
    }

    pub fn process_multiple<A: AsRef<[u8]> + AsMut<[u8]>, I: IntoIterator<Item = Datagram<A>>>(
        &mut self,
        dgrams: I,
        now: Instant,
        max_datagrams: NonZeroUsize,
    ) -> OutputBatch {
        qtrace!("[{self}] Process");
        let out = self.server.process_multiple_input(dgrams, now);
        self.process_http3(now);
        match out {
            OutputBatch::DatagramBatch(d) => {
                qtrace!("[{self}] Send packet: {d:?}");
                OutputBatch::DatagramBatch(d)
            }
            _ => self
                .server
                .process_multiple(Option::<Datagram>::None, now, max_datagrams),
        }
    }

    /// Process HTTP3 layer.
    fn process_http3(&mut self, now: Instant) {
        qtrace!("[{self}] Process http3 internal");
        #[expect(
            clippy::mutable_key_type,
            reason = "ActiveConnectionRef::Hash doesn't access any of the interior mutable types."
        )]
        let mut active_conns = self.server.active_connections();
        active_conns.extend(
            self.http3_handlers
                .iter()
                .filter(|(_, handler)| handler.borrow_mut().should_be_processed())
                .map(|(c, _)| c)
                .cloned(),
        );

        #[expect(
            clippy::iter_over_hash_type,
            reason = "OK to loop over active connections in an undefined order."
        )]
        for conn in active_conns {
            self.process_events(&conn, now);
        }
    }

    #[expect(
        clippy::too_many_lines,
        reason = "Function is mostly a match statement."
    )]
    fn process_events(&mut self, conn: &ConnectionRef, now: Instant) {
        let mut remove = false;
        let http3_parameters = &self.http3_parameters;
        {
            let handler = self.http3_handlers.entry(conn.clone()).or_insert_with(|| {
                Rc::new(RefCell::new(Http3ServerHandler::new(
                    http3_parameters.clone(),
                )))
            });
            handler
                .borrow_mut()
                .process_http3(&mut conn.borrow_mut(), now);
            let mut handler_borrowed = handler.borrow_mut();
            while let Some(e) = handler_borrowed.next_event() {
                match e {
                    Http3ServerConnEvent::Headers {
                        stream_info,
                        headers,
                        fin,
                    } => self.events.headers(
                        Http3OrWebTransportStream::new(
                            conn.clone(),
                            Rc::clone(handler),
                            stream_info,
                        ),
                        headers,
                        fin,
                    ),
                    Http3ServerConnEvent::DataReadable { stream_info } => {
                        prepare_data(
                            stream_info,
                            &mut handler_borrowed,
                            conn,
                            handler,
                            now,
                            &self.events,
                        );
                    }
                    Http3ServerConnEvent::DataWritable { stream_info } => self
                        .events
                        .data_writable(conn.clone(), Rc::clone(handler), stream_info),
                    Http3ServerConnEvent::StreamReset { stream_info, error } => {
                        self.events.stream_reset(
                            conn.clone(),
                            Rc::clone(handler),
                            stream_info,
                            error,
                        );
                    }
                    Http3ServerConnEvent::StreamStopSending { stream_info, error } => {
                        self.events.stream_stop_sending(
                            conn.clone(),
                            Rc::clone(handler),
                            stream_info,
                            error,
                        );
                    }
                    Http3ServerConnEvent::StateChange(state) => {
                        self.events
                            .connection_state_change(conn.clone(), state.clone());
                        if let Http3State::Closed { .. } = state {
                            remove = true;
                        }
                    }
                    Http3ServerConnEvent::PriorityUpdate {
                        stream_id,
                        priority,
                    } => {
                        self.events.priority_update(stream_id, priority);
                    }
                    Http3ServerConnEvent::WebTransport(WebTransportEvent::Session {
                        stream_id,
                        headers,
                    }) => {
                        self.events.webtransport_new_session(
                            WebTransportRequest::new(conn.clone(), Rc::clone(handler), stream_id),
                            headers,
                        );
                    }
                    Http3ServerConnEvent::ConnectUdp(ConnectUdpEvent::Session {
                        stream_id,
                        headers,
                    }) => {
                        self.events.connect_udp_new_session(
                            ConnectUdpRequest::new(conn.clone(), Rc::clone(handler), stream_id),
                            headers,
                        );
                    }
                    Http3ServerConnEvent::WebTransport(WebTransportEvent::SessionClosed {
                        stream_id,
                        reason,
                        headers,
                        ..
                    }) => self.events.webtransport_session_closed(
                        WebTransportRequest::new(conn.clone(), Rc::clone(handler), stream_id),
                        reason,
                        headers,
                    ),
                    Http3ServerConnEvent::ConnectUdp(ConnectUdpEvent::SessionClosed {
                        stream_id,
                        reason,
                        headers,
                        ..
                    }) => self.events.connect_udp_session_closed(
                        ConnectUdpRequest::new(conn.clone(), Rc::clone(handler), stream_id),
                        reason,
                        headers,
                    ),
                    Http3ServerConnEvent::WebTransport(WebTransportEvent::NewStream(
                        stream_info,
                    )) => self
                        .events
                        .webtransport_new_stream(Http3OrWebTransportStream::new(
                            conn.clone(),
                            Rc::clone(handler),
                            stream_info,
                        )),
                    Http3ServerConnEvent::WebTransport(WebTransportEvent::Datagram {
                        session_id,
                        datagram,
                    }) => {
                        self.events.webtransport_datagram(
                            WebTransportRequest::new(conn.clone(), Rc::clone(handler), session_id),
                            datagram,
                        );
                    }
                    Http3ServerConnEvent::ConnectUdp(ConnectUdpEvent::Datagram {
                        session_id,
                        datagram,
                    }) => {
                        self.events.connect_udp_datagram(
                            ConnectUdpRequest::new(conn.clone(), Rc::clone(handler), session_id),
                            datagram,
                        );
                    }
                }
            }
        }
        if remove {
            self.http3_handlers.remove(&conn.clone());
        }
    }

    /// Get all current events. Best used just in debug/testing code, use
    /// `next_event` instead.
    pub fn events(&self) -> impl Iterator<Item = Http3ServerEvent> {
        self.events.events()
    }

    /// Return true if there are outstanding events.
    #[must_use]
    pub fn has_events(&self) -> bool {
        self.events.has_events()
    }

    /// Get events that indicate state changes on the connection. This method
    /// correctly handles cases where handling one event can obsolete
    /// previously-queued events, or cause new events to be generated.
    #[must_use]
    pub fn next_event(&self) -> Option<Http3ServerEvent> {
        self.events.next_event()
    }
}
fn prepare_data(
    stream_info: Http3StreamInfo,
    handler_borrowed: &mut RefMut<Http3ServerHandler>,
    conn: &ConnectionRef,
    handler: &HandlerRef,
    now: Instant,
    events: &Http3ServerEvents,
) {
    loop {
        let mut data = vec![0; MAX_EVENT_DATA_SIZE];
        let res = handler_borrowed.read_data(
            &mut conn.borrow_mut(),
            now,
            stream_info.stream_id(),
            &mut data,
        );
        if let Ok((amount, fin)) = res {
            if amount > 0 || fin {
                if amount < MAX_EVENT_DATA_SIZE {
                    data.resize(amount, 0);
                }

                events.data(conn.clone(), Rc::clone(handler), stream_info, data, fin);
            }
            if amount < MAX_EVENT_DATA_SIZE || fin {
                break;
            }
        } else {
            break;
        }
    }
}
