// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{
    cell::RefCell,
    fmt::{self, Display, Formatter},
    iter,
    net::SocketAddr,
    num::NonZeroUsize,
    rc::Rc,
    time::Instant,
};

use neqo_common::{
    Datagram, Decoder, Encoder, Header, MessageType, Role, event::Provider as EventProvider, hex,
    hex_with_len, qdebug, qinfo, qlog::Qlog, qtrace, qwarn,
};
use neqo_qpack::Stats as QpackStats;
use neqo_transport::{
    AppError, Connection, ConnectionEvent, ConnectionId, ConnectionIdGenerator, DatagramTracking,
    Output, OutputBatch, Stats as TransportStats, StreamId, StreamType, Version, ZeroRttState,
    recv_stream, send_stream, streams::SendOrder,
};
use nss::{AuthenticationStatus, ResumptionToken, SecretAgentInfo, agent::CertificateInfo};

use crate::{
    Error, Http3Parameters, Http3StreamType, NewStreamType, Priority, PriorityHandler, PushId,
    ReceiveOutput, Res,
    client_events::{Http3ClientEvent, Http3ClientEvents},
    connection::{Http3Connection, Http3State, RequestDescription},
    features::ConnectType,
    frames::HFrame,
    push_controller::{PushController, RecvPushEvents},
    recv_message::{RecvMessage, RecvMessageInfo},
    request_target::RequestTarget,
    settings::HSettings,
};

fn id_gte<U>(base: StreamId) -> impl FnMut((&StreamId, &U)) -> Option<StreamId> + 'static
where
    U: ?Sized,
{
    move |(id, _)| (*id >= base && !(id.is_bidi() ^ base.is_bidi())).then_some(*id)
}

const fn alpn_from_quic_version(version: Version) -> &'static str {
    match version {
        Version::Version2 | Version::Version1 => "h3",
        #[cfg(feature = "draft-29")]
        Version::Draft29 => "h3-29",
    }
}

/// # The HTTP/3 client API
///
/// This module implements the HTTP/3 client API. The main implementation of the protocol is in
/// [connection.rs](https://github.com/mozilla/neqo/blob/main/neqo-http3/src/connection.rs) which
/// implements common behavior for the client-side and the server-side. `Http3Client` structure
/// implements the public API and set of functions that differ between the client and the server.
///
/// The API is used for:
/// - create and close an endpoint:
///   - [`Http3Client::new`]
///   - [`Http3Client::new_with_conn`]
///   - [`Http3Client::close`]
/// - configuring an endpoint:
///   - [`Http3Client::authenticated`]
///   - [`Http3Client::enable_ech`]
///   - [`Http3Client::enable_resumption`]
///   - [`Http3Client::set_qlog`]
/// - retrieving information about a connection:
/// - [`Http3Client::peer_certificate`]
///   - [`Http3Client::qpack_encoder_stats`]
///   - [`Http3Client::transport_stats`]
///   - [`Http3Client::state`]
///   - [`Http3Client::tls_info`]
/// - driving HTTP/3 session:
///   - [`Http3Client::process_output`]
///   - [`Http3Client::process_input`]
///   - [`Http3Client::process`]
/// - create requests, send/receive data, and cancel requests:
///   - [`Http3Client::fetch`]
///   - [`Http3Client::send_data`]
///   - [`Http3Client::read_data`]
///   - [`Http3Client::stream_close_send`]
///   - [`Http3Client::cancel_fetch`]
///   - [`Http3Client::stream_reset_send`]
///   - [`Http3Client::stream_stop_sending`]
/// - priority feature:
///   - [`Http3Client::priority_update`]
/// - `WebTransport` feature:
///   - [`Http3Client::webtransport_create_session`]
///   - [`Http3Client::webtransport_close_session`]
///   - [`Http3Client::webtransport_create_stream`]
///   - [`Http3Client::webtransport_enabled`]
///
/// ## Examples
///
/// ### Fetching a resource
///
/// ```ignore
/// let mut client = Http3Client::new(...);
///
/// // Perform a handshake
/// ...
///
/// let req = client
///     .fetch(
///         Instant::now(),
///         "GET",
///         &("https", "something.com", "/"),
///         &[Header::new("example1", "value1"), Header::new("example1", "value2")],
///         Priority::default(),
///     )
///     .unwrap();
///
/// client.stream_close_send(req).unwrap();
///
/// loop {
///     // exchange packets
///     ...
///
///     while let Some(event) = client.next_event() {
///         match event {
///             Http3ClientEvent::HeaderReady { stream_id, headers, interim, fin } => {
///                 println!("New response headers received for stream {:?} [fin={?}, interim={:?}]: {:?}",
///                     stream_id,
///                     fin,
///                     interim,
///                     headers,
///                 );
///             }
///             Http3ClientEvent::DataReadable { stream_id } => {
///                 println!("New data available on stream {stream_id}");
///                let mut buf = [0; 100];
///                let (amount, fin) = client.read_data(now(), stream_id, &mut buf).unwrap();
///                 println!("Read {:?} bytes from stream {:?} [fin={?}]",
///                     amount,
///                     stream_id,
///                     fin,
///                 );
///             }
///             _ => {
///                 println!("Unhandled event {:?}", event);
///             }
///         }
///     }
/// }
/// ```
///
/// ### Creating a `WebTransport` session
///
/// ```ignore
/// let mut client = Http3Client::new(...);
///
/// // Perform a handshake
/// ...
///
/// // Create a session
/// let wt_session_id = client
///     .webtransport_create_session(now(), &("https", "something.com", "/"), &[])
///     .unwrap();
///
/// loop {
///     // exchange packets
///     ...
///
///     while let Some(event) = client.next_event() {
///         match event {
///             Http3ClientEvent::WebTransport(WebTransportEvent::NewSession{
///                 stream_id,
///                 status,
///                 ..
///             }) => {
///                 println!("The response from the server: WebTransport session ID {:?} status={:?}",
///                     stream_id,
///                     status,
///                 );
///             }
///             _ => {
///                 println!("Unhandled event {:?}", event);
///             }
///         }
///     }
/// }
/// ```
///
/// ### `WebTransport`: create a stream, send and receive data on the stream
///
/// ```ignore
/// const BUF_CLIENT: &[u8] = &[0; 10];
/// // wt_session_id is the session ID of a newly created WebTransport session, see the example above.
///
/// // create a  stream
/// let wt_stream_id = client
///     .webtransport_create_stream(wt_session_id, StreamType::BiDi)
///     .unwrap();
///
/// // send data
/// let data_sent = client.send_data(wt_stream_id, BUF_CLIENT).unwrap();
/// assert_eq!(data_sent, BUF_CLIENT.len());
///
/// // close stream for sending
/// client.stream_close_send(wt_stream_id).unwrap();
///
/// // wait for data from the server
/// loop {
///     // exchange packets
///     ...
///
///     while let Some(event) = client.next_event() {
///         match event {
///             Http3ClientEvent::DataReadable{ stream_id } => {
///                 println!("Data received form the server on WebTransport stream ID {:?}",
///                     stream_id,
///                 );
///                 let mut buf = [0; 100];
///                 let (amount, fin) = client.read_data(now(), stream_id, &mut buf).unwrap();
///                 println!("Read {:?} bytes from stream {:?} [fin={?}]",
///                     amount,
///                     stream_id,
///                     fin,
///                 );
///             }
///             _ => {
///                 println!("Unhandled event {:?}", event);
///             }
///         }
///     }
/// }
/// ```
///
/// ### `WebTransport`: receive a new stream form the server
///
/// ```ignore
/// // wt_session_id is the session ID of a newly created WebTransport session, see the example above.
///
/// // wait for a new stream from the server
/// loop {
///     // exchange packets
///     ...
///
///     while let Some(event) = client.next_event() {
///         match event {
///             Http3ClientEvent::WebTransport(WebTransportEvent::NewStream {
///                 stream_id,
///                 session_id,
///             }) => {
///                 println!("New stream received on session{:?}, stream id={:?} stream type={:?}",
///                     session_id.stream_id(),
///                     stream_id.stream_id(),
///                     stream_id.stream_type()
///                 );
///             }
///             Http3ClientEvent::DataReadable{ stream_id } => {
///                 println!("Data received form the server on WebTransport stream ID {:?}",
///                     stream_id,
///                 );
///                 let mut buf = [0; 100];
///                 let (amount, fin) = client.read_data(now(), stream_id, &mut buf).unwrap();
///                 println!("Read {:?} bytes from stream {:?} [fin={:?}]",
///                     amount,
///                     stream_id,
///                     fin,
///                 );
///             }
///             _ => {
///                 println!("Unhandled event {:?}", event);
///             }
///         }
///     }
/// }
/// ```
pub struct Http3Client {
    conn: Connection,
    base_handler: Http3Connection,
    events: Http3ClientEvents,
    push_handler: Rc<RefCell<PushController>>,
}

