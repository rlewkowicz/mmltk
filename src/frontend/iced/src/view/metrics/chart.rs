use super::{
    catalog::Chart,
    history::{BUCKETS, Curve},
};
use iced_plot::{
    AxisScale, LineStyle, MarkerStyle, PlotWidget, PlotWidgetBuilder, Series, ShapeId,
};

pub(super) struct RetainedChart {
    pub kind: Chart,
    pub visible: bool,
    pub dirty: bool,
    pub plot: PlotWidget,
    pub shapes: Vec<Option<ShapeId>>,
    positions: Vec<[f64; 2]>,
    labels: Vec<String>,
    axes: Option<(bool, bool)>,
}
impl RetainedChart {
    pub fn new(kind: Chart, count: usize) -> Self {
        let mut plot = PlotWidgetBuilder::new()
            .with_autoscale_on_updates(true)
            .with_axis_label_size(11.0)
            .with_tick_label_size(10.0)
            .build()
            .expect("valid plot");
        plot.update(iced_plot::PlotUiMessage::ToggleLegend);
        plot.set_cursor_overlay(true);
        plot.set_crosshairs(true);
        plot.get_controls_mut().clear_scroll_bindings();
        Self {
            axes: None,
            labels: vec![String::new(); count],
            kind,
            visible: kind.main(),
            dirty: true,
            plot,
            shapes: vec![None; count],
            positions: Vec::with_capacity(BUCKETS * 5),
        }
    }
    pub fn prepare(
        &mut self,
        curves: &[Curve],
        metrics: &[super::catalog::Metric],
        epoch: bool,
        log: bool,
    ) {
        if !self.dirty {
            return;
        }
        if self.axes != Some((epoch, log)) {
            self.plot
                .set_x_axis_label(if epoch { "Epoch" } else { "Optimizer step" });
            self.plot.set_y_axis_label(match self.kind {
                Chart::Loss | Chart::Components => "Loss",
                Chart::LearningRate => "Rate",
                Chart::Errors => "Error",
                _ => "Score",
            });
            self.plot.set_y_axis_scale(if log && self.kind.loss() {
                AxisScale::Log { base: 10.0 }
            } else {
                AxisScale::Linear
            });
            self.axes = Some((epoch, log));
        }
        for (index, (curve, _metric)) in curves
            .iter()
            .zip(metrics)
            .enumerate()
            .filter(|(_, (_, metric))| metric.chart == self.kind)
        {
            self.positions.clear();
            let mut segment = None;
            for bucket in &curve.buckets {
                if segment != Some(bucket.segment) && !self.positions.is_empty() {
                    self.positions.push([f64::NAN; 2]);
                }
                segment = Some(bucket.segment);
                let mut points = [bucket.first, bucket.min, bucket.max, bucket.last];
                points.sort_by_key(|point| point.order);
                let mut previous = None;
                for point in points {
                    if previous != Some(point.order) {
                        self.positions
                            .push([if epoch { point.epoch } else { point.step }, point.value]);
                        previous = Some(point.order);
                    }
                }
            }
            crate::integration_control::report_metric_projection(&curve.name, &self.positions);
            if let Some(id) = self.shapes[index] {
                self.plot.set_series_positions(&id, &self.positions);
                if self.labels[index] != curve.name {
                    let _ = self
                        .plot
                        .update_series(&id, |series| series.label = Some(curve.name.clone()));
                    self.labels[index].clone_from(&curve.name);
                }
            } else if !self.positions.is_empty() {
                let color = crate::presentation_surface::labels::class_color(
                    &crate::generated::AnnotationColor {
                        hue: index as f32 * 137.5,
                        saturation: 0.8,
                        value: 0.8,
                    },
                );
                let series = Series::new(
                    self.positions.clone(),
                    MarkerStyle::circle(3.0),
                    LineStyle::solid().with_pixel_width(1.5),
                )
                .with_label(curve.name.clone())
                .with_color(color);
                self.shapes[index] = Some(series.id);
                self.labels[index].clone_from(&curve.name);
                let _ = self.plot.add_series(series);
            }
        }
        self.dirty = false;
    }
}
