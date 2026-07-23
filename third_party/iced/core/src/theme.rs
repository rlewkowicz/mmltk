pub mod palette;

pub use palette::Palette;

use crate::Color;

use std::borrow::Cow;
use std::fmt;
use std::sync::Arc;

#[derive(Debug, Clone, PartialEq)]
pub enum Theme {
        Light,
        Dark,
        Dracula,
        Nord,
        SolarizedLight,
        SolarizedDark,
        GruvboxLight,
        GruvboxDark,
        CatppuccinLatte,
        CatppuccinFrappe,
        CatppuccinMacchiato,
        CatppuccinMocha,
        TokyoNight,
        TokyoNightStorm,
        TokyoNightLight,
        KanagawaWave,
        KanagawaDragon,
        KanagawaLotus,
        Moonfly,
        Nightfly,
        Oxocarbon,
        Ferra,
        Custom(Arc<Custom>),
}

impl Theme {
        pub const ALL: &'static [Self] = &[
        Self::Light,
        Self::Dark,
        Self::Dracula,
        Self::Nord,
        Self::SolarizedLight,
        Self::SolarizedDark,
        Self::GruvboxLight,
        Self::GruvboxDark,
        Self::CatppuccinLatte,
        Self::CatppuccinFrappe,
        Self::CatppuccinMacchiato,
        Self::CatppuccinMocha,
        Self::TokyoNight,
        Self::TokyoNightStorm,
        Self::TokyoNightLight,
        Self::KanagawaWave,
        Self::KanagawaDragon,
        Self::KanagawaLotus,
        Self::Moonfly,
        Self::Nightfly,
        Self::Oxocarbon,
        Self::Ferra,
    ];

        pub fn custom(name: impl Into<Cow<'static, str>>, seed: palette::Seed) -> Self {
        Self::custom_with_fn(name, seed, Palette::generate)
    }

            pub fn custom_with_fn(
        name: impl Into<Cow<'static, str>>,
        palette: palette::Seed,
        generate: impl FnOnce(palette::Seed) -> Palette,
    ) -> Self {
        Self::Custom(Arc::new(Custom::with_fn(name, palette, generate)))
    }

        pub fn palette(&self) -> &palette::Palette {
        match self {
            Self::Light => &palette::LIGHT,
            Self::Dark => &palette::DARK,
            Self::Dracula => &palette::DRACULA,
            Self::Nord => &palette::NORD,
            Self::SolarizedLight => &palette::SOLARIZED_LIGHT,
            Self::SolarizedDark => &palette::SOLARIZED_DARK,
            Self::GruvboxLight => &palette::GRUVBOX_LIGHT,
            Self::GruvboxDark => &palette::GRUVBOX_DARK,
            Self::CatppuccinLatte => &palette::CATPPUCCIN_LATTE,
            Self::CatppuccinFrappe => &palette::CATPPUCCIN_FRAPPE,
            Self::CatppuccinMacchiato => &palette::CATPPUCCIN_MACCHIATO,
            Self::CatppuccinMocha => &palette::CATPPUCCIN_MOCHA,
            Self::TokyoNight => &palette::TOKYO_NIGHT,
            Self::TokyoNightStorm => &palette::TOKYO_NIGHT_STORM,
            Self::TokyoNightLight => &palette::TOKYO_NIGHT_LIGHT,
            Self::KanagawaWave => &palette::KANAGAWA_WAVE,
            Self::KanagawaDragon => &palette::KANAGAWA_DRAGON,
            Self::KanagawaLotus => &palette::KANAGAWA_LOTUS,
            Self::Moonfly => &palette::MOONFLY,
            Self::Nightfly => &palette::NIGHTFLY,
            Self::Oxocarbon => &palette::OXOCARBON,
            Self::Ferra => &palette::FERRA,
            Self::Custom(custom) => &custom.palette,
        }
    }

        pub fn seed(&self) -> palette::Seed {
        match self {
            Self::Light => palette::Seed::LIGHT,
            Self::Dark => palette::Seed::DARK,
            Self::Dracula => palette::Seed::DRACULA,
            Self::Nord => palette::Seed::NORD,
            Self::SolarizedLight => palette::Seed::SOLARIZED_LIGHT,
            Self::SolarizedDark => palette::Seed::SOLARIZED_DARK,
            Self::GruvboxLight => palette::Seed::GRUVBOX_LIGHT,
            Self::GruvboxDark => palette::Seed::GRUVBOX_DARK,
            Self::CatppuccinLatte => palette::Seed::CATPPUCCIN_LATTE,
            Self::CatppuccinFrappe => palette::Seed::CATPPUCCIN_FRAPPE,
            Self::CatppuccinMacchiato => palette::Seed::CATPPUCCIN_MACCHIATO,
            Self::CatppuccinMocha => palette::Seed::CATPPUCCIN_MOCHA,
            Self::TokyoNight => palette::Seed::TOKYO_NIGHT,
            Self::TokyoNightStorm => palette::Seed::TOKYO_NIGHT_STORM,
            Self::TokyoNightLight => palette::Seed::TOKYO_NIGHT_LIGHT,
            Self::KanagawaWave => palette::Seed::KANAGAWA_WAVE,
            Self::KanagawaDragon => palette::Seed::KANAGAWA_DRAGON,
            Self::KanagawaLotus => palette::Seed::KANAGAWA_LOTUS,
            Self::Moonfly => palette::Seed::MOONFLY,
            Self::Nightfly => palette::Seed::NIGHTFLY,
            Self::Oxocarbon => palette::Seed::OXOCARBON,
            Self::Ferra => palette::Seed::FERRA,
            Self::Custom(custom) => custom.seed,
        }
    }
}

