use crate::generated::{AnnotationShape, AnnotationUiState};

pub(super) fn sample_pixel(ui: &AnnotationUiState) -> [u16; 2] {
    // Keep the clean-image sample inside the image. A corner sample can land
    // on a filtered mask-cleanup boundary in the compact display.
    [ui.scene.framewidth / 2, ui.scene.frameheight / 2]
}

// Expected source-space sample positions follow public object geometry. The
// browser samples the completed canvas independently of the native rasterizer.
pub(super) fn probes(ui: &AnnotationUiState) -> Vec<f64> {
    let mut samples = Vec::new();
    let mut add = |x: f32, y: f32, color: iced::Color| {
        if x >= 0.0
            && y >= 0.0
            && x < f32::from(ui.scene.framewidth)
            && y < f32::from(ui.scene.frameheight)
        {
            samples.extend([
                f64::from(x),
                f64::from(y),
                f64::from(color.r) * 255.0,
                f64::from(color.g) * 255.0,
                f64::from(color.b) * 255.0,
                24.0,
                2.0,
            ]);
        }
    };
    for (index, object) in ui.scene.objects.iter().enumerate() {
        if !object.enabled {
            continue;
        }
        let selected = ui.editor.selectedobject == Some(index as u16);
        // Unselected singleton splines must remain observable too.
        if !selected && !(object.shape == AnnotationShape::Spline && object.splineknots.len() == 1)
        {
            continue;
        }
        let Some(color) = ui.scene.palette.get(object.category as usize) else {
            continue;
        };
        let color = crate::presentation_surface::labels::class_color(color);
        match object.shape {
            AnnotationShape::Box | AnnotationShape::Mask => {
                let b = &object.box_;
                add(b.first.x - 1.0, (b.first.y + b.second.y) / 2.0, color);
                add(b.second.x, (b.first.y + b.second.y) / 2.0, color);
                if selected {
                    add(b.first.x - 5.0, b.first.y - 5.0, iced::Color::WHITE);
                    add(b.second.x + 4.0, b.second.y + 4.0, iced::Color::WHITE);
                }
                if object.shape == AnnotationShape::Mask && object.sup.sampling {
                    let [x, y] = sample_pixel(ui);
                    let clean =
                        crate::presentation_surface::labels::class_color(&object.sup.center);
                    let alpha = if object
                        .mask
                        .runs
                        .iter()
                        .any(|run| run.row == y && run.first <= x && run.last >= x)
                    {
                        92.0 / 255.0
                    } else {
                        0.0
                    };
                    add(
                        f32::from(x) + 0.5,
                        f32::from(y) + 0.5,
                        iced::Color::from_rgb(
                            clean.r * (1.0 - alpha) + color.r * alpha,
                            clean.g * (1.0 - alpha) + color.g * alpha,
                            clean.b * (1.0 - alpha) + color.b * alpha,
                        ),
                    );
                }
            }
            AnnotationShape::Point => add(object.point.x, object.point.y, color),
            AnnotationShape::Spline => {
                for knot in &object.splineknots {
                    add(knot.point.x, knot.point.y, color);
                    if selected && knot.in_.enabled {
                        add(knot.in_.point.x, knot.in_.point.y, color);
                    }
                    if selected && knot.out.enabled {
                        add(knot.out.point.x, knot.out.point.y, color);
                    }
                }
                if object.splineknots.len() > 1 {
                    let a = &object.splineknots[0];
                    let b = &object.splineknots[1];
                    let c1 = if a.out.enabled {
                        &a.out.point
                    } else {
                        &a.point
                    };
                    let c2 = if b.in_.enabled {
                        &b.in_.point
                    } else {
                        &b.point
                    };
                    add(
                        (a.point.x + 3.0 * c1.x + 3.0 * c2.x + b.point.x) / 8.0,
                        (a.point.y + 3.0 * c1.y + 3.0 * c2.y + b.point.y) / 8.0,
                        color,
                    );
                }
            }
            AnnotationShape::Skeleton => {
                for node in &object.skeletonnodes {
                    if node.visible {
                        add(node.point.x, node.point.y, color);
                    }
                }
                for edge in &object.skeletonedges {
                    let a = &object.skeletonnodes[edge.source as usize];
                    let b = &object.skeletonnodes[edge.target as usize];
                    if a.visible && b.visible {
                        add(
                            (a.point.x + b.point.x) / 2.0,
                            (a.point.y + b.point.y) / 2.0,
                            color,
                        );
                    }
                }
            }
        }
    }
    samples
}

#[cfg(target_arch = "wasm32")]
pub(super) fn place_gesture(
    bounds: iced::Rectangle,
    extent: (f64, f64),
    points: [f64; 4],
) -> [f64; 4] {
    let (width, height) = extent;
    let scale = (f64::from(bounds.width) / width).min(f64::from(bounds.height) / height);
    let left = (f64::from(bounds.width) - width * scale) / 2.0;
    let top = (f64::from(bounds.height) - height * scale) / 2.0;
    [
        (left + points[0] * scale) / f64::from(bounds.width),
        (top + points[1] * scale) / f64::from(bounds.height),
        (left + points[2] * scale) / f64::from(bounds.width),
        (top + points[3] * scale) / f64::from(bounds.height),
    ]
}
