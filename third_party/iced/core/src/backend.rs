use std::env;
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Backend {
        Best,
        Hardware(Api),
        Software,
        Custom(String),
}

impl Backend {
        pub const ALL: &[Self] = &[
        Self::Best,
        Self::Hardware(Api::Best),
        Self::Hardware(Api::Vulkan),
        Self::Hardware(Api::Metal),
        Self::Hardware(Api::DirectX12),
        Self::Hardware(Api::OpenGL),
        Self::Hardware(Api::WebGPU),
        Self::Software,
    ];

        pub fn hardware(&self) -> Option<Api> {
        match self {
            Backend::Hardware(api) => Some(*api),
            _ => None,
        }
    }

        pub fn is_software(&self) -> bool {
        matches!(self, Self::Software)
    }

        pub fn matches(&self, target: &str) -> bool {
        match self {
            Backend::Best => true,
            Backend::Custom(name) => name == target || name == &target.replace("-", "_"),
            _ => false,
        }
    }
}

impl From<String> for Backend {
    fn from(backend: String) -> Self {
        Self::Custom(backend)
    }
}

impl From<&str> for Backend {
    fn from(backend: &str) -> Self {
        Self::Custom(backend.to_owned())
    }
}

impl fmt::Display for Backend {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Backend::Best => write!(f, "Best Backend"),
            Backend::Hardware(api) => write!(f, "Hardware Backend ({api})"),
            Backend::Software => write!(f, "Software Backend"),
            Backend::Custom(name) => write!(f, "Custom Backend ({name})"),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Api {
        #[default]
    Best,
        Vulkan,
        Metal,
        DirectX12,
        OpenGL,
        WebGPU,
}

impl Default for Backend {
    fn default() -> Self {
        let Ok(backend) = env::var("ICED_BACKEND") else {
            return Self::Best;
        };

        Self::Custom(backend.to_owned())
    }
}

impl fmt::Display for Api {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Api::Best => "Best",
            Api::Vulkan => "Vulkan",
            Api::Metal => "Metal",
            Api::DirectX12 => "DirectX 12",
            Api::OpenGL => "OpenGL",
            Api::WebGPU => "WebGPU",
        })
    }
}

#[derive(Debug, Clone, PartialEq, Copy, Default)]
pub enum PowerPreference {
                #[default]
    None,

        LowPower,

        HighPerformance,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Settings {
                pub backend: Backend,

                pub power_preference: PowerPreference,

                                pub antialiasing: bool,

                pub vsync: bool,
}

impl Default for Settings {
    fn default() -> Settings {
        Settings {
            backend: Backend::Best,
            antialiasing: true,
            vsync: true,
            power_preference: PowerPreference::None,
        }
    }
}

impl From<&crate::Settings> for Settings {
    fn from(settings: &crate::Settings) -> Self {
        Self {
            backend: settings.backend.clone(),
            antialiasing: settings.antialiasing,
            vsync: settings.vsync,
            power_preference: settings.power_preference,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum Error {
        #[error("the requested backend version is not supported")]
    VersionNotSupported,

        #[error("failed to find any pixel format that matches the criteria")]
    NoAvailablePixelFormat,

        #[error("a suitable graphics adapter could not be found: {reason}")]
    GraphicsAdapterNotFound {
                backend: &'static str,
                reason: Reason,
    },

        #[error("an error occurred in the context's internal backend")]
    BackendError(String),

        #[error("multiple errors occurred:\n{}", error_list(.0))]
    List(Vec<Self>),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Reason {
        DidNotMatch {
                preferred_backend: Backend,
    },
        RequestFailed(String),
}

impl fmt::Display for Reason {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Reason::DidNotMatch { preferred_backend } => {
                write!(
                    f,
                    "the backend did not match the preference: {preferred_backend}"
                )
            }
            Reason::RequestFailed(error) => f.write_str(error),
        }
    }
}

fn error_list(errors: &Vec<Error>) -> String {
    let mut list = String::new();

    for error in errors {
        list.push_str("- ");
        list.push_str(&error.to_string());
        list.push('\n');
    }

    list
}
