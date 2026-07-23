use bitflags::bitflags;

bitflags! {
        #[derive(Debug, Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
    pub struct Modifiers: u32{
                const SHIFT = 0b100;
                const CTRL = 0b100 << 3;
                const ALT = 0b100 << 6;
                        const LOGO = 0b100 << 9;
                const NONE = 0;
    }
}

impl Modifiers {
                            pub const COMMAND: Self = if cfg!(target_os = "macos") {
        Self::LOGO
    } else {
        Self::CTRL
    };

                pub fn shift(self) -> bool {
        self.contains(Self::SHIFT)
    }

                pub fn control(self) -> bool {
        self.contains(Self::CTRL)
    }

                pub fn alt(self) -> bool {
        self.contains(Self::ALT)
    }

                pub fn logo(self) -> bool {
        self.contains(Self::LOGO)
    }

                                pub fn command(self) -> bool {
        #[cfg(target_os = "macos")]
        let is_pressed = self.logo();

        #[cfg(not(target_os = "macos"))]
        let is_pressed = self.control();

        is_pressed
    }

                    pub fn jump(self) -> bool {
        if cfg!(target_os = "macos") {
            self.alt()
        } else {
            self.control()
        }
    }

                    pub fn macos_command(self) -> bool {
        if cfg!(target_os = "macos") {
            self.logo()
        } else {
            false
        }
    }
}
