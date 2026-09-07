use crate::animation::Interpolable;

#[derive(Debug, Clone, Copy, PartialEq, Default)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[must_use]
pub struct Color {
        pub r: f32,
        pub g: f32,
        pub b: f32,
        pub a: f32,
}

impl Color {
        pub const BLACK: Color = Color {
        r: 0.0,
        g: 0.0,
        b: 0.0,
        a: 1.0,
    };

        pub const WHITE: Color = Color {
        r: 1.0,
        g: 1.0,
        b: 1.0,
        a: 1.0,
    };

        pub const TRANSPARENT: Color = Color {
        r: 0.0,
        g: 0.0,
        b: 0.0,
        a: 0.0,
    };

                    const fn new(r: f32, g: f32, b: f32, a: f32) -> Color {
        debug_assert!(
            r >= 0.0 && r <= 1.0,
            "Red component must be in [0, 1] range."
        );
        debug_assert!(
            g >= 0.0 && g <= 1.0,
            "Green component must be in [0, 1] range."
        );
        debug_assert!(
            b >= 0.0 && b <= 1.0,
            "Blue component must be in [0, 1] range."
        );

        Self { r, g, b, a }
    }

        pub const fn from_rgb(r: f32, g: f32, b: f32) -> Self {
        Self::from_rgba(r, g, b, 1.0f32)
    }

        pub const fn from_rgba(r: f32, g: f32, b: f32, a: f32) -> Self {
        Self::new(r, g, b, a)
    }

        pub const fn from_rgb8(r: u8, g: u8, b: u8) -> Self {
        Self::from_rgba8(r, g, b, 1.0)
    }

        pub const fn from_rgba8(r: u8, g: u8, b: u8, a: f32) -> Self {
        Self::new(r as f32 / 255.0, g as f32 / 255.0, b as f32 / 255.0, a)
    }

        pub const fn from_packed_rgb8(rgb: u32) -> Self {
        Self::from_packed_rgba8(rgb, 1.0)
    }

            pub const fn from_packed_rgba8(rgb: u32, a: f32) -> Self {
        let r = (rgb & 0xff0000) >> 16;
        let g = (rgb & 0xff00) >> 8;
        let b = rgb & 0xff;

        Self::from_rgba8(r as u8, g as u8, b as u8, a)
    }

        pub fn from_linear_rgba(r: f32, g: f32, b: f32, a: f32) -> Self {
        fn gamma_component(u: f32) -> f32 {
            if u < 0.0031308 {
                12.92 * u
            } else {
                1.055 * u.powf(1.0 / 2.4) - 0.055
            }
        }

        Self::new(
            gamma_component(r),
            gamma_component(g),
            gamma_component(b),
            a,
        )
    }

        pub fn from_oklch(oklch: Oklch) -> Color {
        let Oklch { l, c, h, a: alpha } = oklch;

        let a = c * h.cos();
        let b = c * h.sin();

        let l_ = l + 0.39633778 * a + 0.21580376 * b;
        let m_ = l - 0.105561346 * a - 0.06385417 * b;
        let s_ = l - 0.08948418 * a - 1.2914855 * b;

        let l = l_ * l_ * l_;
        let m = m_ * m_ * m_;
        let s = s_ * s_ * s_;

        let r = 4.0767417 * l - 3.3077116 * m + 0.23096994 * s;
        let g = -1.268438 * l + 2.6097574 * m - 0.34131938 * s;
        let b = -0.0041960863 * l - 0.7034186 * m + 1.7076147 * s;

        Color::from_linear_rgba(
            r.clamp(0.0, 1.0),
            g.clamp(0.0, 1.0),
            b.clamp(0.0, 1.0),
            alpha,
        )
    }

        pub const fn invert(&mut self) {
        self.r = 1.0f32 - self.r;
        self.g = 1.0f32 - self.g;
        self.b = 1.0f32 - self.b;
    }

        pub const fn inverse(self) -> Self {
        Self::new(1.0f32 - self.r, 1.0f32 - self.g, 1.0f32 - self.b, self.a)
    }

        pub const fn scale_alpha(self, factor: f32) -> Self {
        Self {
            a: self.a * factor,
            ..self
        }
    }

        pub fn mix(self, b: Color, factor: f32) -> Color {
        let b_amount = factor.clamp(0.0, 1.0);
        let a_amount = 1.0 - b_amount;

        let a_linear = self.into_linear().map(|c| c * a_amount);
        let b_linear = b.into_linear().map(|c| c * b_amount);

        Color::from_linear_rgba(
            a_linear[0] + b_linear[0],
            a_linear[1] + b_linear[1],
            a_linear[2] + b_linear[2],
            a_linear[3] + b_linear[3],
        )
    }

