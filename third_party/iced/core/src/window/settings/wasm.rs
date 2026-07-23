#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PlatformSpecific {
                            pub target: Option<String>,
}

impl Default for PlatformSpecific {
    fn default() -> Self {
        Self {
            target: Some(String::from("iced")),
        }
    }
}
