use crate::protocol::{self, ServerRecord};
use crate::transport_connection::Connection;

#[cfg(target_arch = "wasm32")]
use iced::Subscription;
#[cfg(target_arch = "wasm32")]
use iced::futures::{FutureExt, Sink, SinkExt, StreamExt};

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct TransportConfig {
    pub websocket_url: String,
    pub integration: bool,
    pub surface_trace: bool,
    pub pixel_trace: bool,
    pub integration_pixel_fixture: bool,
    pub integration_window_close: bool,
    pub integration_dataset_source: String,
    pub integration_compiled_directory: String,
    pub integration_resolution: String,
    pub integration_viewer_scenario: String,
    pub integration_square_source: String,
    pub integration_square_compiled: String,
}

impl TransportConfig {
    #[cfg(target_arch = "wasm32")]
    pub fn from_page() -> Self {
        let search = web_sys::window()
            .and_then(|window| window.location().search().ok())
            .unwrap_or_default();
        let params = web_sys::UrlSearchParams::new_with_str(&search).ok();
        let flag = |name| {
            params
                .as_ref()
                .and_then(|params| params.get(name))
                .is_some_and(|value| value == "1")
        };
        let integration = flag("mmltk_integration");
        let surface_trace = flag("mmltk_surface_trace");
        Self {
            websocket_url: params
                .as_ref()
                .and_then(|params| params.get("mmltk_ws_url"))
                .unwrap_or_default(),
            integration,
            surface_trace,
            pixel_trace: surface_trace && flag("mmltk_pixel_trace"),
            integration_pixel_fixture: integration && flag("mmltk_integration_pixel_fixture"),
            integration_window_close: params
                .as_ref()
                .and_then(|params| params.get("mmltk_integration_window_close"))
                .is_some_and(|value| value == "1"),
            integration_dataset_source: params
                .as_ref()
                .and_then(|params| params.get("mmltk_integration_dataset_source"))
                .unwrap_or_default(),
            integration_compiled_directory: params
                .as_ref()
                .and_then(|params| params.get("mmltk_integration_compiled_directory"))
                .unwrap_or_default(),
            integration_resolution: params
                .as_ref()
                .and_then(|params| params.get("mmltk_integration_resolution"))
                .unwrap_or_default(),
            integration_viewer_scenario: params
                .as_ref()
                .and_then(|params| params.get("mmltk_integration_viewer_scenario"))
                .unwrap_or_default(),
            integration_square_source: params
                .as_ref()
                .and_then(|params| params.get("mmltk_integration_square_source"))
                .unwrap_or_default(),
            integration_square_compiled: params
                .as_ref()
                .and_then(|params| params.get("mmltk_integration_square_compiled"))
                .unwrap_or_default(),
        }
    }

    #[cfg(not(target_arch = "wasm32"))]
    pub fn from_page() -> Self {
        Self {
            websocket_url: String::new(),
            integration: false,
            surface_trace: false,
            pixel_trace: false,
            integration_pixel_fixture: false,
            integration_window_close: false,
            integration_dataset_source: String::new(),
            integration_compiled_directory: String::new(),
            integration_resolution: String::new(),
            integration_viewer_scenario: String::new(),
            integration_square_source: String::new(),
            integration_square_compiled: String::new(),
        }
    }
}

#[derive(Debug, Clone)]
pub enum TransportEvent {
    Connected(Connection),
    Bootstrap(protocol::Bootstrap),
    IntentReply(protocol::IntentReply),
    SystemEvent(protocol::SystemEvent),
    IntegrationControl(crate::generated::IntegrationControlReceipt),
    IntegrationInputSettled,
    Disconnected(String),
    ProtocolError(String),
    Rejected(String),
}

#[cfg(target_arch = "wasm32")]
pub fn subscription(config: TransportConfig) -> Subscription<TransportEvent> {
    Subscription::run_with(config, worker)
}

#[cfg(all(test, not(target_arch = "wasm32")))]
pub fn subscription(_config: TransportConfig) -> iced::Subscription<TransportEvent> {
    iced::Subscription::none()
}

