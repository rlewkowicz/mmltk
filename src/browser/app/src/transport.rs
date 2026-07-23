use crate::generated::{HOST_API_PROTOCOL_VERSION, StateSnapshot, WorkspacePresent};
use crate::model::BridgeState;
use iced::Subscription;
use iced::futures::channel::mpsc;
use iced::futures::{FutureExt, SinkExt, StreamExt};
use serde_json::{Value, json};
use std::collections::{HashMap, VecDeque};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};

const EVENT_QUEUE_CAPACITY: usize = 64;
const OUTBOUND_QUEUE_CAPACITY: usize = 64;
const INITIAL_RECONNECT_DELAY_MS: u32 = 250;
const MAX_RECONNECT_DELAY_MS: u32 = 5_000;

static WORKSPACE_TRACE_ENABLED: AtomicBool = AtomicBool::new(false);

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct TransportConfig {
    pub websocket_url: String,
    pub trace_enabled: bool,
}

impl TransportConfig {
    pub fn from_page() -> Self {
        let search = web_sys::window()
            .and_then(|window| window.location().search().ok())
            .unwrap_or_default();
        let params = web_sys::UrlSearchParams::new_with_str(&search).ok();
        Self {
            websocket_url: params
                .as_ref()
                .and_then(|params| params.get("mmltk_ws_url"))
                .unwrap_or_default(),
            trace_enabled: params
                .as_ref()
                .and_then(|params| params.get("mmltk_trace"))
                .is_some_and(|value| value == "1"),
        }
    }
}

#[derive(Debug, Clone)]
struct OutboundMessage {
    encoded: String,
    workspace_release_id: Option<u64>,
}

#[derive(Debug, Clone)]
pub struct Connection(mpsc::Sender<OutboundMessage>);

impl Connection {
    pub fn send(&mut self, encoded: String) -> Result<(), &'static str> {
        self.send_recover(OutboundMessage {
            encoded,
            workspace_release_id: None,
        })
        .map_err(|_| "browser outbound queue is full")
    }

    fn send_recover(&mut self, message: OutboundMessage) -> Result<(), OutboundMessage> {
        self.0
            .try_send(message)
            .map_err(iced::futures::channel::mpsc::TrySendError::into_inner)
    }
}

#[derive(Debug, Clone)]
pub enum TransportEvent {
    Connected(Connection),
    Disconnected(String),
    Snapshot(StateSnapshot),
    Bridge(BridgeState),
    WorkspacePresent(WorkspacePresent),
    Error(String),
}

pub fn subscription(config: TransportConfig) -> Subscription<TransportEvent> {
    Subscription::run_with(config, worker)
}

