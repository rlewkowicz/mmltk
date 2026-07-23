use crate::Pixels;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Length {
                    Fit,

        Fill,

                                FillPortion(u16),

        Shrink,

        Fixed(f32),

        Bounded {
                bounds: Bounds,
                sizing: Sizing,
    },

            Fluid(Constraint),
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Constraint {
        Min(f32),
            Max,
}

impl Constraint {
    fn stack(self, other: Self) -> Self {
        match (self, other) {
            (Constraint::Min(a), Constraint::Min(b)) => Self::Min(a + b),
            (Constraint::Min(min), Constraint::Max) | (Constraint::Max, Constraint::Min(min)) => {
                Constraint::Min(min)
            }
            (Constraint::Max, Constraint::Max) => Self::Max,
        }
    }

    fn cross(self, other: Self) -> Self {
        match (self, other) {
            (Constraint::Min(a), Constraint::Min(b)) => Self::Min(a.max(b)),
            (Constraint::Min(min), Constraint::Max) | (Constraint::Max, Constraint::Min(min)) => {
                Constraint::Min(min)
            }
            (Constraint::Max, Constraint::Max) => Self::Max,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Bounds {
        Min(f32),
        Max(f32),
        Both {
                min: f32,
                max: f32,
    },
}

impl Bounds {
        pub fn min(self, min: f32) -> Self {
        match self {
            Self::Min(_) => Self::Min(min),
            Self::Max(max) | Self::Both { max, .. } => Self::Both { min, max },
        }
    }

        pub fn max(self, max: f32) -> Self {
        match self {
            Self::Max(_) => Self::Max(max),
            Self::Min(min) | Self::Both { min, .. } => Self::Both { min, max },
        }
    }

        pub fn constraint(self) -> Constraint {
        match self {
            Bounds::Min(min) | Bounds::Both { min, .. } => Constraint::Min(min),
            Bounds::Max(_) => Constraint::Max,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Sizing {
        Fit,
        Fill(u16),
        Shrink,
}

impl Length {
        pub fn min(self, min: impl Into<Pixels>) -> Self {
        let min = min.into().0;

        let with = match self {
            Self::Fit | Self::Fluid(_) => Sizing::Fit,
            Self::Fill => Sizing::Fill(1),
            Self::FillPortion(factor) => Sizing::Fill(factor),
            Self::Shrink => Sizing::Shrink,
            Self::Fixed(_) => return self,
            Self::Bounded {
                bounds,
                sizing: with,
            } => {
                return Self::Bounded {
                    bounds: bounds.min(min),
                    sizing: with,
                };
            }
        };

        Self::Bounded {
            bounds: Bounds::Min(min),
            sizing: with,
        }
    }

        pub fn max(self, max: impl Into<Pixels>) -> Self {
        let max = max.into().0;

        let with = match self {
            Self::Fit | Self::Fluid(_) => Sizing::Fit,
            Self::Fill => Sizing::Fill(1),
            Self::FillPortion(factor) => Sizing::Fill(factor),
            Self::Shrink => Sizing::Shrink,
            Self::Fixed(_) => return self,
            Self::Bounded {
                bounds,
                sizing: with,
            } => {
                return Self::Bounded {
                    bounds: bounds.max(max),
                    sizing: with,
                };
            }
        };

        Self::Bounded {
            bounds: Bounds::Max(max),
            sizing: with,
        }
    }

                        pub fn fill_factor(&self) -> u16 {
        match self {
            Length::Fill => 1,
            Length::FillPortion(factor)
            | Length::Bounded {
                sizing: Sizing::Fill(factor),
                ..
            } => *factor,
            Length::Fluid(_) => 1,
            Length::Shrink | Length::Fit | Length::Fixed(_) | Length::Bounded { .. } => 0,
        }
    }

            pub fn is_fill(&self) -> bool {
        self.fill_factor() != 0
    }

        pub fn is_fit(&self) -> bool {
        matches!(self, Self::Fit)
    }

                            pub fn stack(self, other: Length) -> Self {
        self.merge_with(other, Constraint::stack)
    }

                            pub fn cross(self, other: Length) -> Self {
        self.merge_with(other, Constraint::cross)
    }

    fn merge_with(self, other: Self, merge: impl Fn(Constraint, Constraint) -> Constraint) -> Self {
        match (self, other) {
            (Length::Shrink | Length::Fixed(_) | Length::Fill | Length::FillPortion(_), _) => self,

            (Length::Fluid(a), Length::Fluid(b)) => Length::Fluid(merge(a, b)),
            (
                Length::Fluid(a),
                Length::Bounded {
                    bounds,
                    sizing: Sizing::Fill(_),
                },
            ) => Length::Fluid(merge(a, bounds.constraint())),
            (Length::Fluid(constraint), _) => Length::Fluid(constraint),

            (
                Length::Bounded {
                    bounds,
                    sizing: Sizing::Fit,
                },
                Length::Fill
                | Length::FillPortion(_)
                | Length::Bounded {
                    sizing: Sizing::Fill(_),
                    ..
                },
            ) => Length::Bounded {
                bounds,
                sizing: Sizing::Fill(1),
            },
            (
                Length::Bounded {
                    bounds,
                    sizing: with,
                },
                _,
            ) => Length::Bounded {
                bounds,
                sizing: with,
            },

            (
                _,
                Length::Bounded {
                    bounds,
                    sizing: Sizing::Fill(_),
                },
            ) => Length::Fluid(bounds.constraint()),
            (_, Length::Fluid(constraint)) => Length::Fluid(constraint),

            (_, Length::Fill | Length::FillPortion(_)) => Length::Fill,

            _ => Length::Fit,
        }
    }
}

impl From<Pixels> for Length {
    fn from(amount: Pixels) -> Self {
        Self::Fixed(f32::from(amount))
    }
}

impl From<f32> for Length {
    fn from(amount: f32) -> Self {
        Self::Fixed(amount)
    }
}

impl From<u32> for Length {
    fn from(units: u32) -> Self {
        Self::Fixed(units as f32)
    }
}
