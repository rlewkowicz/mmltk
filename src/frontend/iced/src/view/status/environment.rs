#[cfg(target_arch = "wasm32")]
thread_local! {
    static ACCESSIBLE_OUTPUT: std::cell::RefCell<Option<iced::futures::channel::mpsc::UnboundedSender<super::Message>>> = const { std::cell::RefCell::new(None) };
    static ACCESSIBLE_TREE: std::cell::RefCell<AccessibleTree> = std::cell::RefCell::new(AccessibleTree::default());
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
        ACCESSIBLE_TREE.with(|tree| *tree.borrow_mut() = AccessibleTree::default());
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
// hit-test identities. Detached popup nodes retain their text/callbacks while
// closed, but never participate in the browser's hidden-control traversal.
#[cfg(target_arch = "wasm32")]
struct AccessibleControl {
    element: web_sys::Element,
    callback: wasm_bindgen::closure::Closure<dyn FnMut(web_sys::Event)>,
    presentation: Option<crate::view_model::notices::Presentation>,
    present: bool,
}
#[cfg(target_arch = "wasm32")]
impl AccessibleControl {
    fn new(document: &web_sys::Document, control: super::Control) -> Option<Self> {
        use wasm_bindgen::JsCast;
        let element = document.create_element("button").ok()?;
        let id = control.id();
        let name = control.name();
        element.set_id(&format!("accessible.{id}"));
        element.set_attribute("aria-label", &name).ok()?;
        element.set_attribute("tabindex", "-1").ok()?;
        element.set_attribute("data-iced-control", &id).ok()?;
        element.set_text_content(Some(&name));
        let callback = wasm_bindgen::closure::Closure::<dyn FnMut(web_sys::Event)>::new(move |_| {
            ACCESSIBLE_OUTPUT.with(|output| { if let Some(output) = output.borrow().as_ref() { let _ = output.unbounded_send(super::Message::Activate(control, super::Opening::Keyboard)); } });
        });
        element.add_event_listener_with_callback("click", callback.as_ref().unchecked_ref()).ok()?;
        Some(Self { element, callback, presentation: None, present: true })
    }
}
#[cfg(target_arch = "wasm32")]
impl Drop for AccessibleControl {
    fn drop(&mut self) {
        use wasm_bindgen::JsCast;
        let _ = self.element.remove_event_listener_with_callback("click", self.callback.as_ref().unchecked_ref());
        self.element.remove();
    }
}
#[cfg(target_arch = "wasm32")]
#[derive(Default)]
struct AccessibleTree {
    root: Option<web_sys::Element>,
    presentation: Option<crate::view_model::notices::Presentation>,
    open: Option<bool>,
    controls: std::collections::HashMap<super::Control, AccessibleControl>,
}
#[cfg(target_arch = "wasm32")]
impl Drop for AccessibleTree {
    fn drop(&mut self) { if let Some(root) = &self.root { root.remove(); } }
}
#[cfg(target_arch = "wasm32")]
impl AccessibleTree {
    fn sync(&mut self, component: &super::Component, notices: &crate::view_model::notices::NoticeStore) {
        use super::Control;
        use std::collections::hash_map::Entry;
        // No DOM query, row walk, text preparation or allocation on unchanged input.
        let changed = self.presentation.as_ref() != Some(notices.presentation());
        if !changed && self.open == Some(component.open) { return; }
        let Some(document) = web_sys::window().and_then(|window| window.document()) else { return; };
        if self.root.is_none() {
            let Ok(root) = document.create_element("section") else { return; };
            root.set_id("status.accessibility");
            let _ = root.set_attribute("aria-label", "Status notifications");
            let _ = root.set_attribute("style", "position:absolute;width:1px;height:1px;overflow:hidden;clip-path:inset(50%);white-space:pre-wrap");
            let Some(body) = document.body() else { return; };
            if body.append_child(&root).is_err() { return; }
            self.root = Some(root);
        }
        if changed {
            for node in self.controls.values_mut() { node.present = false; }
            for control in [Control::Trigger, Control::Close, Control::Settings] {
                if let Entry::Vacant(entry) = self.controls.entry(control) {
                    let Some(node) = AccessibleControl::new(&document, control) else { return; };
                    entry.insert(node);
                }
                self.controls.get_mut(&control).unwrap().present = true;
            }
            for notice in notices.rows() {
                let mut description = None;
                for control in [Control::Copy(notice.id), Control::Detail(notice.id), Control::Dismiss(notice.id)] {
                    if let Entry::Vacant(entry) = self.controls.entry(control) {
                        let Some(node) = AccessibleControl::new(&document, control) else { return; };
                        entry.insert(node);
                    }
                    let node = self.controls.get_mut(&control).unwrap();
                    node.present = true;
                    if node.presentation.as_ref() != Some(notice.presentation()) {
                        let description = description.get_or_insert_with(|| format!("{}\n\n{}", notice.title, notice.detail));
                        if node.element.set_attribute("aria-description", description).is_err() { return; }
                        node.presentation = Some(notice.presentation().clone());
                    }
                }
            }
            self.controls.retain(|_, node| node.present);
        }
        let root = self.root.as_ref().unwrap();
        let mut controls = component.controls(notices);
        controls.push(Control::Settings);
        if !component.open {
            for (control, node) in &self.controls {
                if !matches!(control, Control::Trigger | Control::Settings) { node.element.remove(); }
            }
        }
        // Keep existing nodes in place; insert only changed ordering/new controls.
        let mut cursor = root.first_child();
        for control in controls {
            let element = &self.controls[&control].element;
            if cursor.as_ref().is_some_and(|node| node.is_same_node(Some(element))) {
                cursor = element.next_sibling();
            } else if root.insert_before(element, cursor.as_ref()).is_err() { return; }
        }
        if self.controls[&Control::Trigger].element.set_attribute("aria-expanded", if component.open { "true" } else { "false" }).is_err() { return; }
        self.presentation = Some(notices.presentation().clone());
        self.open = Some(component.open);
    }
}
#[cfg(target_arch = "wasm32")]
pub fn sync(component: &super::Component, notices: &crate::view_model::notices::NoticeStore) {
    ACCESSIBLE_TREE.with(|tree| tree.borrow_mut().sync(component, notices));
}