fn worker(
    config: &TransportConfig,
) -> impl iced::futures::Stream<Item = TransportEvent> + 'static + use<> {
    let config = config.clone();
    iced::stream::channel(EVENT_QUEUE_CAPACITY, async move |mut output| {
        WORKSPACE_TRACE_ENABLED.store(config.trace_enabled, Ordering::Release);
        if config.websocket_url.is_empty() {
            let _ = output
                .send(TransportEvent::Error(
                    "browser page is missing mmltk_ws_url".into(),
                ))
                .await;
            return;
        }

        let mut reconnect_delay_ms = INITIAL_RECONNECT_DELAY_MS;
        loop {
            let socket = match gloo_net::websocket::futures::WebSocket::open(&config.websocket_url)
            {
                Ok(socket) => socket,
                Err(error) => {
                    if output
                        .send(TransportEvent::Disconnected(format!(
                            "WebSocket open failed: {error:?}"
                        )))
                        .await
                        .is_err()
                    {
                        return;
                    }
                    reconnect_delay(&mut reconnect_delay_ms).await;
                    continue;
                }
            };

            let (mut writer, mut reader) = socket.split();
            let navigator_gpu = navigator_has_webgpu();
            let ready = json!({"type": "host.callbacks.ready"}).to_string();
            let capabilities = json!({
                "type": "host.runtime.capabilities",
                "navigator_gpu": navigator_gpu,
                "workspace_surface_bridge": "unknown",
                "workspace_surface_zero_copy": "unknown",
                "capabilities": {
                    "renderer": {
                        "status": if navigator_gpu { "ready" } else { "blocked" },
                        "summary": if navigator_gpu { "WebGPU available" } else { "WebGPU unavailable" },
                        "detail": "iced is compiled with the browser WebGPU backend only"
                    },
                    "workspace_surface_bridge": {
                        "status": "pending",
                        "summary": "workspace negotiation pending",
                        "detail": "The native host publishes readiness after Firefox imports every DMA-BUF slot."
                    },
                    "workspace_surface_zero_copy": {
                        "status": "pending",
                        "summary": "timeline synchronization pending",
                        "detail": "The native host publishes readiness after CUDA imports every timeline semaphore."
                    }
                }
            })
            .to_string();

            if writer
                .send(gloo_net::websocket::Message::Text(ready))
                .await
                .is_err()
                || writer
                    .send(gloo_net::websocket::Message::Text(capabilities))
                    .await
                    .is_err()
            {
                if output
                    .send(TransportEvent::Disconnected(
                        "WebSocket handshake send failed".into(),
                    ))
                    .await
                    .is_err()
                {
                    return;
                }
                reconnect_delay(&mut reconnect_delay_ms).await;
                continue;
            }

            let (sender, mut receiver) = mpsc::channel(OUTBOUND_QUEUE_CAPACITY);
            if output
                .send(TransportEvent::Connected(Connection(sender)))
                .await
                .is_err()
            {
                return;
            }
            reconnect_delay_ms = INITIAL_RECONNECT_DELAY_MS;

            let disconnect_reason = loop {
                let incoming = reader.next().fuse();
                let outgoing = receiver.next().fuse();
                iced::futures::pin_mut!(incoming, outgoing);

                iced::futures::select! {
                    received = incoming => match received {
                        Some(Ok(gloo_net::websocket::Message::Text(encoded))) => {
                            if let Some(event) = parse_inbound(encoded) {
                                if output.send(event).await.is_err() {
                                    return;
                                }
                            }
                        }
                        Some(Ok(gloo_net::websocket::Message::Bytes(_))) => {
                            if output.send(TransportEvent::Error(
                                "browser host sent a binary WebSocket message".into()
                            )).await.is_err() {
                                return;
                            }
                        }
                        Some(Err(error)) => break format!("WebSocket receive failed: {error:?}"),
                        None => break "WebSocket closed by host".into(),
                    },
                    outbound = outgoing => match outbound {
                        Some(outbound) => {
                            let workspace_release_id = outbound.workspace_release_id;
                            if let Err(error) = writer
                                .send(gloo_net::websocket::Message::Text(outbound.encoded))
                                .await
                            {
                                break format!("WebSocket send failed: {error:?}");
                            }
                            if let Some(release_id) = workspace_release_id {
                                acknowledge_workspace_release(release_id);
                            } else {
                                flush_workspace_releases();
                            }
                        }
                        None => break "browser outbound channel closed".into(),
                    },
                }
            };

            if output
                .send(TransportEvent::Disconnected(disconnect_reason))
                .await
                .is_err()
            {
                return;
            }
            reconnect_delay(&mut reconnect_delay_ms).await;
        }
    })
}

fn parse_inbound(encoded: String) -> Option<TransportEvent> {
    let value: Value = match serde_json::from_str(&encoded) {
        Ok(value) => value,
        Err(error) => {
            return Some(TransportEvent::Error(format!(
                "invalid host JSON message: {error}"
            )));
        }
    };

    match value.get("type").and_then(Value::as_str) {
        Some("state.snapshot") => match serde_json::from_value(value) {
            Ok(snapshot) => Some(TransportEvent::Snapshot(snapshot)),
            Err(error) => Some(TransportEvent::Error(format!(
                "invalid state snapshot: {error}"
            ))),
        },
        Some("bridge.error") => Some(TransportEvent::Error(
            value
                .get("message")
                .and_then(Value::as_str)
                .unwrap_or("native bridge error")
                .to_owned(),
        )),
        Some("workspace.present") => match serde_json::from_value(value) {
            Ok(present) => Some(TransportEvent::WorkspacePresent(present)),
            Err(error) => Some(TransportEvent::Error(format!(
                "invalid workspace present: {error}"
            ))),
        },
        Some(other) => Some(TransportEvent::Error(format!(
            "unsupported host message type: {other}"
        ))),
        None if value.get("phase").is_some() => match serde_json::from_value(value) {
            Ok(bridge) => Some(TransportEvent::Bridge(bridge)),
            Err(error) => Some(TransportEvent::Error(format!(
                "invalid bridge state: {error}"
            ))),
        },
        None => Some(TransportEvent::Error(
            "host message has no recognized type".into(),
        )),
    }
}