impl fmt::Display for Theme {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.name())
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct Custom {
    name: Cow<'static, str>,
    seed: palette::Seed,
    palette: Palette,
}

impl Custom {
        pub fn new(name: impl Into<Cow<'static, str>>, seed: palette::Seed) -> Self {
        Self::with_fn(name, seed, Palette::generate)
    }

            pub fn with_fn(
        name: impl Into<Cow<'static, str>>,
        seed: palette::Seed,
        generate: impl FnOnce(palette::Seed) -> Palette,
    ) -> Self {
        Self {
            name: name.into(),
            seed,
            palette: generate(seed),
        }
    }
}

impl fmt::Display for Custom {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.name)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Mode {
        #[default]
    None,
        Light,
        Dark,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Style {
        pub background_color: Color,

        pub text_color: Color,
}

pub trait Base {
        fn default(preference: Mode) -> Self;

        fn mode(&self) -> Mode;

        fn base(&self) -> Style;

                    fn seed(&self) -> Option<palette::Seed>;

                    fn name(&self) -> &str;
}

impl Base for Theme {
    fn default(preference: Mode) -> Self {
        use std::env;
        use std::sync::OnceLock;

        static SYSTEM: OnceLock<Option<Theme>> = OnceLock::new();

        let system = SYSTEM.get_or_init(|| {
            let name = env::var("ICED_THEME").ok()?;

            Theme::ALL
                .iter()
                .find(|theme| theme.to_string() == name)
                .cloned()
        });

        if let Some(system) = system {
            return system.clone();
        }

        match preference {
            Mode::None | Mode::Light => Self::Light,
            Mode::Dark => Self::Dark,
        }
    }

    fn mode(&self) -> Mode {
        if self.palette().is_dark {
            Mode::Dark
        } else {
            Mode::Light
        }
    }

    fn base(&self) -> Style {
        default(self)
    }

    fn seed(&self) -> Option<palette::Seed> {
        Some(self.seed())
    }

    fn name(&self) -> &str {
        match self {
            Self::Light => "Light",
            Self::Dark => "Dark",
            Self::Dracula => "Dracula",
            Self::Nord => "Nord",
            Self::SolarizedLight => "Solarized Light",
            Self::SolarizedDark => "Solarized Dark",
            Self::GruvboxLight => "Gruvbox Light",
            Self::GruvboxDark => "Gruvbox Dark",
            Self::CatppuccinLatte => "Catppuccin Latte",
            Self::CatppuccinFrappe => "Catppuccin Frappé",
            Self::CatppuccinMacchiato => "Catppuccin Macchiato",
            Self::CatppuccinMocha => "Catppuccin Mocha",
            Self::TokyoNight => "Tokyo Night",
            Self::TokyoNightStorm => "Tokyo Night Storm",
            Self::TokyoNightLight => "Tokyo Night Light",
            Self::KanagawaWave => "Kanagawa Wave",
            Self::KanagawaDragon => "Kanagawa Dragon",
            Self::KanagawaLotus => "Kanagawa Lotus",
            Self::Moonfly => "Moonfly",
            Self::Nightfly => "Nightfly",
            Self::Oxocarbon => "Oxocarbon",
            Self::Ferra => "Ferra",
            Self::Custom(custom) => &custom.name,
        }
    }
}

pub fn default(theme: &Theme) -> Style {
    let palette = theme.palette();

    Style {
        background_color: palette.background.base.color,
        text_color: palette.background.base.text,
    }
}
