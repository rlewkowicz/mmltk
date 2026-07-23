use crate::core::clipboard::{Content, Error, Kind};

pub use platform::*;

impl Default for Clipboard {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(not(target_arch = "wasm32"))]
mod platform {
    use super::*;

    use std::sync::{Arc, Mutex};
    use std::thread;

            pub struct Clipboard {
        state: State,
    }

    enum State {
        Connected {
            clipboard: Arc<Mutex<arboard::Clipboard>>,
        },
        Unavailable,
    }

    impl Clipboard {
                pub fn new() -> Self {
            let clipboard = arboard::Clipboard::new();

            let state = match clipboard {
                Ok(clipboard) => State::Connected {
                    clipboard: Arc::new(Mutex::new(clipboard)),
                },
                Err(_) => State::Unavailable,
            };

            Clipboard { state }
        }

                pub fn read(
            &self,
            kind: Kind,
            callback: impl FnOnce(Result<Content, Error>) + Send + 'static,
        ) {
            let State::Connected { clipboard } = &self.state else {
                callback(Err(Error::ClipboardUnavailable));
                return;
            };

            let clipboard = clipboard.clone();

            let _ = thread::spawn(move || {
                let Ok(mut clipboard) = clipboard.lock() else {
                    callback(Err(Error::ClipboardUnavailable));
                    return;
                };

                let get = clipboard.get();

                let result = match kind {
                    Kind::Text => get.text().map(Content::Text),
                    Kind::Html => get.html().map(Content::Html),
                    #[cfg(feature = "image")]
                    Kind::Image => get.image().map(|image| {
                        let rgba = crate::core::Bytes::from_owner(image.bytes);
                        let size = crate::core::Size {
                            width: image.width as u32,
                            height: image.height as u32,
                        };

                        Content::Image(crate::core::clipboard::Image { rgba, size })
                    }),
                    Kind::Files => get.file_list().map(Content::Files),
                    kind => {
                        log::warn!("unsupported clipboard kind: {kind:?}");

                        Err(arboard::Error::ContentNotAvailable)
                    }
                }
                .map_err(to_error);

                callback(result);
            });
        }

                pub fn write(
            &mut self,
            content: Content,
            callback: impl FnOnce(Result<(), Error>) + Send + 'static,
        ) {
            let State::Connected { clipboard } = &self.state else {
                callback(Err(Error::ClipboardUnavailable));
                return;
            };

            let clipboard = clipboard.clone();

            let _ = thread::spawn(move || {
                let Ok(mut clipboard) = clipboard.lock() else {
                    callback(Err(Error::ClipboardUnavailable));
                    return;
                };

                let set = clipboard.set();

                let result = match content {
                    Content::Text(text) => set.text(text),
                    Content::Html(html) => set.html(html, None),
                    #[cfg(feature = "image")]
                    Content::Image(image) => set.image(arboard::ImageData {
                        bytes: image.rgba.as_ref().into(),
                        width: image.size.width as usize,
                        height: image.size.height as usize,
                    }),
                    Content::Files(files) => set.file_list(&files),
                    content => {
                        log::warn!("unsupported clipboard content: {content:?}");

                        Err(arboard::Error::ClipboardNotSupported)
                    }
                }
                .map_err(to_error);

                callback(result);
            });
        }
    }

    fn to_error(error: arboard::Error) -> Error {
        match error {
            arboard::Error::ContentNotAvailable => Error::ContentNotAvailable,
            arboard::Error::ClipboardNotSupported => Error::ClipboardUnavailable,
            arboard::Error::ClipboardOccupied => Error::ClipboardOccupied,
            arboard::Error::ConversionFailure => Error::ConversionFailure,
            arboard::Error::Unknown { description } => Error::Unknown {
                description: Arc::new(description),
            },
            error => Error::Unknown {
                description: Arc::new(error.to_string()),
            },
        }
    }
}

#[cfg(target_arch = "wasm32")]
mod platform {
    use super::*;
    use js_sys::{Function, Promise, Reflect};
    use std::sync::Arc;
    use wasm_bindgen::{JsCast, JsValue};
    use wasm_bindgen_futures::{JsFuture, spawn_local};

            pub struct Clipboard;

    impl Clipboard {
                pub fn new() -> Self {
            Self
        }

                pub fn read(&self, kind: Kind, callback: impl FnOnce(Result<Content, Error>) + 'static) {
            if kind != Kind::Text {
                callback(Err(Error::ContentNotAvailable));
                return;
            }

            let Some(window) = web_sys::window() else {
                callback(Err(Error::ClipboardUnavailable));
                return;
            };
            let clipboard = match clipboard_api(&window.navigator()) {
                Ok(clipboard) => clipboard,
                Err(error) => {
                    callback(Err(browser_error("access", error)));
                    return;
                }
            };
            let promise = match clipboard_promise(&clipboard, "readText", None) {
                Ok(promise) => promise,
                Err(error) => {
                    callback(Err(browser_error("read", error)));
                    return;
                }
            };

            spawn_local(async move {
                let result = JsFuture::from(promise)
                    .await
                    .map_err(|error| browser_error("read", error))
                    .and_then(|value| {
                        value
                            .as_string()
                            .map(Content::Text)
                            .ok_or(Error::ConversionFailure)
                    });
                callback(result);
            });
        }

                pub fn write(
            &mut self,
            content: Content,
            callback: impl FnOnce(Result<(), Error>) + 'static,
        ) {
            let Content::Text(text) = content else {
                callback(Err(Error::Unknown {
                    description: Arc::new(
                        "the browser clipboard backend only supports text".to_owned(),
                    ),
                }));
                return;
            };
            let Some(window) = web_sys::window() else {
                callback(Err(Error::ClipboardUnavailable));
                return;
            };
            let clipboard = match clipboard_api(&window.navigator()) {
                Ok(clipboard) => clipboard,
                Err(error) => {
                    callback(Err(browser_error("access", error)));
                    return;
                }
            };
            let promise = match clipboard_promise(&clipboard, "writeText", Some(&text)) {
                Ok(promise) => promise,
                Err(error) => {
                    callback(Err(browser_error("write", error)));
                    return;
                }
            };

            spawn_local(async move {
                let result = JsFuture::from(promise)
                    .await
                    .map(|_| ())
                    .map_err(|error| browser_error("write", error));
                callback(result);
            });
        }
    }

    fn browser_error(operation: &str, error: JsValue) -> Error {
        let rejection = error.as_string().unwrap_or_else(|| format!("{error:?}"));
        Error::Unknown {
            description: Arc::new(format!(
                "browser clipboard {operation} rejected: {rejection}"
            )),
        }
    }

    fn clipboard_api(navigator: &web_sys::Navigator) -> Result<JsValue, JsValue> {
        let clipboard = Reflect::get(navigator.as_ref(), &JsValue::from_str("clipboard"))?;
        if clipboard.is_null() || clipboard.is_undefined() {
            Err(JsValue::from_str("navigator.clipboard is unavailable"))
        } else {
            Ok(clipboard)
        }
    }

    fn clipboard_promise(
        clipboard: &JsValue,
        method: &str,
        text: Option<&str>,
    ) -> Result<Promise, JsValue> {
        let function =
            Reflect::get(clipboard, &JsValue::from_str(method))?.dyn_into::<Function>()?;
        let value = match text {
            Some(text) => function.call1(clipboard, &JsValue::from_str(text))?,
            None => function.call0(clipboard)?,
        };
        value.dyn_into::<Promise>()
    }
}