#[derive(Debug)]
struct PendingWorkspaceRelease {
    id: u64,
    encoded: String,
    queued_connection: u64,
}

#[derive(Default)]
struct WorkspaceReleaseTransport {
    connection: Option<Connection>,
    connection_id: u64,
    next_release_id: u64,
    pending: VecDeque<PendingWorkspaceRelease>,
}

impl WorkspaceReleaseTransport {
    fn flush(&mut self) {
        let Some(connection) = self.connection.as_mut() else {
            return;
        };
        for release in &mut self.pending {
            if release.queued_connection == self.connection_id {
                continue;
            }
            let message = OutboundMessage {
                encoded: release.encoded.clone(),
                workspace_release_id: Some(release.id),
            };
            if connection.send_recover(message).is_err() {
                break;
            }
            release.queued_connection = self.connection_id;
        }
    }
}

fn release_transport() -> &'static Mutex<WorkspaceReleaseTransport> {
    static TRANSPORT: OnceLock<Mutex<WorkspaceReleaseTransport>> = OnceLock::new();
    TRANSPORT.get_or_init(|| Mutex::new(WorkspaceReleaseTransport::default()))
}

pub fn trace_workspace_surface(
    event: &'static str,
    generation: &str,
    slot: u32,
    revision: &str,
    ready_ns: Option<&str>,
    encoded: Option<bool>,
) {
    if !WORKSPACE_TRACE_ENABLED.load(Ordering::Acquire) {
        return;
    }
    let message = json!({
        "type": "intent",
        "protocol_version": HOST_API_PROTOCOL_VERSION,
        "request_id": 0,
        "workflow": "annotate",
        "intent": "ui.log",
        "payload": {
            "level": "trace",
            "event": event,
            "fields": {
                "generation": generation,
                "slot": slot,
                "revision": revision,
                "ready_ns": ready_ns,
                "encoded": encoded,
            },
        },
    })
    .to_string();
    if let Ok(mut transport) = release_transport().lock()
        && let Some(connection) = transport.connection.as_mut()
    {
        let _ = connection.send(message);
    }
}

pub fn set_workspace_release_connection(connection: Option<Connection>) {
    if let Ok(mut transport) = release_transport().lock() {
        if connection.is_some() {
            transport.connection_id = transport.connection_id.wrapping_add(1).max(1);
        }
        transport.connection = connection;
        transport.flush();
    }
}

fn acknowledge_workspace_release(release_id: u64) {
    if let Ok(mut transport) = release_transport().lock() {
        if let Some(index) = transport
            .pending
            .iter()
            .position(|release| release.id == release_id)
        {
            transport.pending.remove(index);
        }
        transport.flush();
    }
}

fn flush_workspace_releases() {
    if let Ok(mut transport) = release_transport().lock() {
        transport.flush();
    }
}

pub fn release_workspace_surface(generation: String, slot: u32, revision: String, encoded: bool) {
    let encoded_message = json!({
        "type": "workspace.release",
        "generation": &generation,
        "slot": slot,
        "revision": &revision,
        "encoded": encoded,
    })
    .to_string();
    if let Ok(mut transport) = release_transport().lock() {
        transport.next_release_id = transport.next_release_id.wrapping_add(1).max(1);
        let release_id = transport.next_release_id;
        transport.pending.push_back(PendingWorkspaceRelease {
            id: release_id,
            encoded: encoded_message,
            queued_connection: 0,
        });
        transport.flush();
    }
    trace_workspace_surface(
        "workspace.release_queued",
        &generation,
        slot,
        &revision,
        None,
        Some(encoded),
    );
}

type WorkspaceReleaseKey = (String, u32, String);

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
struct WorkspaceSubmissionState {
    outstanding: u32,
    retired: bool,
    encoded: bool,
}