impl Display for Http3Client {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "Http3 client")
    }
}

impl Http3Client {
    /// # Errors
    ///
    /// Making a `neqo-transport::connection` may produce an error. This can only be a crypto error
    /// if the crypto context can't be created or configured.
    pub fn new<I: Into<String>>(
        server_name: I,
        cid_manager: Rc<RefCell<dyn ConnectionIdGenerator>>,
        local_addr: SocketAddr,
        remote_addr: SocketAddr,
        http3_parameters: Http3Parameters,
        now: Instant,
    ) -> Res<Self> {
        Ok(Self::new_with_conn(
            Connection::new_client(
                server_name,
                &[alpn_from_quic_version(
                    http3_parameters
                        .get_connection_parameters()
                        .get_versions()
                        .initial(),
                )],
                cid_manager,
                local_addr,
                remote_addr,
                http3_parameters.get_connection_parameters().clone(),
                now,
            )?,
            http3_parameters,
        ))
    }

    /// This is a similar function to `new`. In this case, `neqo-transport::connection` has been
    /// already created.
    ///
    /// It is recommended to use `new` instead.
    #[must_use]
    pub fn new_with_conn(c: Connection, http3_parameters: Http3Parameters) -> Self {
        let events = Http3ClientEvents::default();
        let push_streams = http3_parameters.get_max_concurrent_push_streams();
        let mut base_handler = Http3Connection::new(http3_parameters, Role::Client);
        base_handler.set_features_listener(events.clone());
        Self {
            conn: c,
            events: events.clone(),
            push_handler: Rc::new(RefCell::new(PushController::new(push_streams, events))),
            base_handler,
        }
    }

    /// The function returns the current state of the connection.
    #[must_use]
    pub fn state(&self) -> Http3State {
        self.base_handler.state().clone()
    }

    #[must_use]
    pub fn tls_info(&self) -> Option<&SecretAgentInfo> {
        self.conn.tls_info()
    }

    /// Get the peer's certificate.
    #[must_use]
    pub fn peer_certificate(&self) -> Option<CertificateInfo> {
        self.conn.peer_certificate()
    }

    /// This called when peer certificates have been verified.
    ///
    /// `Http3ClientEvent::AuthenticationNeeded` event is emitted when peer’s certificates are
    /// available and need to be verified. When the verification is completed this function is
    /// called. To inform HTTP/3 session of the verification results.
    pub fn authenticated(&mut self, status: AuthenticationStatus, now: Instant) {
        self.conn.authenticated(status, now);
    }