#[cfg(target_arch = "wasm32")]
fn worker(
    config: &TransportConfig,
) -> impl iced::futures::Stream<Item = TransportEvent> + 'static + use<> {
    let config = config.clone();
    // futures MPSC reserves one slot for its sole sender; zero extra slots is one handoff.
    iced::stream::channel(0, async move |mut output| {
        if config.websocket_url.is_empty() {
            let _ = output
                .send(TransportEvent::ProtocolError(
                    "browser page is missing mmltk_ws_url".into(),
                ))
                .await;
            return;
        }
        loop {
            let (sender, mut outbound) = futures_channel::mpsc::channel(1);
            let mut connection = Connection::new(sender);
            connection.enable_integration(config.integration);
            let (socket, mut incoming) = match Socket::open(&config.websocket_url) {
                Ok(value) => value,
                Err(error) => {
                    if output
                        .send(TransportEvent::Disconnected(error))
                        .await
                        .is_err()
                    {
                        return;
                    }
                    gloo_timers::future::TimeoutFuture::new(250).await;
                    continue;
                }
            };
            if output
                .send(TransportEvent::Connected(connection.clone()))
                .await
                .is_err()
            {
                return;
            }
            let mut pending_events = std::collections::VecDeque::with_capacity(64);
            let mut quiet_settlement_pending = config.integration && config.integration_viewer_scenario == "quiet";
            let reason = loop {
                let has_pending = !pending_events.is_empty();
                let event = {
                    let writable = async {
                        if !has_pending {
                            iced::futures::future::pending::<()>().await;
                        }
                        iced::futures::future::poll_fn(|context| {
                            std::pin::Pin::new(&mut output).poll_ready(context)
                        })
                        .await
                    }
                    .fuse();
                    iced::futures::pin_mut!(writable);
                    iced::futures::select! {
                        ready = writable => Some(if ready.is_ok() { SocketEvent::Deliver } else { SocketEvent::Closed("application consumer closed".into()) }),
                        incoming = incoming.next().fuse() => incoming,
                        outbound = outbound.next().fuse() => {
                            if outbound.is_none() { break "browser connection sender closed".to_owned(); }
                            Some(SocketEvent::Writable)
                        }
                    }
                };
                if connection.is_closed() {
                    break "browser connection closed".into();
                }
                match event {
                    Some(SocketEvent::Bytes(bytes)) => {
                        let record = match protocol::decode_server(&bytes) {
                            Ok(record) => record,
                            Err(error) => break format!("invalid server CBOR record: {error}"),
                        };
                        if let Err(error) = connection.observe(&record) {
                            break error;
                        }
                        if quiet_settlement_pending && connection.integration_pressure_settled() {
                            quiet_settlement_pending = false;
                            if let Err(error) = retain_event(&mut pending_events, TransportEvent::IntegrationInputSettled) {
                                break error;
                            }
                        }
                        // Credits are reduced independently of the consumer. Every
                        // ordinary event enters the coalescer before the sole handoff.
                        if let Some(event) = application_event(record) {
                            if let Err(error) = retain_event(&mut pending_events, event) {
                                break error;
                            }
                        }
                    }
                    Some(SocketEvent::Deliver) => {
                        if let Some(event) = pending_events.pop_front() {
                            if output.try_send(event).is_err() {
                                break "application consumer closed".into();
                            }
                        }
                    }
                    Some(SocketEvent::Writable) => {}
                    Some(SocketEvent::Closed(reason)) => break reason,
                    None => break "WebSocket callback stream closed".into(),
                }
                if socket.socket.ready_state() == web_sys::WebSocket::OPEN {
                    if let Err(error) = connection.flush(|bytes| socket.send(bytes)) {
                        break error;
                    }
                }
            };
            connection.close();
            drop(socket);
            // Accepted events retain their delivery order before the terminal edge.
            while let Some(event) = pending_events.pop_front() {
                if output.send(event).await.is_err() {
                    return;
                }
            }
            if output
                .send(TransportEvent::Disconnected(reason))
                .await
                .is_err()
            {
                return;
            }
            gloo_timers::future::TimeoutFuture::new(250).await;
        }
    })
}

