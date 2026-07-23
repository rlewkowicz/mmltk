use crate::core::theme;
use crate::futures::futures::channel::oneshot;
use crate::futures::subscription::{self, Subscription};
use crate::task::{self, Task};

#[derive(Debug)]
pub enum Action {
        GetInformation(oneshot::Sender<Information>),

        GetTheme(oneshot::Sender<theme::Mode>),

        NotifyTheme(theme::Mode),
}

#[derive(Clone, Debug)]
pub struct Information {
        pub system_name: Option<String>,
        pub system_kernel: Option<String>,
                            pub system_version: Option<String>,
        pub system_short_version: Option<String>,
        pub cpu_brand: String,
        pub cpu_cores: Option<usize>,
        pub memory_total: u64,
        pub memory_used: Option<u64>,
        pub graphics_backend: String,
        pub graphics_adapter: String,
}

pub fn information() -> Task<Information> {
    task::oneshot(|channel| crate::Action::System(Action::GetInformation(channel)))
}

pub fn theme() -> Task<theme::Mode> {
    task::oneshot(|sender| crate::Action::System(Action::GetTheme(sender)))
}

pub fn theme_changes() -> Subscription<theme::Mode> {
    #[derive(Hash)]
    struct ThemeChanges;

    subscription::filter_map(ThemeChanges, |event| {
        let subscription::Event::SystemThemeChanged(mode) = event else {
            return None;
        };

        Some(mode)
    })
}
