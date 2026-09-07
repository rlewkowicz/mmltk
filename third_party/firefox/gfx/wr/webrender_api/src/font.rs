/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use peek_poke::PeekPoke;
use std::cmp::Ordering;
use std::hash::{Hash, Hasher};

use std::path::PathBuf;
use std::sync::Arc;
use crate::IdNamespace;
use crate::channel::Sender;
use crate::units::LayoutPoint;

/// Hashable floating-point storage for font size.
#[repr(C)]
#[derive(Clone, Copy, Debug, MallocSizeOf, PartialEq, PartialOrd, Deserialize, Serialize)]
pub struct FontSize(pub f32);

impl Ord for FontSize {
    fn cmp(&self, other: &FontSize) -> Ordering {
        self.partial_cmp(other).unwrap_or(Ordering::Equal)
    }
}

impl Eq for FontSize {}

impl Hash for FontSize {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.0.to_bits().hash(state);
    }
}

impl From<f32> for FontSize {
    fn from(size: f32) -> Self { FontSize(size) }
}

impl From<FontSize> for f32 {
    fn from(size: FontSize) -> Self { size.0 }
}

impl FontSize {
    pub fn zero() -> Self { FontSize(0.0) }

    pub fn from_f32_px(size: f32) -> Self { FontSize(size) }

    pub fn to_f32_px(&self) -> f32 { self.0 }

    pub fn from_f64_px(size: f64) -> Self { FontSize(size as f32) }

    pub fn to_f64_px(&self) -> f64 { self.0 as f64 }
}


#[derive(Clone, Debug, Hash, Eq, PartialEq, PartialOrd, Ord, Serialize, Deserialize)]
pub struct NativeFontHandle {
    pub path: PathBuf,
    pub index: u32,
}

#[repr(C)]
#[derive(Copy, Clone, Deserialize, Serialize, Debug)]
pub struct GlyphDimensions {
    pub left: i32,
    pub top: i32,
    pub width: i32,
    pub height: i32,
    pub advance: f32,
}

pub struct GlyphDimensionRequest {
    pub key: FontInstanceKey,
    pub glyph_indices: Vec<GlyphIndex>,
    pub sender: Sender<Vec<Option<GlyphDimensions>>>,
}

pub struct GlyphIndexRequest {
    pub key: FontKey,
    pub text: String,
    pub sender: Sender<Vec<Option<u32>>>,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, MallocSizeOf, PartialEq, Serialize, Ord, PartialOrd)]
pub struct FontKey(pub IdNamespace, pub u32);

impl FontKey {
    pub fn new(namespace: IdNamespace, key: u32) -> FontKey {
        FontKey(namespace, key)
    }
}

/// Container for the raw data describing a font. This might be a stream of
/// bytes corresponding to a downloaded font, or a handle to a native font from
/// the operating system.
///
/// Note that fonts need to be instantiated before being used, which involves
/// assigning size and various other options. The word 'template' here is
/// intended to distinguish this data from instance-specific data.
#[derive(Debug, Clone, Hash, Eq, PartialEq)]
pub enum FontTemplate {
    Raw(Arc<Vec<u8>>, u32),
    Native(NativeFontHandle),
}

#[repr(u8)]
#[derive(Debug, Copy, Clone, Hash, Eq, MallocSizeOf, PartialEq, Serialize, Deserialize, Ord, PartialOrd, PeekPoke)]
pub enum FontRenderMode {
    Mono = 0,
    Alpha,
    Subpixel,
}

impl Default for FontRenderMode {
    fn default() -> Self {
        FontRenderMode::Mono
    }
}

impl FontRenderMode {
    pub fn limit_by(self, other: FontRenderMode) -> FontRenderMode {
        match (self, other) {
            (FontRenderMode::Subpixel, _) | (_, FontRenderMode::Mono) => other,
            _ => self,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, MallocSizeOf, PartialOrd, Deserialize, Serialize)]
pub struct FontVariation {
    pub tag: u32,
    pub value: f32,
}

impl Ord for FontVariation {
    fn cmp(&self, other: &FontVariation) -> Ordering {
        self.tag.cmp(&other.tag)
            .then(self.value.to_bits().cmp(&other.value.to_bits()))
    }
}

impl PartialEq for FontVariation {
    fn eq(&self, other: &FontVariation) -> bool {
        self.tag == other.tag &&
        self.value.to_bits() == other.value.to_bits()
    }
}

impl Eq for FontVariation {}

impl Hash for FontVariation {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.tag.hash(state);
        self.value.to_bits().hash(state);
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Deserialize, Hash, Eq, PartialEq, PartialOrd, Ord, Serialize, PeekPoke)]
pub struct GlyphOptions {
    pub render_mode: FontRenderMode,
    pub flags: FontInstanceFlags,
}

impl Default for GlyphOptions {
    fn default() -> Self {
        GlyphOptions {
            render_mode: FontRenderMode::Subpixel,
            flags: FontInstanceFlags::empty(),
        }
    }
}

#[repr(C)]
#[derive(Copy, PartialEq, Eq, Clone, PartialOrd, Ord, Hash, Deserialize, MallocSizeOf, Serialize, PeekPoke)]
pub struct FontInstanceFlags(u32);