    pub fn set_qlog(&mut self, qlog: Qlog) {
        self.conn.set_qlog(qlog);
    }

    /// Enable encrypted client hello (ECH).
    ///
    /// # Errors
    ///
    /// Fails when the configuration provided is bad.
    pub fn enable_ech<A: AsRef<[u8]>>(&mut self, ech_config_list: A) -> Res<()> {
        self.conn.client_enable_ech(ech_config_list)?;
        Ok(())
    }

    /// Get the connection id, which is useful for disambiguating connections to
    /// the same origin.
    ///
    /// # Panics
    ///
    /// Never, because clients always have this field.
    #[must_use]
    pub const fn connection_id(&self) -> &ConnectionId {
        self.conn.odcid().expect("Client always has odcid")
    }

    fn encode_resumption_token(&self, token: &ResumptionToken) -> Option<ResumptionToken> {
        self.base_handler.get_settings().map(|settings| {
            let mut enc = Encoder::default();
            settings.encode_frame_contents(&mut enc);
            enc.encode(token.as_ref());
            ResumptionToken::new(enc.into(), token.expiration_time())
        })
    }

    /// This may be call if an application has a resumption token. This must be called before
    /// connection starts.
    ///
    /// The resumption token also contains encoded HTTP/3 settings. The settings will be decoded
    /// and used until the setting are received from the server.
    ///
    /// # Errors
    ///
    /// An error is return if token cannot be decoded or a connection is is a wrong state.
    ///
    /// # Panics
    ///
    /// On closing if the base handler can't handle it (debug only).
    pub fn enable_resumption<A: AsRef<[u8]>>(&mut self, now: Instant, token: A) -> Res<()> {
        if self.base_handler.state() != &Http3State::Initializing {
            return Err(Error::InvalidState);
        }
        let mut dec = Decoder::from(token.as_ref());
        let Some(settings_slice) = dec.decode_vvec() else {
            return Err(Error::InvalidResumptionToken);
        };
        qtrace!("[{self}]   settings {}", hex_with_len(settings_slice));
        let mut dec_settings = Decoder::from(settings_slice);
        let mut settings = HSettings::default();
        Error::map_error(
            settings.decode_frame_contents(&mut dec_settings),
            Error::InvalidResumptionToken,
        )?;
        let tok = dec.decode_remainder();
        qtrace!("[{self}]   Transport token {}", hex(tok));
        self.conn.enable_resumption(now, tok)?;
        if self.conn.state().closed() {
            let state = self.conn.state().clone();
            let res = self
                .base_handler
                .handle_state_change(&mut self.conn, &state);
            debug_assert_eq!(Ok(true), res);
            return Err(Error::Fatal);
        }
        if self.conn.zero_rtt_state() == ZeroRttState::Sending {
            self.base_handler
                .set_0rtt_settings(&mut self.conn, settings)?;
            self.events
                .connection_state_change(self.base_handler.state().clone());
            self.push_handler
                .borrow_mut()
                .maybe_send_max_push_id_frame(&mut self.base_handler);
        }
        Ok(())
    }

    /// Returns a resumption token if one is available, wrapped with the current
    /// H3 settings. Use as a fallback when the `ResumptionToken` event has not
    /// fired before the connection closes (e.g., `NEW_TOKEN` never arrived).
    pub fn take_resumption_token(&mut self, now: Instant) -> Option<ResumptionToken> {
        let transport_token = self.conn.take_resumption_token(now)?;
        self.encode_resumption_token(&transport_token)
    }

    /// This is call to close a connection.
    pub fn close<S>(&mut self, now: Instant, error: AppError, msg: S)
    where
        S: AsRef<str> + Display,
    {
        qinfo!("[{self}] Close the connection error={error} msg={msg}");
        if !matches!(
            self.base_handler.state(),
            Http3State::Closing(_) | Http3State::Closed(_)
        ) {
            self.push_handler.borrow_mut().clear();
            self.conn.close(now, error, msg);
            self.base_handler.close(error);
            self.events
                .connection_state_change(self.base_handler.state().clone());
        }
    }


