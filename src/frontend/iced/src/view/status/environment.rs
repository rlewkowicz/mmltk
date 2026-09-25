#[cfg(target_arch = "wasm32")]
thread_local! {
    static ACCESSIBLE_OUTPUT: std::cell::RefCell<Option<iced::futures::channel::mpsc::UnboundedSender<super::Message>>> = const { std::cell::RefCell::new(None) };
    static ACCESSIBLE_CALLBACKS: std::cell::RefCell<std::collections::HashMap<String, wasm_bindgen::closure::Closure<dyn FnMut(web_sys::Event)>>> = std::cell::RefCell::new(std::collections::HashMap::new());
}
#[cfg(target_arch = "wasm32")]
struct Listener {
    document: web_sys::Document,
    media: Option<web_sys::MediaQueryList>,
    callback: wasm_bindgen::closure::Closure<dyn FnMut(web_sys::Event)>,
}
#[cfg(target_arch = "wasm32")]
impl Drop for Listener {
    fn drop(&mut self) {
        use wasm_bindgen::JsCast;
        let _ = self.document.remove_event_listener_with_callback("visibilitychange", self.callback.as_ref().unchecked_ref());
        if let Some(media) = &self.media { let _ = media.remove_event_listener_with_callback("change", self.callback.as_ref().unchecked_ref()); }
        ACCESSIBLE_OUTPUT.with(|output| *output.borrow_mut() = None);
        if let Some(root) = self.document.get_element_by_id("status.accessibility") { root.remove(); }
        ACCESSIBLE_CALLBACKS.with(|callbacks| callbacks.borrow_mut().clear());
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct State { pub visible: bool, pub reduced_motion: bool }
impl Default for State { fn default() -> Self { Self { visible: true, reduced_motion: false } } }
#[cfg(not(target_arch = "wasm32"))]
pub fn subscription() -> iced::Subscription<super::Message> { iced::Subscription::none() }
#[cfg(target_arch = "wasm32")]
pub fn subscription() -> iced::Subscription<super::Message> {
    use iced::futures::{SinkExt, StreamExt};
    use wasm_bindgen::{JsCast, closure::Closure};
    iced::Subscription::run(|| iced::stream::channel(4, async |mut output| {
        let Some(window) = web_sys::window() else { return; };
        let Some(document) = window.document() else { return; };
        let media = window.match_media("(prefers-reduced-motion: reduce)").ok().flatten();
        let (sender, mut receiver) = iced::futures::channel::mpsc::unbounded();
        let read = || State { visible: !document.hidden(), reduced_motion: media.as_ref().is_some_and(|media| media.matches()) };
        let _ = output.send(super::Message::Environment(read())).await;
        ACCESSIBLE_OUTPUT.with(|output| *output.borrow_mut() = Some(sender.clone()));
        let event_document = document.clone(); let event_media = media.clone();
        let callback = Closure::<dyn FnMut(web_sys::Event)>::new(move |_| {
            let _ = sender.unbounded_send(super::Message::Environment(State { visible: !event_document.hidden(), reduced_motion: event_media.as_ref().is_some_and(|media| media.matches()) }));
        });
        let _ = document.add_event_listener_with_callback("visibilitychange", callback.as_ref().unchecked_ref());
        if let Some(media) = &media { let _ = media.add_event_listener_with_callback("change", callback.as_ref().unchecked_ref()); }
        let _listener = Listener { document, media, callback };
        while let Some(message) = receiver.next().await { if output.send(message).await.is_err() { break; } }
    }))
}

#[cfg(not(target_arch = "wasm32"))]
pub fn sync(_component: &super::Component, _notices: &crate::view_model::notices::NoticeStore) {}

// Canvas controls expose a stable semantic tree alongside their Iced focus and
// hit-test identities. These nodes never cover or intercept the drawn controls.
#[cfg(target_arch = "wasm32")]
pub fn sync(component: &super::Component, notices: &crate::view_model::notices::NoticeStore) {
    use wasm_bindgen::JsCast;
    thread_local! { static SIGNATURE: std::cell::RefCell<Vec<(u64, u64)>> = const { std::cell::RefCell::new(Vec::new()) }; }
    let mut signature = vec![(u64::from(component.open), notices.len() as u64)];
    signature.extend(notices.rows().map(|notice| (notice.id.0, notice.content_version)));
    let Some(document) = web_sys::window().and_then(|window| window.document()) else { return; };
    if document.get_element_by_id("status.accessibility").is_some() && SIGNATURE.with(|prior| *prior.borrow() == signature) { return; }
    let root = if let Some(root) = document.get_element_by_id("status.accessibility") { root }
    else {
        let Ok(root) = document.create_element("section") else { return; };
        root.set_id("status.accessibility");
        let _ = root.set_attribute("aria-label", "Status notifications");
        let _ = root.set_attribute("style", "position:absolute;width:1px;height:1px;overflow:hidden;clip-path:inset(50%);white-space:pre-wrap");
        if let Some(body) = document.body() { let _ = body.append_child(&root); }
        root
    };
    let mut controls = component.controls(notices);
    controls.push(super::Control::Settings);
    let identities: Vec<_> = controls.iter().map(|control| format!("accessible.{}", control.id())).collect();
    ACCESSIBLE_CALLBACKS.with(|callbacks| callbacks.borrow_mut().retain(|id, _| {
        if identities.contains(id) { true } else { if let Some(element) = document.get_element_by_id(id) { element.remove(); } false }
    }));
    for (control, identity) in controls.into_iter().zip(identities) {
        let existing = document.get_element_by_id(&identity);
        let is_new = existing.is_none();
        let Some(button) = existing.or_else(|| document.create_element("button").ok()) else { continue; };
        button.set_id(&identity);
        let _ = button.set_attribute("aria-label", &control.name());
        let _ = button.set_attribute("tabindex", "-1");
        let _ = button.set_attribute("data-iced-control", &control.id());
        button.set_text_content(Some(&control.name()));
        if control == super::Control::Trigger { let _ = button.set_attribute("aria-expanded", if component.open { "true" } else { "false" }); }
        if let super::Control::Copy(id) | super::Control::Detail(id) | super::Control::Dismiss(id) = control {
            if let Some(notice) = notices.get(id) { let _ = button.set_attribute("aria-description", &format!("{}\n\n{}", notice.title, notice.detail)); }
        }
        if is_new {
        let callback = wasm_bindgen::closure::Closure::<dyn FnMut(web_sys::Event)>::new(move |_| {
            ACCESSIBLE_OUTPUT.with(|output| { if let Some(output) = output.borrow().as_ref() { let _ = output.unbounded_send(super::Message::Activate(control, super::Opening::Keyboard)); } });
        });
        let _ = button.add_event_listener_with_callback("click", callback.as_ref().unchecked_ref());
        ACCESSIBLE_CALLBACKS.with(|callbacks| callbacks.borrow_mut().insert(identity, callback));
            let _ = root.append_child(&button);
        }
    }
    SIGNATURE.with(|prior| *prior.borrow_mut() = signature);
}
