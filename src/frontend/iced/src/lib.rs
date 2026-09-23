#[cfg(any(target_arch = "wasm32", test))]
mod app;
pub mod application_codec;
#[cfg(any(target_arch = "wasm32", test))]
mod fluent_theme;
#[allow(non_snake_case, non_upper_case_globals, unused_mut, unused_variables)]
pub mod generated;
#[cfg(any(target_arch = "wasm32", test))]
mod integration_control;
#[cfg(any(target_arch = "wasm32", test))]
mod message;
#[cfg(any(target_arch = "wasm32", test))]
mod presentation_surface;
pub mod protocol;
#[cfg(test)]
mod test_support;
#[cfg(any(target_arch = "wasm32", test))]
mod transport;
#[cfg(any(target_arch = "wasm32", test))]
mod transport_connection;
#[cfg(any(target_arch = "wasm32", test))]
mod view;
#[cfg(any(target_arch = "wasm32", test))]
mod view_model;
#[cfg(any(target_arch = "wasm32", test))]
mod workspace_fps;
#[cfg(any(target_arch = "wasm32", test))]
mod workspace_input;

#[cfg(any(target_arch = "wasm32", test))]
pub fn run() -> iced::Result {
    use iced::Backend;
    use iced::backend::Api;

    let mut fonts = iced_fluent_theme::font::load();
    fonts.extend([
        include_bytes!("../assets/SourceSansPro-Regular.otf")
            .as_slice()
            .into(),
        include_bytes!("../assets/SourceCodePro-Semibold.otf")
            .as_slice()
            .into(),
        include_bytes!("../assets/fonts/VeraBd.ttf")
            .as_slice()
            .into(),
    ]);
    let settings = iced::Settings {
        fonts,
        default_font: iced_fluent_theme::font::REGULAR,
        default_text_size: 14.0.into(),
        ..iced::Settings::default()
    };
    iced::application(app::boot, app::update, app::view)
        .settings(settings)
        .title("mmltk")
        .subscription(app::subscription)
        .theme(app::theme)
        .scale_factor(app::scale_factor)
        .backend(Backend::Hardware(Api::WebGPU))
        .run()
}
