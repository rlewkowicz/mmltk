use crate::Size;

use std::fmt;

#[derive(Debug, Hash, Clone, Copy, PartialEq, Eq, Default)]
pub enum ContentFit {
                                            #[default]
    Contain,

                                            Cover,

                        Fill,

                                None,

                            ScaleDown,
}

impl ContentFit {
                pub fn fit(&self, content: Size, bounds: Size) -> Size {
        let content_ar = content.width / content.height;
        let bounds_ar = bounds.width / bounds.height;

        match self {
            Self::Contain => {
                if bounds_ar > content_ar {
                    Size {
                        width: content.width * bounds.height / content.height,
                        ..bounds
                    }
                } else {
                    Size {
                        height: content.height * bounds.width / content.width,
                        ..bounds
                    }
                }
            }
            Self::Cover => {
                if bounds_ar < content_ar {
                    Size {
                        width: content.width * bounds.height / content.height,
                        ..bounds
                    }
                } else {
                    Size {
                        height: content.height * bounds.width / content.width,
                        ..bounds
                    }
                }
            }
            Self::Fill => bounds,
            Self::None => content,
            Self::ScaleDown => {
                if bounds_ar > content_ar && bounds.height < content.height {
                    Size {
                        width: content.width * bounds.height / content.height,
                        ..bounds
                    }
                } else if bounds.width < content.width {
                    Size {
                        height: content.height * bounds.width / content.width,
                        ..bounds
                    }
                } else {
                    content
                }
            }
        }
    }
}

impl fmt::Display for ContentFit {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            ContentFit::Contain => "Contain",
            ContentFit::Cover => "Cover",
            ContentFit::Fill => "Fill",
            ContentFit::None => "None",
            ContentFit::ScaleDown => "Scale Down",
        })
    }
}
