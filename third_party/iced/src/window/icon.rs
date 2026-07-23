pub use crate::core::window::icon::*;

use crate::core::window::icon;

use std::io;

#[cfg(feature = "image")]
use std::path::Path;

#[cfg(feature = "image")]
pub fn from_file<P: AsRef<Path>>(icon_path: P) -> Result<Icon, Error> {
    let icon = image::ImageReader::open(icon_path)?.decode()?.to_rgba8();

    Ok(icon::from_rgba(icon.to_vec(), icon.width(), icon.height())?)
}

#[cfg(feature = "image")]
pub fn from_file_data(
    data: &[u8],
    explicit_format: Option<image::ImageFormat>,
) -> Result<Icon, Error> {
    let mut icon = image::ImageReader::new(std::io::Cursor::new(data));

    let icon_with_format = match explicit_format {
        Some(format) => {
            icon.set_format(format);
            icon
        }
        None => icon.with_guessed_format()?,
    };

    let pixels = icon_with_format.decode()?.to_rgba8();

    Ok(icon::from_rgba(
        pixels.to_vec(),
        pixels.width(),
        pixels.height(),
    )?)
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
        #[error("The icon is invalid: {0}")]
    InvalidError(#[from] icon::Error),

        #[error("The underlying OS failed to create the window icon: {0}")]
    OsError(#[from] io::Error),

        #[cfg(feature = "image")]
    #[error("Unable to create icon from a file: {0}")]
    ImageError(#[from] image::error::ImageError),
}