    /// The function fetches a resource using `method`, `target` and `headers`. A response body
    /// may be added by calling `send_data`. `stream_close_send` must be sent to finish the request
    /// even if request data are not sent.
    ///
    /// # Errors
    ///
    /// If a new stream cannot be created an error will be return.
    ///
    /// # Panics
    ///
    /// `SendMessage` implements `http_stream` so it will not panic.
    pub fn fetch<'t, T>(
        &mut self,
        now: Instant,
        method: &'t str,
        target: T,
        headers: &'t [Header],
        priority: Priority,
    ) -> Res<StreamId>
    where
        T: RequestTarget,
    {
        if method == "CONNECT" {
            qwarn!("Invalid method CONNECT in fetch. Use Http3Client::connect instead.");
            return Err(Error::InvalidInput);
        }
        let output = self.base_handler.request(
            &mut self.conn,
            Box::new(self.events.clone()),
            Box::new(self.events.clone()),
            Some(Rc::clone(&self.push_handler)),
            &RequestDescription {
                method,
                connect_type: None,
                target,
                headers,
                priority,
            },
            now,
        );
        if let Err(e) = &output
            && e.connection_error()
        {
            self.close(now, e.code(), "");
        }
        output
    }

    /// The function establishes a classic HTTP CONNECT tunnel on top of this
    /// connection using `target` and `headers`. Data can be send into the
    /// tunnel via [`Http3Client::send_data`] and received from the tunnel via
    /// [`Http3Client::read_data`]. The tunnel can be closed via
    /// [`Http3Client::stream_close_send`].
    ///
    /// # Errors
    ///
    /// If a new stream cannot be created an error will be return.
    pub fn connect<A>(
        &mut self,
        now: Instant,
        authority: A,
        headers: &[Header],
        priority: Priority,
    ) -> Res<StreamId>
    where
        A: AsRef<str>,
    {
        let output = self.base_handler.request(
            &mut self.conn,
            Box::new(self.events.clone()),
            Box::new(self.events.clone()),
            Some(Rc::clone(&self.push_handler)),
            &RequestDescription {
                method: "CONNECT",
                connect_type: Some(ConnectType::Classic),

                target: ("", authority.as_ref(), ""),
                headers,
                priority,
            },
            now,
        );
        if let Err(e) = &output
            && e.connection_error()
        {
            self.close(now, e.code(), "");
        }
        output
    }

    /// Send an [`PRIORITY_UPDATE`-frame][1] on next `Http3Client::process_output()` call.
    /// Returns if the priority got changed.
    ///
    /// # Errors
    ///
    /// `InvalidStreamId` if the stream does not exist
    ///
    /// [1]: https://datatracker.ietf.org/doc/html/draft-kazuho-httpbis-priority-04#section-5.2
    pub fn priority_update(&mut self, stream_id: StreamId, priority: Priority) -> Res<bool> {
        self.base_handler.queue_update_priority(stream_id, priority)
    }

    /// An application may cancel a stream(request).
    /// Both sides, the receiving and sending side, sending and receiving side, will be closed.
    ///
    /// # Errors
    ///
    /// An error will be return if a stream does not exist.
    pub fn cancel_fetch(&mut self, stream_id: StreamId, error: AppError) -> Res<()> {
        qinfo!("[{self}] reset_stream {stream_id} error={error}");
        self.base_handler
            .cancel_fetch(stream_id, error, &mut self.conn)
    }

    /// This is call when application is done sending a request.
    ///
    /// # Errors
    ///
    /// An error will be return if stream does not exist.
    pub fn stream_close_send(&mut self, stream_id: StreamId, now: Instant) -> Res<()> {
        qdebug!("[{self}] Close sending side stream={stream_id}");
        self.base_handler
            .stream_close_send(&mut self.conn, stream_id, now)
    }

    /// # Errors
    ///
    /// An error will be return if a stream does not exist.
    pub fn stream_reset_send(&mut self, stream_id: StreamId, error: AppError) -> Res<()> {
        qinfo!("[{self}] stream_reset_send {stream_id} error={error}");
        self.base_handler
            .stream_reset_send(&mut self.conn, stream_id, error)
    }

    /// # Errors
    ///
    /// An error will be return if a stream does not exist.
    pub fn stream_stop_sending(&mut self, stream_id: StreamId, error: AppError) -> Res<()> {
        qinfo!("[{self}] stream_stop_sending {stream_id} error={error}");
        self.base_handler
            .stream_stop_sending(&mut self.conn, stream_id, error)
    }

    /// This function is used for regular HTTP requests and `WebTransport` streams.
    /// In the case of regular HTTP requests, the request body is supplied using this function, and
    /// headers are supplied through the `fetch` function.
    ///
    /// # Errors
    ///
    /// `InvalidStreamId` if the stream does not exist,
    /// `AlreadyClosed` if the stream has already been closed.
    /// `TransportStreamDoesNotExist` if the transport stream does not exist (this may happen if
    /// `process_output` has not been called when needed, and HTTP3 layer has not picked up the
    /// info that the stream has been closed.) `InvalidInput` if an empty buffer has been
    /// supplied.
    pub fn send_data(&mut self, stream_id: StreamId, buf: &[u8], now: Instant) -> Res<usize> {
        qinfo!(
            "[{self}] end_data from stream {stream_id} sending {} bytes",
            buf.len()
        );
        self.base_handler
            .send_streams_mut()
            .get_mut(&stream_id)
            .ok_or(Error::InvalidStreamId)?
            .send_data(&mut self.conn, buf, now)
    }

    /// Response data are read directly into a buffer supplied as a parameter of this function to
    /// avoid copying data.
    ///
    /// # Errors
    ///
    /// It returns an error if a stream does not exist or an error happen while reading a stream,
    /// e.g. early close, protocol error, etc.
    pub fn read_data(
        &mut self,
        now: Instant,
        stream_id: StreamId,
        buf: &mut [u8],
    ) -> Res<(usize, bool)> {
        qdebug!("[{self}] read_data from stream {stream_id}");
        let res = self
            .base_handler
            .read_data(&mut self.conn, stream_id, buf, now);
        if let Err(e) = &res
            && e.connection_error()
        {
            self.close(now, e.code(), "");
        }
        res
    }


    /// Cancel a push
    ///
    /// # Errors
    ///
    /// `InvalidStreamId` if the stream does not exist.
    pub fn cancel_push(&mut self, push_id: PushId) -> Res<()> {
        self.push_handler
            .borrow_mut()
            .cancel(push_id, &mut self.conn, &mut self.base_handler)
    }

    /// Push response data are read directly into a buffer supplied as a parameter of this function
    /// to avoid copying data.
    ///
    /// # Errors
    ///
    /// It returns an error if a stream does not exist(`InvalidStreamId`) or an error has happened
    /// while reading a stream, e.g. early close, protocol error, etc.
    pub fn push_read_data(
        &mut self,
        now: Instant,
        push_id: PushId,
        buf: &mut [u8],
    ) -> Res<(usize, bool)> {
        let stream_id = self
            .push_handler
            .borrow_mut()
            .get_active_stream_id(push_id)
            .ok_or(Error::InvalidStreamId)?;
        self.conn.stream_keep_alive(stream_id, true)?;
        self.read_data(now, stream_id, buf)
    }


    /// # Errors
    ///
    /// If `WebTransport` cannot be created, e.g. the `WebTransport` support is
    /// not negotiated or the HTTP/3 connection is closed.
    pub fn webtransport_create_session<T>(
        &mut self,
        now: Instant,
        target: T,
        headers: &[Header],
    ) -> Res<StreamId>
    where
        T: RequestTarget,
    {
        let output = self.base_handler.webtransport_create_session(
            &mut self.conn,
            Box::new(self.events.clone()),
            target,
            headers,
        );

        if let Err(e) = &output
            && e.connection_error()
        {
            self.close(now, e.code(), "");
        }
        output
    }

    /// # Errors
    ///
    /// If MASQUE connect-udp session cannot be created, e.g. the HTTP CONNECT
    /// setting is not negotiated or the HTTP/3 connection is closed.
    pub fn connect_udp_create_session<T>(
        &mut self,
        now: Instant,
        target: T,
        headers: &[Header],
    ) -> Res<StreamId>
    where
        T: RequestTarget,
    {
        let output = self.base_handler.connect_udp_create_session(
            &mut self.conn,
            Box::new(self.events.clone()),
            target,
            headers,
        );

        if let Err(e) = &output
            && e.connection_error()
        {
            self.close(now, e.code(), "");
        }
        output
    }

    /// Close `WebTransport` cleanly
    ///
    /// # Errors
    ///
    /// `InvalidStreamId` if the stream does not exist,
    /// `TransportStreamDoesNotExist` if the transport stream does not exist (this may happen if
    /// `process_output` has not been called when needed, and HTTP3 layer has not picked up the
    /// info that the stream has been closed.) `InvalidInput` if an empty buffer has been
    /// supplied.
    pub fn webtransport_close_session(
        &mut self,
        session_id: StreamId,
        error: u32,
        message: &str,
        now: Instant,
    ) -> Res<()> {
        self.base_handler.webtransport_close_session(
            &mut self.conn,
            session_id,
            error,
            message,
            now,
        )
    }

    /// Close `ConnectUdp` cleanly
    ///
    /// # Errors
    ///
    /// [`Error::InvalidStreamId`] if the stream does not exist,
    /// [`Error::TransportStreamDoesNotExist`] if the transport stream does not
    /// exist (this may happen if [`Http3Client::process_output`] has not been
    /// called when needed, and HTTP3 layer has not picked up the info that the
    /// stream has been closed.)
    pub fn connect_udp_close_session(
        &mut self,
        session_id: StreamId,
        error: u32,
        message: &str,
        now: Instant,
    ) -> Res<()> {
        self.base_handler
            .connect_udp_close_session(&mut self.conn, session_id, error, message, now)
    }

    /// # Errors
    ///
    /// This may return an error if the particular session does not exist
    /// or the connection is not in the active state.
    pub fn webtransport_create_stream(
        &mut self,
        session_id: StreamId,
        stream_type: StreamType,
    ) -> Res<StreamId> {
        self.base_handler.webtransport_create_stream_local(
            &mut self.conn,
            session_id,
            stream_type,
            Box::new(self.events.clone()),
            Box::new(self.events.clone()),
        )
    }

    /// Send `WebTransport` datagram.
    ///
    /// # Errors
    ///
    /// It may return `InvalidStreamId` if a stream does not exist anymore.
    /// The function returns `TooMuchData` if the supply buffer is bigger than
    /// the allowed remote datagram size.
    pub fn webtransport_send_datagram<I: Into<DatagramTracking>>(
        &mut self,
        session_id: StreamId,
        buf: &[u8],
        id: I,
        now: Instant,
    ) -> Res<()> {
        qtrace!("webtransport_send_datagram session:{session_id:?}");
        self.base_handler
            .webtransport_send_datagram(session_id, &mut self.conn, buf, id, now)
    }

    /// Send `ConnectUdp` datagram.
    ///
    /// # Errors
    ///
    /// It may return `InvalidStreamId` if a stream does not exist anymore.
    /// The function returns `TooMuchData` if the supply buffer is bigger than
    /// the allowed remote datagram size.
    pub fn connect_udp_send_datagram<I: Into<DatagramTracking>>(
        &mut self,
        session_id: StreamId,
        buf: &[u8],
        id: I,
        now: Instant,
    ) -> Res<()> {
        qtrace!("connect_udp_send_datagram session:{session_id:?}");
        self.base_handler
            .connect_udp_send_datagram(session_id, &mut self.conn, buf, id, now)
    }

    /// Returns the current max size of a datagram that can fit into a packet.
    /// The value will change over time depending on the encoded size of the
    /// packet number, ack frames, etc.
    ///
    /// # Errors
    ///
    /// The function returns `NotAvailable` if datagrams are not enabled.
    ///
    /// # Panics
    ///
    /// This cannot panic. The max varint length is 8.
    pub fn webtransport_max_datagram_size(&self, session_id: StreamId) -> Res<u64> {
        Ok(self.conn.max_datagram_size()?
            - u64::try_from(Encoder::varint_len(session_id.as_u64()))
                .map_err(|_| Error::Internal)?)
    }

    /// Sets the `SendOrder` for a given stream
    ///
    /// # Errors
    ///
    /// It may return `InvalidStreamId` if a stream does not exist anymore.
    pub fn webtransport_set_sendorder(
        &mut self,
        stream_id: StreamId,
        sendorder: Option<SendOrder>,
    ) -> Res<()> {
        Http3Connection::stream_set_sendorder(&mut self.conn, stream_id, sendorder)
    }

    /// Sets the `Fairness` for a given stream
    ///
    /// # Errors
    ///
    /// It may return `InvalidStreamId` if a stream does not exist anymore.
    pub fn webtransport_set_fairness(&mut self, stream_id: StreamId, fairness: bool) -> Res<()> {
        Http3Connection::stream_set_fairness(&mut self.conn, stream_id, fairness)
    }

    /// Returns the current `send_stream::Stats` of a `WebTransportSendStream`.
    ///
    /// # Errors
    ///
    /// `InvalidStreamId` if the stream does not exist.
    pub fn webtransport_send_stream_stats(
        &mut self,
        stream_id: StreamId,
    ) -> Res<send_stream::Stats> {
        self.base_handler
            .send_streams_mut()
            .get_mut(&stream_id)
            .ok_or(Error::InvalidStreamId)?
            .stats(&mut self.conn)
    }

    /// Returns the current `recv_stream::Stats` of a `WebTransportRecvStream`.
    ///
    /// # Errors
    ///
    /// `InvalidStreamId` if the stream does not exist.
    pub fn webtransport_recv_stream_stats(
        &mut self,
        stream_id: StreamId,
    ) -> Res<recv_stream::Stats> {
        self.base_handler
            .recv_streams_mut()
            .get_mut(&stream_id)
            .ok_or(Error::InvalidStreamId)?
            .stats(&mut self.conn)
    }

    /// This function combines  `process_input` and `process_output` function.
    pub fn process<A: AsRef<[u8]> + AsMut<[u8]>>(
        &mut self,
        dgram: Option<Datagram<A>>,
        now: Instant,
    ) -> Output {
        qtrace!("[{self}] Process");
        if let Some(d) = dgram {
            self.process_input(d, now);
        }
        self.process_output(now)
    }

    /// The function should be called when there is a new UDP packet available. The function will
    /// handle the packet payload.
    ///
    /// First, the payload will be handled by the QUIC layer. Afterward, `process_http3` will be
    /// called to handle new [`ConnectionEvent`][1]s.
    ///
    /// After this function is called `process_output` should be called to check whether new
    /// packets need to be sent or if a timer needs to be updated.
    ///
    /// [1]: ../neqo_transport/enum.ConnectionEvent.html
    pub fn process_input<A: AsRef<[u8]> + AsMut<[u8]>>(
        &mut self,
        dgram: Datagram<A>,
        now: Instant,
    ) {
        self.process_multiple_input(iter::once(dgram), now);
    }

    pub fn process_multiple_input<
        A: AsRef<[u8]> + AsMut<[u8]>,
        I: IntoIterator<Item = Datagram<A>>,
    >(
        &mut self,
        dgrams: I,
        now: Instant,
    ) {
        let mut dgrams = dgrams.into_iter().peekable();
        qtrace!("[{self}] Process multiple datagrams");
        if dgrams.peek().is_none() {
            return;
        }
        self.conn.process_multiple_input(dgrams, now);
        self.process_http3(now);
    }

    /// Process HTTP3 layer.
    /// When `process_output`, `process_input`, or `process` is called we must call this function
    /// as well. The functions calls `Http3Client::check_connection_events` to handle events from
    /// the QUC layer and calls `Http3Connection::process_sending` to ensure that HTTP/3 layer
    /// data, e.g. control frames, are sent.
    fn process_http3(&mut self, now: Instant) {
        qtrace!("[{self}] Process http3 internal");
        match self.base_handler.state() {
            Http3State::ZeroRtt | Http3State::Connected | Http3State::GoingAway(..) => {
                let res = self.check_connection_events(now);
                if self.check_result(now, &res) {
                    return;
                }
                self.push_handler
                    .borrow_mut()
                    .maybe_send_max_push_id_frame(&mut self.base_handler);
                let res = self.base_handler.process_sending(&mut self.conn, now);
                self.check_result(now, &res);
            }
            Http3State::Closed { .. } => {}
            _ => {
                let res = self.check_connection_events(now);
                _ = self.check_result(now, &res);
            }
        }
    }

    /// Wrapper around [`Http3Client::process_multiple_output`] that processes a single
    /// output datagram only.
    #[expect(clippy::missing_panics_doc, reason = "see expect()")]
    pub fn process_output(&mut self, now: Instant) -> Output {
        self.process_multiple_output(now, 1.try_into().expect(">0"))
            .try_into()
            .expect("max_datagrams is 1")
    }

    /// The function should be called to check if there are new UDP packets to be sent. It should
    /// be called after a new packet is received and processed and after a timer expires (QUIC
    /// needs timers to handle events like PTO detection and timers are not implemented by the neqo
    /// library, but instead must be driven by the application).
    ///
    /// [`Http3Client::process_multiple_output`] can return:
    /// - a [`OutputBatch::DatagramBatch`]: data that should be sent as a UDP payload,
    /// - a [`OutputBatch::Callback`]: the duration of a  timer. `process_output` should be called
    ///   at least after the time expires,
    /// - [`OutputBatch::None`]: this is returned when `Http3Client` is done and can be destroyed.
    ///
    /// The application should call this function repeatedly until a timer value or None is
    /// returned. After that, the application should call the function again if a new UDP packet is
    /// received and processed or the timer value expires.
    ///
    /// The HTTP/3 neqo implementation drives the HTTP/3 and QUIC layers, therefore this function
    /// will call both layers:
    ///  - First it calls HTTP/3 layer processing (`process_http3`) to make sure the layer writes
    ///    data to QUIC layer or cancels streams if needed.
    ///  - Then QUIC layer processing is called - [`Connection::process_output`][3]. This produces a
    ///    packet or a timer value. It may also produce new [`ConnectionEvent`][2]s, e.g. connection
    ///    state-change event.
    ///  - Therefore the HTTP/3 layer processing (`process_http3`) is called again.
    ///
    /// [1]: ../neqo_transport/enum.Output.html
    /// [2]: ../neqo_transport/struct.ConnectionEvents.html
    /// [3]: ../neqo_transport/struct.Connection.html#method.process_output
    pub fn process_multiple_output(
        &mut self,
        now: Instant,
        max_datagrams: NonZeroUsize,
    ) -> OutputBatch {
        qtrace!("[{self}] Process output");

        self.process_http3(now);

        let out = self.conn.process_multiple_output(now, max_datagrams);

        self.process_http3(now);

        out
    }

    /// This function takes the provided result and check for an error.
    /// An error results in closing the connection.
    fn check_result<ERR>(&mut self, now: Instant, res: &Res<ERR>) -> bool {
        match &res {
            Err(Error::HttpGoaway) => {
                qinfo!("[{self}] Connection error: goaway stream_id increased");
                self.close(
                    now,
                    Error::HttpGeneralProtocol.code(),
                    "Connection error: goaway stream_id increased",
                );
                true
            }
            Err(e) => {
                qinfo!("[{self}] Connection error: {e}");
                self.close(now, e.code(), format!("{e}"));
                true
            }
            _ => false,
        }
    }

    /// This function checks [`ConnectionEvent`][2]s emitted by the QUIC layer, e.g. connection
    /// change state events, new incoming stream data is available, a stream is was reset, etc.
    /// The HTTP/3 layer needs to handle these events. Most of the events are handled by
    /// [`Http3Connection`][1] by calling appropriate functions, e.g. `handle_state_change`,
    /// `handle_stream_reset`, etc. [`Http3Connection`][1] handle functionalities that are common
    /// for the client and server side. Some of the functionalities are specific to the client and
    /// they are handled by `Http3Client`. For example, [`ConnectionEvent::RecvStreamReadable`][3]
    /// event is handled by `Http3Client::handle_stream_readable`. The  function calls
    /// `Http3Connection::handle_stream_readable` and then hands the return value as appropriate
    /// for the client-side.
    ///
    /// [1]: https://github.com/mozilla/neqo/blob/main/neqo-http3/src/connection.rs
    /// [2]: ../neqo_transport/enum.ConnectionEvent.html
    /// [3]: ../neqo_transport/enum.ConnectionEvent.html#variant.RecvStreamReadable
    fn check_connection_events(&mut self, now: Instant) -> Res<()> {
        qtrace!("[{self}] Check connection events");
        while let Some(e) = self.conn.next_event() {
            qdebug!("[{self}] check_connection_events - event {e:?}");
            match e {
                ConnectionEvent::NewStream { stream_id } => {
                    self.base_handler.add_new_stream(stream_id);
                }
                ConnectionEvent::SendStreamWritable { stream_id } => {
                    if let Some(s) = self.base_handler.send_streams_mut().get_mut(&stream_id) {
                        s.stream_writable();
                    }
                }
                ConnectionEvent::RecvStreamReadable { stream_id } => {
                    self.handle_stream_readable(stream_id, now)?;
                }
                ConnectionEvent::RecvStreamReset {
                    stream_id,
                    app_error,
                } => self
                    .base_handler
                    .handle_stream_reset(stream_id, app_error, &mut self.conn)?,
                ConnectionEvent::SendStreamStopSending {
                    stream_id,
                    app_error,
                } => self.base_handler.handle_stream_stop_sending(
                    stream_id,
                    app_error,
                    &mut self.conn,
                )?,

                ConnectionEvent::SendStreamCreatable { stream_type } => {
                    self.events.new_requests_creatable(stream_type);
                }
                ConnectionEvent::AuthenticationNeeded => self.events.authentication_needed(),
                ConnectionEvent::EchFallbackAuthenticationNeeded { public_name } => {
                    self.events.ech_fallback_authentication_needed(public_name);
                }
                ConnectionEvent::StateChange(state) => {
                    if self
                        .base_handler
                        .handle_state_change(&mut self.conn, &state)?
                    {
                        self.events
                            .connection_state_change(self.base_handler.state().clone());
                    }
                }
                ConnectionEvent::ZeroRttRejected => {
                    self.base_handler.handle_zero_rtt_rejected()?;
                    self.events.zero_rtt_rejected();
                    self.push_handler.borrow_mut().handle_zero_rtt_rejected();
                }
                ConnectionEvent::ResumptionToken(token) => {
                    if let Some(t) = self.encode_resumption_token(&token) {
                        self.events.resumption_token(t);
                    }
                }
                ConnectionEvent::Datagram(dgram) => {
                    self.base_handler.handle_datagram(dgram);
                }
                ConnectionEvent::SendStreamComplete { .. }
                | ConnectionEvent::OutgoingDatagramOutcome { .. }
                | ConnectionEvent::IncomingDatagramDropped
                | ConnectionEvent::SconeUpdated(_)
                | ConnectionEvent::PathMigrated { .. } => {}
            }
        }
        Ok(())
    }

    /// This function handled new data available on a stream. It calls
    /// `Http3Client::handle_stream_readable` and handles its response. Reading streams are mostly
    /// handled by [`Http3Connection`][1] because most part of it is common for the client and
    /// server. The following actions need to be handled by the client-specific code:
    ///  - `ReceiveOutput::NewStream(NewStreamType::Push(_))` - the server cannot receive a push
    ///    stream,
    ///  - `ReceiveOutput::NewStream(NewStreamType::Http)` - client cannot  receive a
    ///    server-initiated HTTP request,
    ///  - `ReceiveOutput::NewStream(NewStreamType::WebTransportStream(_))` - because
    ///    `Http3ClientEvents`is needed and events handler is specific to the client.
    ///  - `ReceiveOutput::ControlFrames(control_frames)` - some control frame handling differs
    ///    between the  client and the server:
    ///     - `HFrame::CancelPush` - only the client-side may receive it,
    ///     - `HFrame::MaxPushId { .. }`, `HFrame::PriorityUpdateRequest { .. } ` and
    ///       `HFrame::PriorityUpdatePush` can only be receive on the server side,
    ///     - `HFrame::Goaway { stream_id }` needs specific handling by the client by the protocol
    ///       specification.
    ///
    /// [1]: https://github.com/mozilla/neqo/blob/main/neqo-http3/src/connection.rs
    fn handle_stream_readable(&mut self, stream_id: StreamId, now: Instant) -> Res<()> {
        match self
            .base_handler
            .handle_stream_readable(&mut self.conn, stream_id, now)?
        {
            ReceiveOutput::NewStream(NewStreamType::Push(push_id)) => {
                self.handle_new_push_stream(stream_id, push_id, now)
            }
            ReceiveOutput::NewStream(NewStreamType::Http(_)) => Err(Error::HttpStreamCreation),
            ReceiveOutput::NewStream(NewStreamType::WebTransportStream(session_id)) => {
                self.base_handler.webtransport_create_stream_remote(
                    StreamId::from(session_id),
                    stream_id,
                    Box::new(self.events.clone()),
                    Box::new(self.events.clone()),
                )?;
                let res =
                    self.base_handler
                        .handle_stream_readable(&mut self.conn, stream_id, now)?;
                debug_assert!(matches!(res, ReceiveOutput::NoOutput));
                Ok(())
            }
            ReceiveOutput::ControlFrames(control_frames) => {
                for f in control_frames {
                    match f {
                        HFrame::CancelPush { push_id } => self
                            .push_handler
                            .borrow_mut()
                            .handle_cancel_push(push_id, &mut self.conn, &mut self.base_handler),
                        HFrame::MaxPushId { .. }
                        | HFrame::PriorityUpdateRequest { .. }
                        | HFrame::PriorityUpdatePush { .. } => Err(Error::HttpFrameUnexpected),
                        HFrame::Goaway { stream_id } => self.handle_goaway(stream_id),
                        _ => {
                            unreachable!(
                                "we should only put MaxPushId, Goaway and PriorityUpdates into control_frames"
                            );
                        }
                    }?;
                }
                Ok(())
            }
            _ => Ok(()),
        }
    }

    fn handle_new_push_stream(
        &mut self,
        stream_id: StreamId,
        push_id: PushId,
        now: Instant,
    ) -> Res<()> {
        if !self.push_handler.borrow().can_receive_push() {
            return Err(Error::HttpId);
        }

        if !self
            .push_handler
            .borrow_mut()
            .add_new_push_stream(push_id, stream_id)?
        {
            drop(
                self.conn
                    .stream_stop_sending(stream_id, Error::HttpRequestCancelled.code()),
            );
            return Ok(());
        }

        self.base_handler.add_recv_stream(
            stream_id,
            Box::new(RecvMessage::new(
                &RecvMessageInfo {
                    message_type: MessageType::Response,
                    stream_type: Http3StreamType::Push,
                    stream_id,
                    first_frame_type: None,
                },
                Rc::clone(self.base_handler.qpack_decoder()),
                Box::new(RecvPushEvents::new(push_id, Rc::clone(&self.push_handler))),
                None,
                PriorityHandler::new(true, Priority::default()),
            )),
        );
        let res = self
            .base_handler
            .handle_stream_readable(&mut self.conn, stream_id, now)?;
        debug_assert!(matches!(res, ReceiveOutput::NoOutput));
        Ok(())
    }

    fn handle_goaway(&mut self, goaway_stream_id: StreamId) -> Res<()> {
        qinfo!("[{self}] handle_goaway {goaway_stream_id}");

        if goaway_stream_id.is_uni() || goaway_stream_id.is_server_initiated() {
            return Err(Error::HttpId);
        }

        match self.base_handler.state_mut() {
            Http3State::Connected => {
                self.base_handler
                    .set_state(Http3State::GoingAway(goaway_stream_id));
            }
            Http3State::GoingAway(stream_id) => {
                if goaway_stream_id > *stream_id {
                    return Err(Error::HttpGoaway);
                }
                *stream_id = goaway_stream_id;
            }
            Http3State::Closing(..) | Http3State::Closed(..) => {}
            _ => unreachable!("Should not receive Goaway frame in this state"),
        }

        let send_ids: Vec<StreamId> = self
            .base_handler
            .send_streams()
            .iter()
            .filter_map(id_gte(goaway_stream_id))
            .collect();
        for id in send_ids {
            drop(self.base_handler.handle_stream_stop_sending(
                id,
                Error::HttpRequestRejected.code(),
                &mut self.conn,
            ));
        }

        let recv_ids: Vec<StreamId> = self
            .base_handler
            .recv_streams()
            .iter()
            .filter_map(id_gte(goaway_stream_id))
            .collect();
        for id in recv_ids {
            drop(self.base_handler.handle_stream_reset(
                id,
                Error::HttpRequestRejected.code(),
                &mut self.conn,
            ));
        }

        self.events.goaway_received();

        Ok(())
    }

    #[must_use]
    pub fn qpack_encoder_stats(&self) -> QpackStats {
        self.base_handler.qpack_encoder().borrow().stats()
    }

    #[must_use]
    pub fn transport_stats(&self) -> TransportStats {
        self.conn.stats()
    }

    #[must_use]
    pub const fn webtransport_enabled(&self) -> bool {
        self.base_handler.webtransport_enabled()
    }
}

impl EventProvider for Http3Client {
    type Event = Http3ClientEvent;

    /// Return true if there are outstanding events.
    fn has_events(&self) -> bool {
        self.events.has_events()
    }

    /// Get events that indicate state changes on the connection. This method
    /// correctly handles cases where handling one event can obsolete
    /// previously-queued events, or cause new events to be generated.
    fn next_event(&mut self) -> Option<Self::Event> {
        self.events.next_event()
    }
}