fn application_event(record: ServerRecord) -> Option<TransportEvent> {
    match record {
        ServerRecord::Bootstrap(record) => Some(TransportEvent::Bootstrap(record)),
        ServerRecord::IntentReply(record) => Some(TransportEvent::IntentReply(record)),
        ServerRecord::SystemEvent(record) => Some(TransportEvent::SystemEvent(record)),
        ServerRecord::InputProgress(_) => None,
        ServerRecord::IntegrationControl(record) => Some(TransportEvent::IntegrationControl(record.receipt)),
        ServerRecord::InteractionRejected(record) => {
            Some(TransportEvent::Rejected(record.error.detail))
        }
    }
}

#[cfg(test)]
fn parse_inbound(encoded: &[u8]) -> TransportEvent {
    match protocol::decode_server(encoded) {
        Ok(record) => application_event(record)
            .unwrap_or_else(|| TransportEvent::ProtocolError("unexpected progress fixture".into())),
        Err(error) => TransportEvent::ProtocolError(format!("invalid server CBOR record: {error}")),
    }
}

#[cfg(any(target_arch = "wasm32", test))]
fn retain_event(
    pending: &mut std::collections::VecDeque<TransportEvent>,
    event: TransportEvent,
) -> Result<(), String> {
    if let TransportEvent::SystemEvent(next) = &event {
        if next.delivery == crate::generated::EventDelivery::LatestState {
            if let Some((index, revision)) =
                pending
                    .iter()
                    .enumerate()
                    .find_map(|(index, prior)| match prior {
                        TransportEvent::SystemEvent(prior)
                            if prior.delivery == crate::generated::EventDelivery::LatestState
                                && std::mem::discriminant(&prior.event)
                                    == std::mem::discriminant(&next.event) =>
                        {
                            Some((index, prior.state_revision))
                        }
                        _ => None,
                    })
            {
                if next.state_revision <= revision {
                    return Ok(());
                }
                pending.remove(index);
            }
        } else if next.delivery == crate::generated::EventDelivery::Transient && pending.len() == 64
        {
            return Ok(());
        }
    }
    if pending.len() == 64 {
        return Err("essential application event continuity capacity exceeded".into());
    }
    pending.push_back(event);
    Ok(())
}

#[cfg(any(target_arch = "wasm32", test))]
#[derive(Debug)]
enum SocketEvent {
    Writable,
    #[cfg(target_arch = "wasm32")]
    Deliver,
    Bytes(Vec<u8>),
    Closed(String),
}

#[cfg(any(target_arch = "wasm32", test))]
#[derive(Default)]
struct SocketIngress {
    bytes: std::collections::VecDeque<Vec<u8>>,
    byte_count: usize,
    terminal: Option<String>,
    writable: bool,
    waker: Option<std::task::Waker>,
}
#[cfg(any(target_arch = "wasm32", test))]
#[derive(Clone, Default)]
struct SocketEvents(std::sync::Arc<std::sync::Mutex<SocketIngress>>);
#[cfg(any(target_arch = "wasm32", test))]
impl SocketEvents {
    fn notify(state: &mut SocketIngress) {
        if let Some(waker) = state.waker.take() {
            waker.wake();
        }
    }
    fn close(&self, reason: String) {
        let mut state = self.0.lock().expect("socket ingress");
        state.terminal.get_or_insert(reason);
        Self::notify(&mut state);
    }
    fn writable(&self) {
        let mut state = self.0.lock().expect("socket ingress");
        state.writable = true;
        Self::notify(&mut state);
    }
    fn bytes(&self, length: usize, copy: impl FnOnce() -> Vec<u8>) -> bool {
        let mut state = self.0.lock().expect("socket ingress");
        if state.terminal.is_some() {
            return false;
        }
        if length == 0
            || length > crate::generated::MAX_RECORD_WIRE_BYTES.saturating_sub(state.byte_count)
            || state.bytes.len() == 64
        {
            state.terminal = Some("WebSocket ingress byte or record capacity exceeded".into());
            Self::notify(&mut state);
            return false;
        }
        state.bytes.push_back(copy());
        state.byte_count += length;
        Self::notify(&mut state);
        true
    }
}
#[cfg(any(target_arch = "wasm32", test))]
impl iced::futures::Stream for SocketEvents {
    type Item = SocketEvent;
    fn poll_next(
        self: std::pin::Pin<&mut Self>,
        context: &mut std::task::Context<'_>,
    ) -> std::task::Poll<Option<Self::Item>> {
        let mut state = self.0.lock().expect("socket ingress");
        if state.terminal.is_none() && state.writable {
            state.writable = false;
            return std::task::Poll::Ready(Some(SocketEvent::Writable));
        }
        if let Some(bytes) = state.bytes.pop_front() {
            state.byte_count -= bytes.len();
            return std::task::Poll::Ready(Some(SocketEvent::Bytes(bytes)));
        }
        if let Some(reason) = &state.terminal {
            return std::task::Poll::Ready(Some(SocketEvent::Closed(reason.clone())));
        }
        state.waker = Some(context.waker().clone());
        std::task::Poll::Pending
    }
}