bitflags! {
    impl FontInstanceFlags: u32 {
        const SYNTHETIC_BOLD    = 1 << 1;
        const EMBEDDED_BITMAPS  = 1 << 2;
        const SUBPIXEL_BGR      = 1 << 3;
        const TRANSPOSE         = 1 << 4;
        const FLIP_X            = 1 << 5;
        const FLIP_Y            = 1 << 6;
        const SUBPIXEL_POSITION = 1 << 7;
        const VERTICAL          = 1 << 8;
        const MULTISTRIKE_BOLD  = 1 << 9;

        const TRANSFORM_GLYPHS  = 1 << 12;
        const TEXTURE_PADDING   = 1 << 13;

        const FORCE_GDI         = 1 << 16;
        const FORCE_SYMMETRIC   = 1 << 17;
        const NO_SYMMETRIC      = 1 << 18;

        const FONT_SMOOTHING    = 1 << 16;

        const FORCE_AUTOHINT    = 1 << 16;
        const NO_AUTOHINT       = 1 << 17;
        const VERTICAL_LAYOUT   = 1 << 18;
        const LCD_VERTICAL      = 1 << 19;
    }
}

impl core::fmt::Debug for FontInstanceFlags {
    fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
        if self.is_empty() {
            write!(f, "{:#x}", Self::empty().bits())
        } else {
            bitflags::parser::to_writer(self, f)
        }
    }
}

impl Default for FontInstanceFlags {

    fn default() -> FontInstanceFlags {
        FontInstanceFlags::SUBPIXEL_POSITION
    }
}


#[repr(C)]
#[derive(Clone, Copy, Debug, Deserialize, Hash, Eq, MallocSizeOf, PartialEq, PartialOrd, Ord, Serialize)]
pub struct SyntheticItalics {
    pub angle: i16,
}

impl SyntheticItalics {
    pub const ANGLE_SCALE: f32 = 256.0;

    pub fn from_degrees(degrees: f32) -> Self {
        SyntheticItalics { angle: (degrees.clamp(-89.0, 89.0) * Self::ANGLE_SCALE) as i16 }
    }

    pub fn to_degrees(self) -> f32 {
        self.angle as f32 / Self::ANGLE_SCALE
    }

    pub fn to_radians(self) -> f32 {
        self.to_degrees().to_radians()
    }

    pub fn to_skew(self) -> f32 {
        self.to_radians().tan()
    }

    pub fn enabled() -> Self {
        Self::from_degrees(14.0)
    }

    pub fn disabled() -> Self {
        SyntheticItalics { angle: 0 }
    }

    pub fn is_enabled(self) -> bool {
        self.angle != 0
    }
}

impl Default for SyntheticItalics {
    fn default() -> Self {
        SyntheticItalics::disabled()
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Deserialize, Hash, Eq, MallocSizeOf, PartialEq, PartialOrd, Ord, Serialize)]
pub struct FontInstanceOptions {
    pub flags: FontInstanceFlags,
    pub synthetic_italics: SyntheticItalics,
    pub render_mode: FontRenderMode,
    pub _padding: u8,
}

impl Default for FontInstanceOptions {
    fn default() -> FontInstanceOptions {
        FontInstanceOptions {
            render_mode: FontRenderMode::Subpixel,
            flags: Default::default(),
            synthetic_italics: SyntheticItalics::disabled(),
            _padding: 0,
        }
    }
}


#[repr(u8)]
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, MallocSizeOf, PartialEq, PartialOrd, Ord, Serialize)]
pub enum FontLCDFilter {
    None,
    Default,
    Light,
    Legacy,
}


#[repr(u8)]
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, MallocSizeOf, PartialEq, PartialOrd, Ord, Serialize)]
pub enum FontHinting {
    None,
    Mono,
    Light,
    Normal,
    LCD,
}


#[repr(C)]
#[derive(Clone, Copy, Debug, Deserialize, Hash, Eq, MallocSizeOf, PartialEq, PartialOrd, Ord, Serialize)]
pub struct FontInstancePlatformOptions {
    pub lcd_filter: FontLCDFilter,
    pub hinting: FontHinting,
    pub gamma: i16,
    pub enhanced_contrast: i16,
}


impl Default for FontInstancePlatformOptions {
    fn default() -> FontInstancePlatformOptions {
        FontInstancePlatformOptions {
            lcd_filter: FontLCDFilter::Default,
            hinting: FontHinting::LCD,
            gamma: -1,
            enhanced_contrast: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, Hash, PartialEq, Ord, PartialOrd, MallocSizeOf, PeekPoke)]
#[derive(Deserialize, Serialize)]
pub struct FontInstanceKey(pub IdNamespace, pub u32);

impl FontInstanceKey {
    pub fn new(namespace: IdNamespace, key: u32) -> FontInstanceKey {
        FontInstanceKey(namespace, key)
    }
}

/// Data corresponding to an instantiation of a font, with size and
/// other options specified.
///
/// Note that the actual font is stored out-of-band in `FontTemplate`.
#[derive(Clone)]
pub struct FontInstanceData {
    pub font_key: FontKey,
    pub size: f32,
    pub options: Option<FontInstanceOptions>,
    pub platform_options: Option<FontInstancePlatformOptions>,
    pub variations: Vec<FontVariation>,
}

pub type GlyphIndex = u32;

#[repr(C)]
#[derive(Clone, Copy, Debug, Deserialize, MallocSizeOf, PartialEq, Serialize, PeekPoke)]
pub struct GlyphInstance {
    pub index: GlyphIndex,
    pub point: LayoutPoint,
}

impl Default for GlyphInstance {
    fn default() -> Self {
        GlyphInstance {
            index: 0,
            point: LayoutPoint::zero(),
        }
    }
}

impl Eq for GlyphInstance {}

#[allow(clippy::derived_hash_with_manual_eq)]
impl Hash for GlyphInstance {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.index.hash(state);
        self.point.x.to_bits().hash(state);
        self.point.y.to_bits().hash(state);
    }
}
