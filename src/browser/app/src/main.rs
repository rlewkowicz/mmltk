mod app;
mod generated;
mod message;
mod model;
mod transport;
mod view;
mod workspace_surface;

use iced::Backend;
use iced::backend::{Api, PowerPreference};

const PRIMARY_ACTION_FONT_BYTES: &[u8] = include_bytes!("../assets/fonts/VeraBd.ttf");

fn main() -> iced::Result {
    let settings = iced::Settings {
        default_text_size: 14.0.into(),
        ..iced::Settings::default()
    };
    iced::application(app::boot, app::update, view::view)
        .settings(settings)
        .title("mmltk")
        .subscription(app::subscription)
        .theme(|state: &app::App| state.theme.clone())
        .scale_factor(app::App::ui_scale)
        .font(iced_aw::ICED_AW_FONT_BYTES)
        .font(PRIMARY_ACTION_FONT_BYTES)
        .backend(Backend::Hardware(Api::WebGPU))
        .power_preference(PowerPreference::HighPerformance)
        .run()
}
