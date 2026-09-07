use crate::{Color, color};

use std::sync::LazyLock;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Palette {
        pub background: Background,
        pub primary: Swatch,
        pub secondary: Swatch,
        pub success: Swatch,
        pub warning: Swatch,
        pub danger: Swatch,
        pub is_dark: bool,
}

impl Palette {
        pub fn generate(palette: Seed) -> Self {
        Self {
            background: Background::new(palette.background, palette.text),
            primary: Swatch::generate(palette.primary, palette.background, palette.text),
            secondary: Swatch::derive(palette.background, palette.text),
            success: Swatch::generate(palette.success, palette.background, palette.text),
            warning: Swatch::generate(palette.warning, palette.background, palette.text),
            danger: Swatch::generate(palette.danger, palette.background, palette.text),
            is_dark: is_dark(palette.background),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Pair {
        pub color: Color,

                        pub text: Color,
}

impl Pair {
        pub fn new(color: Color, text: Color) -> Self {
        Self {
            color,
            text: readable(color, text),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Background {
        pub base: Pair,
        pub weakest: Pair,
        pub weaker: Pair,
        pub weak: Pair,
        pub neutral: Pair,
        pub strong: Pair,
        pub stronger: Pair,
        pub strongest: Pair,
}

impl Background {
        pub fn new(base: Color, text: Color) -> Self {
        let weakest = deviate(base, 0.03);
        let weaker = deviate(base, 0.07);
        let weak = deviate(base, 0.1);
        let neutral = deviate(base, 0.125);
        let strong = deviate(base, 0.15);
        let stronger = deviate(base, 0.175);
        let strongest = deviate(base, 0.20);

        Self {
            base: Pair::new(base, text),
            weakest: Pair::new(weakest, text),
            weaker: Pair::new(weaker, text),
            weak: Pair::new(weak, text),
            neutral: Pair::new(neutral, text),
            strong: Pair::new(strong, text),
            stronger: Pair::new(stronger, text),
            strongest: Pair::new(strongest, text),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Swatch {
        pub base: Pair,
        pub weak: Pair,
        pub strong: Pair,
}

impl Swatch {
        pub fn generate(base: Color, background: Color, text: Color) -> Self {
        let weak = base.mix(background, 0.4);
        let strong = deviate(base, 0.1);

        Self {
            base: Pair::new(base, text),
            weak: Pair::new(weak, text),
            strong: Pair::new(strong, text),
        }
    }

        pub fn derive(base: Color, text: Color) -> Self {
        let factor = if is_dark(base) { 0.2 } else { 0.4 };

        let weak = deviate(base, 0.1).mix(text, factor);
        let strong = deviate(base, 0.3).mix(text, factor);
        let base = deviate(base, 0.2).mix(text, factor);

        Self {
            base: Pair::new(base, text),
            weak: Pair::new(weak, text),
            strong: Pair::new(strong, text),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct Seed {
        pub background: Color,
        pub text: Color,
        pub primary: Color,
        pub success: Color,
        pub warning: Color,
        pub danger: Color,
}

impl Seed {
        pub const LIGHT: Self = Self {
        background: Color::WHITE,
        text: Color::BLACK,
        primary: color!(0x5865F2),
        success: color!(0x12664f),
        warning: color!(0xb77e33),
        danger: color!(0xc3423f),
    };

        pub const DARK: Self = Self {
        background: color!(0x2B2D31),
        text: Color::from_rgb(0.90, 0.90, 0.90),
        primary: color!(0x5865F2),
        success: color!(0x12664f),
        warning: color!(0xffc14e),
        danger: color!(0xc3423f),
    };

                pub const DRACULA: Self = Self {
        background: color!(0x282A36), 
        text: color!(0xf8f8f2),       
        primary: color!(0xbd93f9),    
        success: color!(0x50fa7b),    
        warning: color!(0xf1fa8c),    
        danger: color!(0xff5555),     
    };

                pub const NORD: Self = Self {
        background: color!(0x2e3440), 
        text: color!(0xeceff4),       
        primary: color!(0x8fbcbb),    
        success: color!(0xa3be8c),    
        warning: color!(0xebcb8b),    
        danger: color!(0xbf616a),     
    };

                pub const SOLARIZED_LIGHT: Self = Self {
        background: color!(0xfdf6e3), 
        text: color!(0x657b83),       
        primary: color!(0x2aa198),    
        success: color!(0x859900),    
        warning: color!(0xb58900),    
        danger: color!(0xdc322f),     
    };

                pub const SOLARIZED_DARK: Self = Self {
        background: color!(0x002b36), 
        text: color!(0x839496),       
        primary: color!(0x2aa198),    
        success: color!(0x859900),    
        warning: color!(0xb58900),    
        danger: color!(0xdc322f),     
    };

                pub const GRUVBOX_LIGHT: Self = Self {
        background: color!(0xfbf1c7), 
        text: color!(0x282828),       
        primary: color!(0x458588),    
        success: color!(0x98971a),    
        warning: color!(0xd79921),    
        danger: color!(0xcc241d),     
    };

                pub const GRUVBOX_DARK: Self = Self {
        background: color!(0x282828), 
        text: color!(0xfbf1c7),       
        primary: color!(0x458588),    
        success: color!(0x98971a),    
        warning: color!(0xd79921),    
        danger: color!(0xcc241d),     
    };

                pub const CATPPUCCIN_LATTE: Self = Self {
        background: color!(0xeff1f5), 
        text: color!(0x4c4f69),       
        primary: color!(0x1e66f5),    
        success: color!(0x40a02b),    
        warning: color!(0xdf8e1d),    
        danger: color!(0xd20f39),     
    };

                pub const CATPPUCCIN_FRAPPE: Self = Self {
        background: color!(0x303446), 
        text: color!(0xc6d0f5),       
        primary: color!(0x8caaee),    
        success: color!(0xa6d189),    
        warning: color!(0xe5c890),    
        danger: color!(0xe78284),     
    };

                pub const CATPPUCCIN_MACCHIATO: Self = Self {
        background: color!(0x24273a), 
        text: color!(0xcad3f5),       
        primary: color!(0x8aadf4),    
        success: color!(0xa6da95),    
        warning: color!(0xeed49f),    
        danger: color!(0xed8796),     
    };

                pub const CATPPUCCIN_MOCHA: Self = Self {
        background: color!(0x1e1e2e), 
        text: color!(0xcdd6f4),       
        primary: color!(0x89b4fa),    
        success: color!(0xa6e3a1),    
        warning: color!(0xf9e2af),    
        danger: color!(0xf38ba8),     
    };

                pub const TOKYO_NIGHT: Self = Self {
        background: color!(0x1a1b26), 
        text: color!(0x9aa5ce),       
        primary: color!(0x2ac3de),    
        success: color!(0x9ece6a),    
        warning: color!(0xe0af68),    
        danger: color!(0xf7768e),     
    };

                pub const TOKYO_NIGHT_STORM: Self = Self {
        background: color!(0x24283b), 
        text: color!(0x9aa5ce),       
        primary: color!(0x2ac3de),    
        success: color!(0x9ece6a),    
        warning: color!(0xe0af68),    
        danger: color!(0xf7768e),     
    };

                pub const TOKYO_NIGHT_LIGHT: Self = Self {
        background: color!(0xd5d6db), 
        text: color!(0x565a6e),       
        primary: color!(0x166775),    
        success: color!(0x485e30),    
        warning: color!(0x8f5e15),    
        danger: color!(0x8c4351),     
    };

                pub const KANAGAWA_WAVE: Self = Self {
        background: color!(0x1f1f28), 
        text: color!(0xDCD7BA),       
        primary: color!(0x7FB4CA),    
        success: color!(0x76946A),    
        warning: color!(0xff9e3b),    
        danger: color!(0xC34043),     
    };

                pub const KANAGAWA_DRAGON: Self = Self {
        background: color!(0x181616), 
        text: color!(0xc5c9c5),       
        primary: color!(0x223249),    
        success: color!(0x8a9a7b),    
        warning: color!(0xff9e3b),    
        danger: color!(0xc4746e),     
    };

                pub const KANAGAWA_LOTUS: Self = Self {
        background: color!(0xf2ecbc), 
        text: color!(0x545464),       
        primary: color!(0x4d699b),    
        success: color!(0x6f894e),    
        warning: color!(0xe98a00),    
        danger: color!(0xc84053),     
    };

                pub const MOONFLY: Self = Self {
        background: color!(0x080808), 
        text: color!(0xbdbdbd),       
        primary: color!(0x80a0ff),    
        success: color!(0x8cc85f),    
        warning: color!(0xe3c78a),    
        danger: color!(0xff5454),     
    };

                pub const NIGHTFLY: Self = Self {
        background: color!(0x011627), 
        text: color!(0xbdc1c6),       
        primary: color!(0x82aaff),    
        success: color!(0xa1cd5e),    
        warning: color!(0xe3d18a),    
        danger: color!(0xfc514e),     
    };

                pub const OXOCARBON: Self = Self {
        background: color!(0x232323),
        text: color!(0xd0d0d0),
        primary: color!(0x00b4ff),
        success: color!(0x00c15a),
        warning: color!(0xbe95ff), 
        danger: color!(0xf62d0f),
    };

                pub const FERRA: Self = Self {
        background: color!(0x2b292d),
        text: color!(0xfecdb2),
        primary: color!(0xd1d1e0),
        success: color!(0xb1b695),
        warning: color!(0xf5d76e), 
        danger: color!(0xe06b75),
    };
}

pub static LIGHT: LazyLock<Palette> = LazyLock::new(|| Palette::generate(Seed::LIGHT));

pub static DARK: LazyLock<Palette> = LazyLock::new(|| Palette::generate(Seed::DARK));

pub static DRACULA: LazyLock<Palette> = LazyLock::new(|| Palette::generate(Seed::DRACULA));

pub static NORD: LazyLock<Palette> = LazyLock::new(|| Palette::generate(Seed::NORD));

pub static SOLARIZED_LIGHT: LazyLock<Palette> =
    LazyLock::new(|| Palette::generate(Seed::SOLARIZED_LIGHT));

pub static SOLARIZED_DARK: LazyLock<Palette> =
    LazyLock::new(|| Palette::generate(Seed::SOLARIZED_DARK));

pub static GRUVBOX_LIGHT: LazyLock<Palette> =
    LazyLock::new(|| Palette::generate(Seed::GRUVBOX_LIGHT));

pub static GRUVBOX_DARK: LazyLock<Palette> =
    LazyLock::new(|| Palette::generate(Seed::GRUVBOX_DARK));

pub static CATPPUCCIN_LATTE: LazyLock<Palette> =
    LazyLock::new(|| Palette::generate(Seed::CATPPUCCIN_LATTE));

pub static CATPPUCCIN_FRAPPE: LazyLock<Palette> =
    LazyLock::new(|| Palette::generate(Seed::CATPPUCCIN_FRAPPE));

pub static CATPPUCCIN_MACCHIATO: LazyLock<Palette> =
    LazyLock::new(|| Palette::generate(Seed::CATPPUCCIN_MACCHIATO));

pub static CATPPUCCIN_MOCHA: LazyLock<Palette> =
    LazyLock::new(|| Palette::generate(Seed::CATPPUCCIN_MOCHA));

pub static TOKYO_NIGHT: LazyLock<Palette> = LazyLock::new(|| Palette::generate(Seed::TOKYO_NIGHT));

pub static TOKYO_NIGHT_STORM: LazyLock<Palette> =
    LazyLock::new(|| Palette::generate(Seed::TOKYO_NIGHT_STORM));

pub static TOKYO_NIGHT_LIGHT: LazyLock<Palette> =
    LazyLock::new(|| Palette::generate(Seed::TOKYO_NIGHT_LIGHT));

pub static KANAGAWA_WAVE: LazyLock<Palette> =
    LazyLock::new(|| Palette::generate(Seed::KANAGAWA_WAVE));

pub static KANAGAWA_DRAGON: LazyLock<Palette> =
    LazyLock::new(|| Palette::generate(Seed::KANAGAWA_DRAGON));

pub static KANAGAWA_LOTUS: LazyLock<Palette> =
    LazyLock::new(|| Palette::generate(Seed::KANAGAWA_LOTUS));

pub static MOONFLY: LazyLock<Palette> = LazyLock::new(|| Palette::generate(Seed::MOONFLY));

pub static NIGHTFLY: LazyLock<Palette> = LazyLock::new(|| Palette::generate(Seed::NIGHTFLY));

pub static OXOCARBON: LazyLock<Palette> = LazyLock::new(|| Palette::generate(Seed::OXOCARBON));

pub static FERRA: LazyLock<Palette> = LazyLock::new(|| Palette::generate(Seed::FERRA));

pub fn darken(color: Color, amount: f32) -> Color {
    let mut oklch = color.into_oklch();

    if oklch.c > 0.0 && oklch.c < (1.0 - oklch.l) / 2.0 {
        oklch.c *= 1.0 + (0.2 / oklch.c).min(100.0) * amount;
    }

    oklch.l = if oklch.l - amount < 0.0 {
        0.0
    } else {
        oklch.l - amount
    };

    Color::from_oklch(oklch)
}

pub fn lighten(color: Color, amount: f32) -> Color {
    let mut oklch = color.into_oklch();

    oklch.c *= 1.0 + 2.0 * amount / oklch.l.max(0.05);

    oklch.l = if oklch.l + amount > 1.0 {
        1.0
    } else {
        oklch.l + amount
    };

    Color::from_oklch(oklch)
}

pub fn deviate(color: Color, amount: f32) -> Color {
    if is_dark(color) {
        lighten(color, amount)
    } else {
        darken(color, amount)
    }
}

pub fn readable(background: Color, text: Color) -> Color {
    if text.is_readable_on(background) {
        return text;
    }

    let improve = if is_dark(background) { lighten } else { darken };

    let candidate = improve(text, 0.1);

    if candidate.is_readable_on(background) {
        return candidate;
    }

    let candidate = improve(text, 0.2);

    if candidate.is_readable_on(background) {
        return candidate;
    }

    let white_contrast = background.relative_contrast(Color::WHITE);
    let black_contrast = background.relative_contrast(Color::BLACK);

    if white_contrast >= black_contrast {
        Color::WHITE.mix(background, 0.05)
    } else {
        Color::BLACK.mix(background, 0.05)
    }
}

pub fn is_dark(color: Color) -> bool {
    color.into_oklch().l < 0.6
}
