use crate::protocol::{self, ServerRecord};
use crate::transport_connection::Connection;

#[cfg(target_arch = "wasm32")]
use iced::Subscription;
#[cfg(target_arch = "wasm32")]
use iced::futures::{SinkExt, StreamExt};

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
}

impl TransportConfig {
    #[cfg(target_arch = "wasm32")]
    pub fn from_page() -> Self {
        let search = web_sys::window()
            .and_then(|window| window.location().search().ok())
            .unwrap_or_default();
        let params = web_sys::UrlSearchParams::new_with_str(&search).ok();
        let flag = |name| {
            params.as_ref().and_then(|params| params.get(name)).is_some_and(|value| value == "1")
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
        }
    }
}

#[derive(Debug, Clone)]
pub enum TransportEvent {
    Connected(Connection),
    Bootstrap(protocol::Bootstrap),
    IntentReply(protocol::IntentReply),
    SystemEvent(protocol::SystemEvent),
    Disconnected(String),
    ProtocolError(String),
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
    iced::stream::channel(64, async move |mut output| {
        if config.websocket_url.is_empty() {
            let _ = output
                .send(TransportEvent::ProtocolError(
                    "browser page is missing mmltk_ws_url".into(),
                ))
                .await;
            return;
        }
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
                    gloo_timers::future::TimeoutFuture::new(250).await;
                    continue;
                }
            };
            let (mut writer, reader) = socket.split();
            let (sender, outbound) = futures_channel::mpsc::channel(64);
            if output
                .send(TransportEvent::Connected(Connection::new(sender)))
                .await
                .is_err()
            {
                return;
            }
            let mut reader = reader.fuse();
            let mut outbound = outbound.fuse();
            let reason = loop {
                iced::futures::select! {
                    received = reader.next() => match received {
                        Some(Ok(gloo_net::websocket::Message::Bytes(bytes))) => {
                            let event = parse_inbound(&bytes);
                            let invalid = matches!(
                                event,
                                TransportEvent::ProtocolError(_)
                            );
                            if output.send(event).await.is_err() {
                                return;
                            }
                            if invalid {
                                break "browser host sent an invalid browser protocol record".into();
                            }
                        }
                        Some(Ok(gloo_net::websocket::Message::Text(_))) => {
                            break "browser host sent a text WebSocket message".into();
                        }
                        Some(Err(error)) => {
                            break format!("WebSocket receive failed: {error:?}");
                        }
                        None => break "WebSocket closed by host".into(),
                    },
                    record = outbound.next() => match record {
                        Some(record) => {
                            let encoded = match record.encode() {
                                Ok(encoded) => encoded,
                                Err(error) => {
                                    if output.send(TransportEvent::ProtocolError(error.to_string())).await.is_err() {
                                        return;
                                    }
                                    continue;
                                }
                            };
                            if let Err(error) = writer
                                .send(gloo_net::websocket::Message::Bytes(encoded))
                                .await
                            {
                                break format!("WebSocket send failed: {error:?}");
                            }
                        }
                        None => break "browser connection sender closed".into(),
                    }
                }
            };
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

fn parse_inbound(encoded: &[u8]) -> TransportEvent {
    match protocol::decode_server(encoded) {
        Ok(ServerRecord::Bootstrap(record)) => TransportEvent::Bootstrap(record),
        Ok(ServerRecord::IntentReply(record)) => TransportEvent::IntentReply(record),
        Ok(ServerRecord::SystemEvent(record)) => TransportEvent::SystemEvent(record),
        Err(error) => TransportEvent::ProtocolError(format!("invalid server CBOR record: {error}")),
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
    }
}
