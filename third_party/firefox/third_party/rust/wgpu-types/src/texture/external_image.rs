/// Color spaces supported on the web.
///
/// Corresponds to [HTML Canvas `PredefinedColorSpace`](
/// https://html.spec.whatwg.org/multipage/canvas.html#predefinedcolorspace).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[cfg_attr(feature = "serde", serde(rename_all = "kebab-case"))]
pub enum PredefinedColorSpace {
    /// sRGB color space
    Srgb,
    /// Display-P3 color space
    DisplayP3,
}

/// View of a texture which can be used to copy to a texture, including
/// color space and alpha premultiplication information.
///
/// Corresponds to [WebGPU `GPUCopyExternalImageDestInfo`](
/// https://gpuweb.github.io/gpuweb/#dictdef-gpuimagecopytexturetagged).
#[derive(Copy, Clone, Debug)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct CopyExternalImageDestInfo<T> {
    /// The texture to be copied to/from.
    pub texture: T,
    /// The target mip level of the texture.
    pub mip_level: u32,
    /// The base texel of the texture in the selected `mip_level`.
    pub origin: crate::Origin3d,
    /// The copy aspect.
    pub aspect: crate::TextureAspect,
    /// The color space of this texture.
    pub color_space: PredefinedColorSpace,
    /// The premultiplication of this texture
    pub premultiplied_alpha: bool,
}

impl<T> CopyExternalImageDestInfo<T> {
    /// Removes the colorspace information from the type.
    pub fn to_untagged(self) -> crate::TexelCopyTextureInfo<T> {
        crate::TexelCopyTextureInfo {
            texture: self.texture,
            mip_level: self.mip_level,
            origin: self.origin,
            aspect: self.aspect,
        }
    }
}