fn workspace_submissions() -> &'static Mutex<HashMap<WorkspaceReleaseKey, WorkspaceSubmissionState>>
{
    static SUBMISSIONS: OnceLock<Mutex<HashMap<WorkspaceReleaseKey, WorkspaceSubmissionState>>> =
        OnceLock::new();
    SUBMISSIONS.get_or_init(|| Mutex::new(HashMap::new()))
}

pub fn mark_workspace_surface_submitted(generation: &str, slot: u32, revision: &str) -> bool {
    workspace_submissions().lock().is_ok_and(|mut submissions| {
        let key = (generation.to_owned(), slot, revision.to_owned());
        let submission = submissions.entry(key).or_default();
        if submission.retired {
            return false;
        }
        let Some(outstanding) = submission.outstanding.checked_add(1) else {
            return false;
        };
        submission.outstanding = outstanding;
        true
    })
}

pub fn confirm_workspace_surface_encoded(generation: &str, slot: u32, revision: &str) {
    if let Ok(mut submissions) = workspace_submissions().lock()
        && let Some(submission) =
            submissions.get_mut(&(generation.to_owned(), slot, revision.to_owned()))
    {
        submission.encoded = true;
    }
}

pub fn complete_workspace_surface_submission(generation: String, slot: u32, revision: String) {
    let key = (generation, slot, revision);
    let should_release = workspace_submissions()
        .lock()
        .ok()
        .is_some_and(|mut submissions| {
            let Some(submission) = submissions.get_mut(&key) else {
                return false;
            };
            if submission.outstanding == 0 {
                return false;
            }
            submission.outstanding -= 1;
            let finished = submission.retired && submission.outstanding == 0;
            if finished {
                submissions.remove(&key);
            }
            finished
        });
    trace_workspace_surface(
        "workspace.gpu_completed",
        &key.0,
        key.1,
        &key.2,
        None,
        Some(true),
    );
    if should_release {
        release_workspace_surface(key.0, key.1, key.2, true);
    }
}

pub fn cancel_workspace_surface_submission(generation: String, slot: u32, revision: String) {
    let key = (generation, slot, revision);
    let release = workspace_submissions()
        .lock()
        .ok()
        .and_then(|mut submissions| {
            let submission = submissions.get_mut(&key)?;
            if submission.outstanding == 0 {
                return None;
            }
            submission.outstanding -= 1;
            let finished = submission.retired && submission.outstanding == 0;
            let encoded = submission.encoded;
            if finished {
                submissions.remove(&key);
                Some(encoded)
            } else {
                None
            }
        });
    trace_workspace_surface(
        "workspace.encode_cancelled",
        &key.0,
        key.1,
        &key.2,
        None,
        Some(false),
    );
    if let Some(encoded) = release {
        release_workspace_surface(key.0, key.1, key.2, encoded);
    }
}

pub fn release_workspace_surface_if_unsubmitted(present: &WorkspacePresent) {
    if present.revision.is_empty() {
        return;
    }
    trace_workspace_surface(
        "workspace.retired",
        &present.generation,
        present.slot,
        &present.revision,
        Some(&present.ready_ns),
        None,
    );
    let key = (
        present.generation.clone(),
        present.slot,
        present.revision.clone(),
    );
    let release = workspace_submissions()
        .lock()
        .ok()
        .and_then(|mut submissions| {
            let Some(submission) = submissions.get_mut(&key) else {
                return Some(false);
            };
            if submission.retired {
                return None;
            }
            submission.retired = true;
            if submission.outstanding == 0 {
                let encoded = submission.encoded;
                submissions.remove(&key);
                Some(encoded)
            } else {
                None
            }
        });
    if let Some(encoded) = release {
        release_workspace_surface(key.0, key.1, key.2, encoded);
    }
}

async fn reconnect_delay(delay_ms: &mut u32) {
    gloo_timers::future::TimeoutFuture::new(*delay_ms).await;
    *delay_ms = delay_ms.saturating_mul(2).min(MAX_RECONNECT_DELAY_MS);
}

fn navigator_has_webgpu() -> bool {
    let Some(window) = web_sys::window() else {
        return false;
    };
    let navigator = window.navigator();
    js_sys::Reflect::get(navigator.as_ref(), &wasm_bindgen::JsValue::from_str("gpu"))
        .is_ok_and(|value| !value.is_null() && !value.is_undefined())
}
