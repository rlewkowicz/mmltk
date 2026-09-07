#[cfg(target_arch = "wasm32")]
fn main() -> iced::Result {
    mmltk_browser_app::run()
}

#[cfg(not(target_arch = "wasm32"))]
fn main() {}
