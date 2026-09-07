//! Validation of a surface configuration against its capabilities, including
//! resolving `SurfaceColorSpace::Auto` (and present/alpha `Auto`) to concrete
//! values. Split out of the very large `resource.rs`.

use crate::{api_log, present};
use wgt::TextureFormat;

/// The concrete color space [`SurfaceColorSpace::Auto`] resolves to for `format`,
/// given the color spaces a surface supports for it, or `None` if `Auto` cannot
/// be satisfied.
///
/// Reproduces wgpu's historical behavior: extended linear scRGB for fp16 formats
/// when supported, sRGB otherwise. `Auto` never resolves to a wide-gamut or HDR
/// color space (DisplayP3, ExtendedSrgb, ExtendedDisplayP3, Bt2100Pq, Bt2100Hlg),
/// because those change how the application must encode its output, so they must
/// be requested explicitly.
///
/// This is the single source of truth shared by [`validate_surface_configuration`]
/// and the `get_capabilities` `formats` filter, so a format is listed in
/// [`SurfaceCapabilities::formats`] exactly when `Auto` resolves for it.
///
/// [`SurfaceColorSpace::Auto`]: wgt::SurfaceColorSpace::Auto
/// [`SurfaceCapabilities::formats`]: wgt::SurfaceCapabilities::formats
pub(crate) fn resolve_auto_color_space(
    format: TextureFormat,
    color_spaces: wgt::SurfaceColorSpaces,
) -> Option<wgt::SurfaceColorSpace> {
    let fallbacks: &[_] = if format == TextureFormat::Rgba16Float {
        &[
            wgt::SurfaceColorSpace::ExtendedSrgbLinear,
            wgt::SurfaceColorSpace::Srgb,
        ]
    } else {
        &[wgt::SurfaceColorSpace::Srgb]
    };
    fallbacks
        .iter()
        .copied()
        .find(|fallback| color_spaces.contains(fallback.to_color_spaces().unwrap()))
}

/// Validate `config` against `caps`, resolving the `Auto` values in
/// `config` to concrete ones.
pub(crate) fn validate_surface_configuration(
    config: &mut hal::SurfaceConfiguration,
    caps: &hal::SurfaceCapabilities,
    max_texture_dimension_2d: u32,
) -> Result<(), present::ConfigureSurfaceError> {
    use present::ConfigureSurfaceError as E;
    let width = config.extent.width;
    let height = config.extent.height;

    if width > max_texture_dimension_2d || height > max_texture_dimension_2d {
        return Err(E::TooLarge {
            width,
            height,
            max_texture_dimension_2d,
        });
    }

    if !caps.present_modes.contains(&config.present_mode) {
        let fallbacks = match config.present_mode {
            wgt::PresentMode::AutoVsync => {
                &[wgt::PresentMode::FifoRelaxed, wgt::PresentMode::Fifo][..]
            }
            wgt::PresentMode::AutoNoVsync => &[
                wgt::PresentMode::Immediate,
                wgt::PresentMode::Mailbox,
                wgt::PresentMode::Fifo,
            ][..],
            _ => {
                return Err(E::UnsupportedPresentMode {
                    requested: config.present_mode,
                    available: caps.present_modes.clone(),
                });
            }
        };

        let new_mode = fallbacks
            .iter()
            .copied()
            .find(|fallback| caps.present_modes.contains(fallback))
            .unwrap_or_else(|| {
                unreachable!(
                    "Fallback system failed to choose present mode. \
                    This is a bug. Mode: {:?}, Options: {:?}",
                    config.present_mode, &caps.present_modes
                );
            });

        api_log!(
            "Automatically choosing presentation mode by rule {:?}. Chose {new_mode:?}",
            config.present_mode
        );
        config.present_mode = new_mode;
    }
    let Some(format_caps) = caps.formats.iter().find(|fc| fc.format == config.format) else {
        return Err(E::UnsupportedFormat {
            requested: config.format,
            available: caps.texture_formats().collect(),
        });
    };
    if config.color_space == wgt::SurfaceColorSpace::Auto {
        let Some(new_color_space) =
            resolve_auto_color_space(config.format, format_caps.color_spaces)
        else {
            return Err(E::UnsupportedColorSpace {
                requested: config.color_space,
                format: config.format,
                available: format_caps.color_spaces,
            });
        };

        api_log!(
            "Automatically choosing color space by rule {:?}. Chose {new_color_space:?}",
            config.color_space
        );
        config.color_space = new_color_space;
    }
    if !format_caps
        .color_spaces
        .contains(config.color_space.to_color_spaces().unwrap())
    {
        return Err(E::UnsupportedColorSpace {
            requested: config.color_space,
            format: config.format,
            available: format_caps.color_spaces,
        });
    }
    if !caps
        .composite_alpha_modes
        .contains(&config.composite_alpha_mode)
    {
        let new_alpha_mode = 'alpha: {
            let fallbacks = match config.composite_alpha_mode {
                wgt::CompositeAlphaMode::Auto => &[
                    wgt::CompositeAlphaMode::Opaque,
                    wgt::CompositeAlphaMode::Inherit,
                ][..],
                _ => {
                    return Err(E::UnsupportedAlphaMode {
                        requested: config.composite_alpha_mode,
                        available: caps.composite_alpha_modes.clone(),
                    });
                }
            };

            for &fallback in fallbacks {
                if caps.composite_alpha_modes.contains(&fallback) {
                    break 'alpha fallback;
                }
            }

            unreachable!(
                "Fallback system failed to choose alpha mode. This is a bug. \
                          AlphaMode: {:?}, Options: {:?}",
                config.composite_alpha_mode, &caps.composite_alpha_modes
            );
        };

        api_log!(
            "Automatically choosing alpha mode by rule {:?}. Chose {new_alpha_mode:?}",
            config.composite_alpha_mode
        );
        config.composite_alpha_mode = new_alpha_mode;
    }
    if !caps.usage.contains(config.usage) {
        return Err(E::UnsupportedUsage {
            requested: config.usage,
            available: caps.usage,
        });
    }
    if width == 0 || height == 0 {
        return Err(E::ZeroArea);
    }
    Ok(())
}