#[cfg(target_arch = "wasm32")]
struct Socket {
    socket: web_sys::WebSocket,
    _open: wasm_bindgen::closure::Closure<dyn FnMut(web_sys::Event)>,
    _message: wasm_bindgen::closure::Closure<dyn FnMut(web_sys::MessageEvent)>,
    _error: wasm_bindgen::closure::Closure<dyn FnMut(web_sys::Event)>,
    _close: wasm_bindgen::closure::Closure<dyn FnMut(web_sys::CloseEvent)>,
}

#[cfg(target_arch = "wasm32")]
impl Socket {
    fn open(url: &str) -> Result<(Self, SocketEvents), String> {
        use wasm_bindgen::{JsCast, closure::Closure};
        let socket = web_sys::WebSocket::new(url)
            .map_err(|error| format!("WebSocket open failed: {error:?}"))?;
        socket.set_binary_type(web_sys::BinaryType::Arraybuffer);
        let receiver = SocketEvents::default();
        let opened = receiver.clone();
        let open = Closure::wrap(Box::new(move |_: web_sys::Event| {
            opened.writable();
        }) as Box<dyn FnMut(_)>);
        let messages = receiver.clone();
        let message_socket = socket.clone();
        let message = Closure::wrap(Box::new(move |event: web_sys::MessageEvent| {
            let accepted = if let Ok(buffer) = event.data().dyn_into::<js_sys::ArrayBuffer>() {
                messages.bytes(buffer.byte_length() as usize, || {
                    js_sys::Uint8Array::new(&buffer).to_vec()
                })
            } else {
                messages.close("browser host sent a non-binary WebSocket message".into());
                false
            };
            if !accepted {
                let _ =
                    message_socket.close_with_code_and_reason(1002, "invalid or excess ingress");
            }
        }) as Box<dyn FnMut(_)>);
        let errors = receiver.clone();
        let error = Closure::wrap(Box::new(move |_: web_sys::Event| {
            errors.close("WebSocket failed".into());
        }) as Box<dyn FnMut(_)>);
        let closed = receiver.clone();
        let close = Closure::wrap(Box::new(move |event: web_sys::CloseEvent| {
            closed.close(format!(
                "WebSocket closed: {} {}",
                event.code(),
                event.reason()
            ));
        }) as Box<dyn FnMut(_)>);
        socket.set_onopen(Some(open.as_ref().unchecked_ref()));
        socket.set_onmessage(Some(message.as_ref().unchecked_ref()));
        socket.set_onerror(Some(error.as_ref().unchecked_ref()));
        socket.set_onclose(Some(close.as_ref().unchecked_ref()));
        Ok((
            Self {
                socket,
                _open: open,
                _message: message,
                _error: error,
                _close: close,
            },
            receiver,
        ))
    }
    fn send(&self, bytes: &[u8]) -> Result<(), String> {
        self.socket
            .send_with_u8_array(bytes)
            .map_err(|error| format!("WebSocket send failed: {error:?}"))
    }
}
#[cfg(target_arch = "wasm32")]
impl Drop for Socket {
    fn drop(&mut self) {
        self.socket.set_onopen(None);
        self.socket.set_onmessage(None);
        self.socket.set_onerror(None);
        self.socket.set_onclose(None);
        let _ = self.socket.close();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::application_codec::Value;
    use crate::protocol::{encode_envelope, object};

    #[test]
    fn reconnect_configuration_carries_only_the_socket_boundary() {
        let config = TransportConfig::from_page();
        assert!(config.websocket_url.is_empty());
        assert!(!config.integration);
        assert!(!config.integration_window_close);
    }

    #[test]
    fn inbound_protocol_failures_are_terminal_for_the_current_peer() {
        let invalid = encode_envelope(
            "Unknown",
            &object([(
                "protocol_version",
                Value::Unsigned(crate::generated::BROWSER_PROTOCOL_VERSION),
            )]),
        )
        .expect("fixture");
        assert!(matches!(
            parse_inbound(&invalid),
            TransportEvent::ProtocolError(_)
        ));
        use iced::futures::Stream;
        let mut ingress = SocketEvents::default();
        let waker = iced::futures::task::noop_waker();
        let mut context = std::task::Context::from_waker(&waker);
        assert!(
            std::pin::Pin::new(&mut ingress)
                .poll_next(&mut context)
                .is_pending()
        );
        assert!(ingress.bytes(1, || vec![0xa2]));
        ingress.writable();
        assert!(matches!(
            std::pin::Pin::new(&mut ingress).poll_next(&mut context),
            std::task::Poll::Ready(Some(SocketEvent::Writable))
        ));
        let std::task::Poll::Ready(Some(SocketEvent::Bytes(partial))) =
            std::pin::Pin::new(&mut ingress).poll_next(&mut context)
        else {
            panic!("queued bytes");
        };
        assert!(matches!(
            parse_inbound(&partial),
            TransportEvent::ProtocolError(_)
        ));
        assert!(ingress.bytes(crate::generated::MAX_RECORD_WIRE_BYTES, || {
            vec![0; crate::generated::MAX_RECORD_WIRE_BYTES]
        }));
        assert!(!ingress.bytes(1, || panic!("overflow must not copy browser memory")));
        ingress.close("later close cannot overwrite capacity failure".into());
        ingress.writable();
        assert!(
            matches!(std::pin::Pin::new(&mut ingress).poll_next(&mut context), std::task::Poll::Ready(Some(SocketEvent::Bytes(bytes))) if bytes.len() == crate::generated::MAX_RECORD_WIRE_BYTES)
        );
        assert!(
            matches!(std::pin::Pin::new(&mut ingress).poll_next(&mut context), std::task::Poll::Ready(Some(SocketEvent::Closed(reason))) if reason.contains("capacity"))
        );
        assert!(!ingress.bytes(1, || panic!("terminal peer cannot accept more bytes")));
        let mut records = SocketEvents::default();
        for value in 0..64 {
            assert!(records.bytes(1, || vec![value]));
        }
        assert!(!records.bytes(1, || panic!("record overflow must not allocate")));
        records.writable();
        for expected in 0..64 {
            assert!(
                matches!(std::pin::Pin::new(&mut records).poll_next(&mut context), std::task::Poll::Ready(Some(SocketEvent::Bytes(bytes))) if bytes == vec![expected])
            );
        }
        assert!(
            matches!(std::pin::Pin::new(&mut records).poll_next(&mut context), std::task::Poll::Ready(Some(SocketEvent::Closed(reason))) if reason.contains("capacity"))
        );
        let mut closed = SocketEvents::default();
        assert!(closed.bytes(1, || vec![1]));
        assert!(closed.bytes(1, || vec![2]));
        closed.close("WebSocket closed: 1001 going away".into());
        closed.close("later error".into());
        closed.writable();
        assert!(!closed.bytes(1, || panic!("terminal callback rejects before copy")));
        for expected in [1, 2] {
            assert!(
                matches!(std::pin::Pin::new(&mut closed).poll_next(&mut context), std::task::Poll::Ready(Some(SocketEvent::Bytes(bytes))) if bytes == vec![expected])
            );
        }
        for _ in 0..2 {
            assert!(
                matches!(std::pin::Pin::new(&mut closed).poll_next(&mut context), std::task::Poll::Ready(Some(SocketEvent::Closed(reason))) if reason == "WebSocket closed: 1001 going away")
            );
        }

        let snapshot = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .find_map(|fact| {
                if let crate::generated::ApplicationSnapshot::Annotation(value) = fact.value {
                    Some(value)
                } else {
                    None
                }
            })
            .unwrap();
        let state = |revision, delivery| {
            let mut snapshot = snapshot.clone();
            snapshot.revision = revision;
            TransportEvent::SystemEvent(crate::protocol::SystemEvent {
                delivery,
                state_revision: if delivery == crate::generated::EventDelivery::LatestState {
                    revision
                } else {
                    0
                },
                event: crate::generated::ApplicationEvent::AnnotationAnnotationChanged(
                    crate::generated::AnnotationChanged { snapshot },
                ),
            })
        };
        let edge = |correlation| {
            TransportEvent::IntentReply(crate::protocol::IntentReply {
                correlation,
                result: Ok(Value::Null),
            })
        };
        let drain_latest = |pending: &mut std::collections::VecDeque<TransportEvent>,
                            correlations: std::ops::Range<u64>| {
            for expected in correlations {
                assert!(
                    matches!(pending.pop_front(), Some(TransportEvent::IntentReply(reply)) if reply.correlation == expected)
                );
            }
            assert!(
                matches!(pending.pop_front(), Some(TransportEvent::SystemEvent(crate::protocol::SystemEvent {
                event: crate::generated::ApplicationEvent::AnnotationAnnotationChanged(changed), ..
            })) if changed.snapshot.revision == 130)
            );
        };
        let mut pending = std::collections::VecDeque::with_capacity(64);
        retain_event(
            &mut pending,
            state(1, crate::generated::EventDelivery::LatestState),
        )
        .unwrap();
        retain_event(&mut pending, edge(1)).unwrap();
        retain_event(
            &mut pending,
            state(2, crate::generated::EventDelivery::LatestState),
        )
        .unwrap();
        retain_event(&mut pending, edge(2)).unwrap();
        for revision in 3..=130 {
            retain_event(
                &mut pending,
                state(revision, crate::generated::EventDelivery::LatestState),
            )
            .unwrap();
            assert_eq!(
                pending.len(),
                3,
                "blocked delivery retains only the latest state and discrete edges"
            );
        }
        drain_latest(&mut pending, 1..3);
        retain_event(
            &mut pending,
            state(11, crate::generated::EventDelivery::LatestState),
        )
        .unwrap();
        retain_event(&mut pending, edge(3)).unwrap();
        for revision in [10, 11, 0] {
            let mut older = state(revision, crate::generated::EventDelivery::LatestState);
            if let TransportEvent::SystemEvent(crate::protocol::SystemEvent {
                event: crate::generated::ApplicationEvent::AnnotationAnnotationChanged(changed),
                ..
            }) = &mut older
            {
                changed.snapshot.busy = true;
            }
            retain_event(&mut pending, older).unwrap();
            assert_eq!(pending.len(), 2);
            assert!(
                matches!(pending.front(), Some(TransportEvent::SystemEvent(crate::protocol::SystemEvent {
                state_revision: 11, event: crate::generated::ApplicationEvent::AnnotationAnnotationChanged(changed), ..
            })) if changed.snapshot.revision == 11 && !changed.snapshot.busy)
            );
            assert!(
                matches!(pending.back(), Some(TransportEvent::IntentReply(reply)) if reply.correlation == 3)
            );
        }
        retain_event(
            &mut pending,
            state(12, crate::generated::EventDelivery::LatestState),
        )
        .unwrap();
        assert!(
            matches!(pending.pop_front(), Some(TransportEvent::IntentReply(reply)) if reply.correlation == 3)
        );
        assert!(matches!(
            pending.pop_front(),
            Some(TransportEvent::SystemEvent(crate::protocol::SystemEvent {
                state_revision: 12,
                ..
            }))
        ));
        for correlation in 0..63 {
            retain_event(&mut pending, edge(correlation)).unwrap();
        }
        for revision in 1..=130 {
            retain_event(
                &mut pending,
                state(revision, crate::generated::EventDelivery::LatestState),
            )
            .unwrap();
            assert_eq!(pending.len(), 64);
        }
        retain_event(
            &mut pending,
            state(131, crate::generated::EventDelivery::Transient),
        )
        .unwrap();
        assert_eq!(pending.len(), 64, "transient overflow is dropped");
        assert!(
            retain_event(
                &mut pending,
                state(132, crate::generated::EventDelivery::Critical)
            )
            .is_err()
        );
        drain_latest(&mut pending, 0..63);
    }
}