            #[must_use]
    pub fn relative_luminance(self) -> f32 {
        let linear = self.into_linear();
        0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]
    }

                #[must_use]
    pub fn relative_contrast(self, b: Self) -> f32 {
        let lum_a = self.relative_luminance();
        let lum_b = b.relative_luminance();

        (lum_a.max(lum_b) + 0.05) / (lum_a.min(lum_b) + 0.05)
    }

            #[must_use]
    pub fn is_readable_on(self, background: Self) -> bool {
        background.relative_contrast(self) >= 6.0
    }

        #[must_use]
    pub const fn into_rgba8(self) -> [u8; 4] {
        [
            (self.r * 255.0).round() as u8,
            (self.g * 255.0).round() as u8,
            (self.b * 255.0).round() as u8,
            (self.a * 255.0).round() as u8,
        ]
    }

        #[must_use]
    pub fn into_linear(self) -> [f32; 4] {
        fn linear_component(u: f32) -> f32 {
            if u < 0.04045 {
                u / 12.92
            } else {
                ((u + 0.055) / 1.055).powf(2.4)
            }
        }

        [
            linear_component(self.r),
            linear_component(self.g),
            linear_component(self.b),
            self.a,
        ]
    }

        pub fn into_oklch(self) -> Oklch {
        let [r, g, b, alpha] = self.into_linear();

        let l = 0.41222146 * r + 0.53633255 * g + 0.051445995 * b;
        let m = 0.2119035 * r + 0.6806995 * g + 0.10739696 * b;
        let s = 0.08830246 * r + 0.28171885 * g + 0.6299787 * b;

        let l_ = l.cbrt();
        let m_ = m.cbrt();
        let s_ = s.cbrt();

        let l = 0.21045426 * l_ + 0.7936178 * m_ - 0.004072047 * s_;
        let a = 1.9779985 * l_ - 2.4285922 * m_ + 0.4505937 * s_;
        let b = 0.025904037 * l_ + 0.78277177 * m_ - 0.80867577 * s_;

        let c = (a * a + b * b).sqrt();
        let h = b.atan2(a); 

        Oklch { l, c, h, a: alpha }
    }
}

impl From<[f32; 3]> for Color {
    fn from([r, g, b]: [f32; 3]) -> Self {
        Color::new(r, g, b, 1.0)
    }
}

impl From<[f32; 4]> for Color {
    fn from([r, g, b, a]: [f32; 4]) -> Self {
        Color::new(r, g, b, a)
    }
}

impl From<Oklch> for Color {
    fn from(oklch: Oklch) -> Self {
        Self::from_oklch(oklch)
    }
}

impl From<Color> for Oklch {
    fn from(color: Color) -> Self {
        color.into_oklch()
    }
}

#[derive(Debug, thiserror::Error)]
pub enum ParseError {
        #[error(transparent)]
    ParseIntError(#[from] std::num::ParseIntError),
        #[error("expected hex string of length 3, 4, 6 or 8 excluding optional prefix '#', found {0}")]
    InvalidLength(usize),
}

impl std::str::FromStr for Color {
    type Err = ParseError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let hex = s.strip_prefix('#').unwrap_or(s);

        let parse_channel = |from: usize, to: usize| -> Result<f32, std::num::ParseIntError> {
            let num = usize::from_str_radix(&hex[from..=to], 16)? as f32 / 255.0;

            Ok(if from == to { num + num * 16.0 } else { num })
        };

        let val = match hex.len() {
            3 => Color::from_rgb(
                parse_channel(0, 0)?,
                parse_channel(1, 1)?,
                parse_channel(2, 2)?,
            ),
            4 => Color::from_rgba(
                parse_channel(0, 0)?,
                parse_channel(1, 1)?,
                parse_channel(2, 2)?,
                parse_channel(3, 3)?,
            ),
            6 => Color::from_rgb(
                parse_channel(0, 1)?,
                parse_channel(2, 3)?,
                parse_channel(4, 5)?,
            ),
            8 => Color::from_rgba(
                parse_channel(0, 1)?,
                parse_channel(2, 3)?,
                parse_channel(4, 5)?,
                parse_channel(6, 7)?,
            ),
            _ => return Err(ParseError::InvalidLength(hex.len())),
        };

        Ok(val)
    }
}

impl std::fmt::Display for Color {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let [r, g, b, a] = self.into_rgba8();

        if self.a == 1.0 {
            return write!(f, "#{r:02x}{g:02x}{b:02x}");
        }

        write!(f, "#{r:02x}{g:02x}{b:02x}{a:02x}")
    }
}

impl Interpolable for Color {
        fn interpolated(&self, other: Self, ratio: f32) -> Self {
        self.mix(other, ratio)
    }
}

pub struct Oklch {
        pub l: f32,
        pub c: f32,
        pub h: f32,
        pub a: f32,
}

#[macro_export]
macro_rules! color {
    ($r:expr, $g:expr, $b:expr) => {
        $crate::Color::from_rgb8($r, $g, $b)
    };
    ($r:expr, $g:expr, $b:expr, $a:expr) => {{ $crate::Color::from_rgba8($r, $g, $b, $a) }};
    ($hex:literal) => {{ $crate::color!($hex, 1.0) }};
    ($hex:literal, $a:expr) => {{
        let mut hex = $hex as u32;

        if stringify!($hex).len() == 5 {
            let r = hex & 0xF00;
            let g = hex & 0xF0;
            let b = hex & 0xF;

            hex = (r << 12) | (r << 8) | (g << 8) | (g << 4) | (b << 4) | b;
        }

        debug_assert!(hex <= 0xffffff, "color! value must not exceed 0xffffff");

        $crate::Color::from_packed_rgba8(hex, $a)
    }};
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse() {
        let tests = [
            ("#ff0000", [255, 0, 0, 255], "#ff0000"),
            ("00ff0080", [0, 255, 0, 128], "#00ff0080"),
            ("#F80", [255, 136, 0, 255], "#ff8800"),
            ("#00f1", [0, 0, 255, 17], "#0000ff11"),
            ("#00ff", [0, 0, 255, 255], "#0000ff"),
        ];

        for (arg, expected_rgba8, expected_str) in tests {
            let color = arg.parse::<Color>().expect("color must parse");

            assert_eq!(color.into_rgba8(), expected_rgba8);
            assert_eq!(color.to_string(), expected_str);
        }

        assert!("invalid".parse::<Color>().is_err());
    }

    const SHORTHAND: Color = color!(0x123);

    #[test]
    fn shorthand_notation() {
        assert_eq!(SHORTHAND, Color::from_rgb8(0x11, 0x22, 0x33));
    }
}
