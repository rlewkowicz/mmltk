use crate::Theme;
use crate::Element;
use core::fmt;
use std::{
    collections::{HashMap, HashSet},
    sync::{
        Arc, RwLock,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
};

use glam::{DVec2, Vec2};
use iced::{
    Color, Length, Rectangle,
    alignment::{self, Horizontal, Vertical},
    keyboard,
    mouse::{self, Interaction},
    padding::{self, Padding},
    wgpu::TextureFormat,
    widget::{
        self, container,
        shader::{self, Pipeline, Viewport},
        stack,
    },
};
use indexmap::IndexMap;

use crate::{
    AxisScale, DragEvent, Fill, HLine, HoverPickEvent, KeyAction, MarkerStyle, PlotUiMessage,
    PointId, Series, Size, TooltipContext, Transform, VLine, axes_labels,
    axis_link::AxisLink,
    axis_scale::plot_point_to_data,
    camera::Camera,
    controls::PlotControls,
    default_style,
    legend::{self, LegendEntry},
    message::{CursorPositionUiPayload, PlotRenderUpdate, TooltipUiPayload},
    picking, plot_overlay,
    plot_renderer::{PlotRenderStrategy, PlotRenderer, RenderParams},
    plot_state::PlotState,
    series::{SeriesError, ShapeId},
    style::{PlotStyle, StyleFn},
    ticks::{self, PositionedTick, TickFormatter, TickProducer},
    transform::{PositionTransform, data_point_to_plot_with_transform},
};

const PLOT_CONTENT_PADDING: f32 = 2.0;
pub(crate) type CursorProvider = Arc<dyn Fn(f64, f64) -> String + Send + Sync>;

/// Provider for highlighting a point.
///
/// Modifies the highlight point in mutable reference.
///
/// Returns the tooltip text to display for the point, if any.
pub(crate) type HighlightPointProvider =
    Arc<dyn Fn(TooltipContext<'_>, &mut HighlightPoint) -> Option<String> + Send + Sync>;

/// The exact source incorporated by a published camera. Tree-local projection
/// versions do not establish that a retained plot has consumed a data update.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct ViewSource {
    instance: u64,
    data: u64,
    x_lim: Option<(f64, f64)>,
    y_lim: Option<(f64, f64)>,
    x_scale: AxisScale,
    y_scale: AxisScale,
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct SettledView {
    source: ViewSource,
    prev_data_max_x: Option<f64>,
    prev_last_point_y: Option<f64>,
    follow_reset_seen: u64,
}

/// A plot widget that renders data series with interactive features.
pub struct PlotWidget {
    pub(crate) instance_id: u64,
    // Data
    pub(crate) series: IndexMap<ShapeId, Series>,
    pub(crate) fills: IndexMap<ShapeId, Fill>,
    pub(crate) vlines: IndexMap<ShapeId, VLine>,
    pub(crate) hlines: IndexMap<ShapeId, HLine>,
    pub(crate) hidden_shapes: HashSet<ShapeId>,
    pub(crate) data_version: u64,
    pub(crate) reference_version: u64,
    // Configuration
    pub(crate) autoscale_on_updates: bool,
    pub(crate) controls: PlotControls,
    pub(crate) highlight_on_hover: bool,
    pub(crate) show_controls_help: bool,
    pub(crate) legend_enabled: bool,
    pub(crate) legend_collapsed: bool,
    pub(crate) x_axis_label: String,
    pub(crate) y_axis_label: String,
    pub(crate) x_lim: Option<(f64, f64)>,
    pub(crate) y_lim: Option<(f64, f64)>,
    /// Follow mode: automatically scale Y-axis on each data update.
    /// X remains under user control (pan/zoom is preserved).
    pub(crate) autoscale_y_on_updates: bool,
    /// Follow mode: locks camera X to the right edge (data_max.x) on each data update.
    /// Zoom level (half_extents.x) is preserved — user can zoom in/out.
    pub(crate) follow_right_edge: bool,
    pub(crate) follow_reset_counter: u64,
    pub(crate) x_axis_scale: AxisScale,
    pub(crate) y_axis_scale: AxisScale,
    pub(crate) x_axis_link: Option<AxisLink>,
    pub(crate) y_axis_link: Option<AxisLink>,
    pub(crate) hover_radius_px: f32,
    pub(crate) pick_highlight_provider: Option<HighlightPointProvider>,
    pub(crate) hover_highlight_provider: Option<HighlightPointProvider>,
    pub(crate) cursor_overlay: bool,
    pub(crate) cursor_provider: Option<CursorProvider>,
    pub(crate) crosshairs_enabled: bool,
    pub(crate) render_strategy: PlotRenderStrategy,
    pub(crate) controls_overlay_open: bool,
    #[cfg(feature = "canvas")]
    pub(crate) canvas_caches: crate::plot_renderer::canvas::CanvasCaches,
    pub(crate) x_axis_formatter: Option<TickFormatter>,
    pub(crate) y_axis_formatter: Option<TickFormatter>,
    pub(crate) x_tick_producer: Option<TickProducer>,
    pub(crate) y_tick_producer: Option<TickProducer>,
    pub(crate) tick_label_size: f32,
    pub(crate) axis_label_size: f32,
    pub(crate) data_aspect: Option<f64>,
    pub(crate) style: StyleFn,
    pub(crate) resolved_style: RwLock<PlotStyle>,
    // UI state
    /// Map of picked point id to highlight point data & tooltip text.
    pub(crate) picked_points: IndexMap<PointId, (HighlightPoint, Option<TooltipUiPayload>)>,
    /// Map of hovered point id to highlight point data & tooltip text.
    pub(crate) hovered_points: IndexMap<PointId, (HighlightPoint, Option<TooltipUiPayload>)>,
    pub(crate) cursor_ui: Option<CursorPositionUiPayload>,
    pub(crate) x_ticks: Arc<Vec<PositionedTick>>,
    pub(crate) y_ticks: Arc<Vec<PositionedTick>>,
    pub(crate) shape_overlays_enabled: AtomicBool,
    // Camera and bounds for coordinate conversion (updated when ticks are updated)
    pub(crate) camera_bounds: Option<(Camera, Rectangle)>,
    settled_view: Option<SettledView>,
    next_publication: AtomicU64,
    accepted_publication: u64,
}

impl Default for PlotWidget {
    fn default() -> Self {
        Self::new()
    }
}

impl PlotWidget {
    /// Create a new plot widget with default settings.
    pub fn new() -> Self {
        Self {
            instance_id: NEXT_ID.fetch_add(1, Ordering::Relaxed),
            series: IndexMap::new(),
            fills: IndexMap::new(),
            vlines: IndexMap::new(),
            hlines: IndexMap::new(),
            hidden_shapes: HashSet::new(),
            data_version: 1,
            reference_version: 1,
            autoscale_on_updates: false,
            controls: PlotControls::default(),
            highlight_on_hover: true,
            show_controls_help: true,
            legend_enabled: true,
            legend_collapsed: false,
            x_axis_label: String::new(),
            y_axis_label: String::new(),
            x_lim: None,
            y_lim: None,
            autoscale_y_on_updates: false,
            follow_right_edge: false,
            follow_reset_counter: 0,
            x_axis_scale: AxisScale::Linear,
            y_axis_scale: AxisScale::Linear,
            x_axis_link: None,
            y_axis_link: None,
            hover_radius_px: 8.0,
            pick_highlight_provider: None,
            hover_highlight_provider: None,
            cursor_overlay: true,
            cursor_provider: None,
            crosshairs_enabled: false,
            render_strategy: PlotRenderStrategy::default(),
            controls_overlay_open: false,
            #[cfg(feature = "canvas")]
            canvas_caches: crate::plot_renderer::canvas::CanvasCaches::default(),
            x_axis_formatter: Some(Arc::new(ticks::default_formatter)),
            y_axis_formatter: Some(Arc::new(ticks::default_formatter)),
            x_tick_producer: Some(Arc::new(ticks::default_tick_producer)),
            y_tick_producer: Some(Arc::new(ticks::default_tick_producer)),
            tick_label_size: 10.0,
            axis_label_size: 16.0,
            data_aspect: None,
            style: Arc::new(default_style),
            resolved_style: RwLock::new(PlotStyle::default()),
            x_ticks: Arc::new(Vec::new()),
            y_ticks: Arc::new(Vec::new()),
            picked_points: IndexMap::new(),
            hovered_points: IndexMap::new(),
            cursor_ui: None,
            shape_overlays_enabled: AtomicBool::new(false),
            camera_bounds: None,
            settled_view: None,
            next_publication: AtomicU64::new(1),
            accepted_publication: 0,
        }
    }

    fn view_source(&self) -> ViewSource {
        ViewSource { instance: self.instance_id, data: self.data_version,
            x_lim: self.x_lim, y_lim: self.y_lim,
            x_scale: self.x_axis_scale, y_scale: self.y_axis_scale }
    }

    /// Last settled camera ranges in plot coordinates, or None before first render.
    /// Log axes return logarithmic coordinates. This read does not request rendering.
    pub fn view_ranges(&self) -> Option<[[f64; 2]; 2]> {
        self.camera_bounds.map(|(camera, _)| {
            let (x, y) = camera.axis_ranges();
            [x, y]
        })
    }

    /// Current plot-area rectangle in logical layout coordinates, before scroll translation.
    pub fn view_bounds(&self) -> Option<Rectangle> { self.camera_bounds.map(|(_, bounds)| bounds) }

    /// Stable identifier of this plot's legend control container.
    pub fn legend_control_id(&self) -> String { format!("plot.legend.{}", self.instance_id) }

    /// Whether the retained legend is collapsed.
    pub fn legend_collapsed(&self) -> bool { self.legend_collapsed }

    /// Add a data series to the plot.
    /// If there exists a series with the same `item.id` ([ShapeId]), the old one will be replaced.
    pub fn add_series(&mut self, item: Series) -> Result<(), SeriesError> {
        item.validate()?;
        self.series.insert(item.id, item);
        self.data_version += 1;
        Ok(())
    }

    /// Set the data aspect ratio (y units per x unit). Use 1.0 for square pixels.
    pub fn set_data_aspect(&mut self, aspect: f64) {
        if aspect.is_finite() && aspect > 0.0 {
            self.data_aspect = Some(aspect);
        } else {
            self.data_aspect = None;
        }
        self.data_version = self.data_version.wrapping_add(1);
    }

    /// Set a style function for the plot widget.
    pub fn set_style<F>(&mut self, style: F)
    where
        F: Fn(&Theme) -> PlotStyle + Send + Sync + 'static,
    {
        self.style = Arc::new(style);
    }

    /// Remove a data series from the plot by its ID.
    pub fn remove_series(&mut self, id: &ShapeId) -> Result<(), SeriesError> {
        if self.series.shift_remove(id).is_some() {
            self.hidden_shapes.remove(id);
            self.data_version += 1;
            Ok(())
        } else {
            Err(SeriesError::NotFound(*id))
        }
    }

    /// Remove a fill from the plot by its ID.
    pub fn remove_fill(&mut self, id: &ShapeId) -> Result<(), SeriesError> {
        if self.fills.shift_remove(id).is_some() {
            self.hidden_shapes.remove(id);
            self.data_version += 1;
            Ok(())
        } else {
            Err(SeriesError::NotFound(*id))
        }
    }

    /// Update a data series by its id.
    pub fn update_series<F: FnMut(&mut Series)>(
        &mut self,
        id: &ShapeId,
        mut f: F,
    ) -> Result<(), SeriesError> {
        if let Some(series) = self.series.get_mut(id) {
            f(series);
            self.data_version += 1;
            Ok(())
        } else {
            Err(SeriesError::NotFound(*id))
        }
    }

    /// Add a vertical reference line to the plot.
    /// If there exists a line with the same `vline.id` ([ShapeId]), the old one will be replaced.
    pub fn add_vline(&mut self, vline: VLine) {
        if self.vlines.get(&vline.id) == Some(&vline) { return; }
        let geometry_changed = self.vlines.get(&vline.id).is_none_or(|old| !old.geometry_eq(&vline));
        self.vlines.insert(vline.id, vline);
        if !geometry_changed { return; }
        self.reference_version = self.reference_version.wrapping_add(1);
        self.data_version += 1;
    }

    /// Add a filled region between two shapes.
    pub fn add_fill(&mut self, fill: Fill) -> Result<(), SeriesError> {
        if fill.begin == fill.end {
            return Err(SeriesError::InvalidFillEndpoints);
        }
        if !self.is_fill_endpoint_available(fill.begin) {
            return Err(SeriesError::FillEndpointNotFound(fill.begin));
        }
        if !self.is_fill_endpoint_available(fill.end) {
            return Err(SeriesError::FillEndpointNotFound(fill.end));
        }
        self.fills.insert(fill.id, fill);
        self.data_version = self.data_version.wrapping_add(1);
        Ok(())
    }

    /// Add a horizontal reference line to the plot.
    /// If there exists a line with the same `hline.id` ([ShapeId]), the old one will be replaced.
    pub fn add_hline(&mut self, hline: HLine) {
        if self.hlines.get(&hline.id) == Some(&hline) { return; }
        let geometry_changed = self.hlines.get(&hline.id).is_none_or(|old| !old.geometry_eq(&hline));
        self.hlines.insert(hline.id, hline);
        if !geometry_changed { return; }
        self.reference_version = self.reference_version.wrapping_add(1);
        self.data_version += 1;
    }

    /// Set the x-axis label.
    pub fn set_x_axis_label(&mut self, label: impl Into<String>) {
        self.x_axis_label = label.into();
    }

    /// Set the y-axis label.
    pub fn set_y_axis_label(&mut self, label: impl Into<String>) {
        self.y_axis_label = label.into();
    }

    /// Set the x-axis limits (min, max) for the plot.
    ///
    /// If set, these will override autoscaling for the x-axis.
    pub fn set_x_lim(&mut self, min: f64, max: f64) {
        self.x_lim = Some((min, max));
    }

    /// Set the x-axis scale mode.
    ///
    /// This does not modify tick producer/formatter settings.
    pub fn set_x_axis_scale(&mut self, scale: AxisScale) {
        self.x_axis_scale = scale;
        self.data_version = self.data_version.wrapping_add(1);
    }

    /// Set the y-axis limits (min, max) for the plot.
    ///
    /// If set, these will override autoscaling for the y-axis.
    pub fn set_y_lim(&mut self, min: f64, max: f64) {
        self.y_lim = Some((min, max));
    }

    /// Clear the x-axis limits, allowing autoscaling to use data bounds.
    pub fn clear_x_lim(&mut self) {
        self.x_lim = None;
    }

    /// Follow mode: only scale Y-axis on each data update.
    /// X camera is preserved under user pan/zoom state.
    pub fn set_autoscale_y_on_updates(&mut self, enabled: bool) {
        self.autoscale_y_on_updates = enabled;
    }

    /// Follow mode: lock camera X to data right edge (data_max.x) on each update.
    /// User's zoom level (half_extents.x) is preserved — only position changes.
    pub fn set_follow_right_edge(&mut self, enabled: bool) {
        if enabled {
            self.follow_reset_counter = self.follow_reset_counter.wrapping_add(1);
        }
        self.follow_right_edge = enabled;
    }

    /// Set the y-axis scale mode.
    ///
    /// This does not modify tick producer/formatter settings.
    pub fn set_y_axis_scale(&mut self, scale: AxisScale) {
        self.y_axis_scale = scale;
        self.data_version = self.data_version.wrapping_add(1);
    }

    /// Link the x-axis to other plots. When the x-axis is panned or zoomed,
    /// all plots sharing this link will update synchronously.
    pub fn set_x_axis_link(&mut self, link: AxisLink) {
        self.x_axis_link = Some(link);
    }

    /// Link the y-axis to other plots. When the y-axis is panned or zoomed,
    /// all plots sharing this link will update synchronously.
    pub fn set_y_axis_link(&mut self, link: AxisLink) {
        self.y_axis_link = Some(link);
    }

    /// Update and return the current plot style for a given application theme.
    pub(crate) fn update_style(&self, theme: &Theme) -> PlotStyle {
        let style = (self.style)(theme);
        *self
            .resolved_style
            .write()
            .expect("plot style lock poisoned") = style;
        style
    }

    /// Convert world position to screen position using camera and bounds
    /// Similar to how PositionedTick calculates screen position
    fn world_to_screen_position(
        world: [f64; 2],
        camera_bounds: &(Camera, Rectangle),
        x_axis_scale: AxisScale,
        y_axis_scale: AxisScale,
        transform: &PositionTransform,
    ) -> Option<[f32; 2]> {
        let (camera, bounds) = camera_bounds;
        let world = data_point_to_plot_with_transform(
            world,
            x_axis_scale,
            y_axis_scale,
            transform,
            Some(camera.axis_ranges()),
        )?;
        if let (Some(screen_x), Some(screen_y)) = (
            world_to_screen_position_x(world[0], camera, bounds),
            world_to_screen_position_y(world[1], camera, bounds),
        ) {
            Some([screen_x, screen_y])
        } else {
            None
        }
    }

    fn world_to_screen_position_unclipped(
        world: [f64; 2],
        camera_bounds: &(Camera, Rectangle),
        x_axis_scale: AxisScale,
        y_axis_scale: AxisScale,
        transform: &PositionTransform,
    ) -> Option<[f32; 2]> {
        let (camera, bounds) = camera_bounds;
        let world = data_point_to_plot_with_transform(
            world,
            x_axis_scale,
            y_axis_scale,
            transform,
            Some(camera.axis_ranges()),
        )?;
        let ndc_x = (world[0] - camera.position.x) / camera.half_extents.x;
        let ndc_y = (world[1] - camera.position.y) / camera.half_extents.y;
        let screen_x = (ndc_x as f32 + 1.0) * 0.5 * bounds.width;
        let screen_y = (1.0 - ndc_y as f32) * 0.5 * bounds.height;

        (screen_x.is_finite() && screen_y.is_finite()).then_some([screen_x, screen_y])
    }

    /// Compute the world-space anchor point for a tooltip.
    ///
    /// For pixel-sized markers (or no markers), this is the point's (x, y).
    /// For world-sized square markers, the marker is effectively bottom-left anchored,
    /// so we anchor the tooltip at the marker's center (x + size/2, y + size/2).
    fn tooltip_anchor_world(point: &HighlightPoint) -> [f64; 2] {
        if let Some(marker_style) = point.marker_style
            && let Size::World(size) = marker_style.size
        {
            let half = size * 0.5;
            [point.x + half, point.y + half]
        } else {
            [point.x, point.y]
        }
    }

    /// Update tooltip positions for all hovered and picked points
    /// This should be called when the plot canvas position changes
    fn update_tooltip_positions(&mut self) {
        if let Some(camera_bounds) = &self.camera_bounds {
            for (highlight_point, tooltip) in self
                .hovered_points
                .values_mut()
                .chain(self.picked_points.values_mut())
            {
                if let Some(tooltip) = tooltip {
                    tooltip.screen_xy = Self::world_to_screen_position(
                        Self::tooltip_anchor_world(highlight_point),
                        camera_bounds,
                        self.x_axis_scale,
                        self.y_axis_scale,
                        &highlight_point.transform,
                    );
                }
            }
        }
    }

    /// Get an iterator over the series ids in the plot.
    pub fn series_ids(&self) -> Vec<ShapeId> {
        self.series.keys().copied().collect()
    }
    /// Get the position of a point in the plot.
    pub fn point_position(&self, point_id: PointId) -> Option<[f64; 2]> {
        self.series
            .get(&point_id.series_id)?
            .positions
            .get(point_id.point_index)
            .copied()
    }

    /// Find the nearest point to a given position in the plot.
    pub fn nearest_point(&self, series_id: ShapeId, x: f64, y: f64) -> Option<PointId> {
        let series = self.series.get(&series_id)?;
        let mut min_distance = f64::INFINITY;
        let mut nearest_point = None;
        for (i, position) in series.positions.iter().enumerate() {
            let distance = (position[0] - x).powi(2) + (position[1] - y).powi(2);
            if distance < min_distance {
                min_distance = distance;
                nearest_point = Some(PointId {
                    series_id,
                    point_index: i,
                });
            }
        }
        nearest_point
    }

    /// Find the nearest point to a given x-coordinate in the plot.
    pub fn nearest_point_horizontal(&self, series_id: ShapeId, x: f64) -> Option<PointId> {
        let series = self.series.get(&series_id)?;
        let mut min_distance = f64::INFINITY;
        let mut nearest_point = None;
        for (i, position) in series.positions.iter().enumerate() {
            let distance = (position[0] - x).abs();
            if distance < min_distance {
                min_distance = distance;
                nearest_point = Some(PointId {
                    series_id,
                    point_index: i,
                });
            }
        }
        nearest_point
    }

    /// Find the nearest point to a given y-coordinate in the plot.
    pub fn nearest_point_vertical(&self, series_id: ShapeId, y: f64) -> Option<PointId> {
        let series = self.series.get(&series_id)?;
        let mut min_distance = f64::INFINITY;
        let mut nearest_point = None;
        for (i, position) in series.positions.iter().enumerate() {
            let distance = (position[1] - y).abs();
            if distance < min_distance {
                min_distance = distance;
                nearest_point = Some(PointId {
                    series_id,
                    point_index: i,
                });
            }
        }
        nearest_point
    }

    /// Add a hover point to the plot.
    pub fn add_hover_point(&mut self, point_id: PointId) {
        self.handle_hover_pick::<false>(point_id);
    }

    /// Add a pick point to the plot.
    pub fn add_pick_point(&mut self, point_id: PointId) {
        self.handle_hover_pick::<true>(point_id);
    }

    /// Clear all hover points from the plot.
    pub fn clear_hover(&mut self) {
        if !self.hovered_points.is_empty() {
            self.hovered_points.clear();
        }
    }

    /// Clear all pick points from the plot.
    pub fn clear_pick(&mut self) {
        if !self.picked_points.is_empty() {
            self.picked_points.clear();
        }
    }

    fn cached_style(&self) -> PlotStyle {
        *self
            .resolved_style
            .read()
            .expect("plot style lock poisoned")
    }

    fn handle_hover_pick<const PICK: bool>(&mut self, point_id: PointId) -> bool {
        let mut changed = false;
        let (highlight_provider, points) = if PICK {
            // Clicking an already-picked point deselects it.
            if self.picked_points.shift_remove(&point_id).is_some() {
                return true;
            }
            changed |= self.hovered_points.shift_remove(&point_id).is_some();
            (&self.pick_highlight_provider, &mut self.picked_points)
        } else {
            if self.picked_points.contains_key(&point_id) {
                return false;
            }
            (&self.hover_highlight_provider, &mut self.hovered_points)
        };
        if let Some(highlight_provider) = highlight_provider
            && let Some(series) = self.series.get(&point_id.series_id)
            && let Some(position) = series.positions.get(point_id.point_index)
            && let Some(camera_bounds) = &self.camera_bounds
        {
            let mut highlight_point = HighlightPoint {
                x: position[0],
                y: position[1],
                transform: series.transform.clone(),
                color: series
                    .point_colors
                    .as_ref()
                    .map(|colors| colors[point_id.point_index])
                    .unwrap_or(series.color),
                marker_style: series.marker_style,
                mask_padding: Some(3.0),
            };
            let tooltip_text = highlight_provider(
                TooltipContext {
                    series_id: series.id,
                    series_label: series.label.as_deref().unwrap_or(""),
                    point_index: point_id.point_index,
                },
                &mut highlight_point,
            );
            let tooltip = tooltip_text.map(|text| TooltipUiPayload {
                screen_xy: Self::world_to_screen_position(
                    Self::tooltip_anchor_world(&highlight_point),
                    camera_bounds,
                    self.x_axis_scale,
                    self.y_axis_scale,
                    &highlight_point.transform,
                ),
                text,
            });
            let new_payload = (highlight_point, tooltip);
            match points.entry(point_id) {
                indexmap::map::Entry::Occupied(mut occupied) => {
                    if PartialEq::ne(occupied.get(), &new_payload) {
                        occupied.insert(new_payload);
                        changed = true;
                    }
                }
                indexmap::map::Entry::Vacant(vacant) => {
                    vacant.insert(new_payload);
                    changed = true;
                }
            }
        }
        changed
    }
    /// Handle a message sent to the plot widget.
    pub fn update(&mut self, msg: PlotUiMessage) {
        match msg {
            PlotUiMessage::ToggleLegend => {
                self.legend_collapsed = !self.legend_collapsed;
            }
            PlotUiMessage::ToggleControlsOverlay => {
                self.controls_overlay_open = !self.controls_overlay_open;
            }
            PlotUiMessage::ToggleSeriesVisibility(id) => {
                self.toggle_visibility(&id);
            }
            PlotUiMessage::RenderUpdate(payload) => {
                // Source validity governs the whole message; freshness only governs
                // replaceable view state, not explicit pick/clear-pick controls.
                if payload.view.source != self.view_source() {
                    return;
                }
                let fresh_view = payload.publication > self.accepted_publication;
                if fresh_view {
                    self.accepted_publication = payload.publication;
                    self.settled_view = Some(payload.view);
                    if self.camera_bounds != Some(payload.camera_bounds) {
                        self.camera_bounds = Some(payload.camera_bounds);
                        self.update_tooltip_positions();
                    }
                }

                match payload.hover_pick {
                    Some(HoverPickEvent::Hover(point_id)) if fresh_view => {
                        self.hovered_points.clear();
                        self.handle_hover_pick::<false>(point_id);
                    }
                    Some(HoverPickEvent::Pick(point_id)) => {
                        self.handle_hover_pick::<true>(point_id);
                    }
                    Some(HoverPickEvent::ClearHover) if fresh_view => {
                        self.hovered_points.clear();
                    }
                    Some(HoverPickEvent::ClearPick) => {
                        self.picked_points.clear();
                    }
                    _ => {}
                };
                if fresh_view {
                    if payload.clear_cursor_position {
                        self.cursor_ui = None;
                    }
                    if let Some(c) = payload.cursor_position_ui {
                        self.cursor_ui = Some(c);
                    }
                    self.x_ticks = payload.x_ticks;
                    self.y_ticks = payload.y_ticks;
                }
            }
        }
    }

    /// View the plot widget.
    pub fn view<'a>(&'a self) -> crate::Element<'a, PlotUiMessage> {
        self.shape_overlays_enabled.store(false, Ordering::Relaxed);
        self.view_with_shape_elements(std::iter::empty(), std::iter::empty(), |message| message)
    }

    /// View the plot widget with external Iced elements anchored to plot coordinates.
    pub fn view_with_shapes<'a, Message>(
        &'a self,
        shapes_bottom: impl Iterator<Item = plot_overlay::PlotOverlay<'a, Message>> + 'a,
        shapes_top: impl Iterator<Item = plot_overlay::PlotOverlay<'a, Message>> + 'a,
        map_plot: impl Fn(PlotUiMessage) -> Message + Copy + 'a,
    ) -> crate::Element<'a, Message>
    where
        Message: 'a,
    {
        self.shape_overlays_enabled.store(true, Ordering::Relaxed);

        let shapes_bottom = shapes_bottom.filter_map(|shape| self.view_shape_overlay(shape));
        let shapes_top = shapes_top.filter_map(|shape| self.view_shape_overlay(shape));
        self.view_with_shape_elements(shapes_bottom, shapes_top, map_plot)
    }

    fn view_with_shape_elements<'a, Message, MapPlot>(
        &'a self,
        shapes_bottom: impl Iterator<Item = Element<'a, Message>>,
        shapes_top: impl Iterator<Item = Element<'a, Message>>,
        map_plot: MapPlot,
    ) -> crate::Element<'a, Message>
    where
        Message: 'a,
        MapPlot: Fn(PlotUiMessage) -> Message + Copy + 'a,
    {
        let tooltip_overlays = self
            .visible_highlighted_points()
            .filter_map(|(_, tooltip)| self.view_tooltip_overlay(tooltip, &self.camera_bounds))
            .map(|element| element.map(map_plot));
        let shapes_top = shapes_top.chain(tooltip_overlays);

        let inner_container = self.view_plot_area(shapes_bottom, shapes_top, map_plot);

        let legend = if self.legend_enabled {
            legend::legend(self, self.legend_collapsed)
        } else {
            None
        };
        let has_legend = legend.is_some();

        let mut layers = Vec::new();
        layers.push(inner_container);
        layers.push(self.view_top_right_overlay(has_legend).map(map_plot));

        if let Some(legend) = legend {
            layers.push(container(legend.map(map_plot)).id(self.legend_control_id()).into());
        }

        let elements: Element<'a, Message> = stack(layers)
            .width(Length::Fill)
            .height(Length::Fill)
            .into();

        container(axes_labels::stack_with_labels(
            elements,
            self.view_tick_labels(true).map(|labels| labels.map(map_plot)).unwrap_or_else(|| container(widget::space()).height(Length::Fill).into()),
            self.view_tick_labels(false).map(|labels| labels.map(map_plot)).unwrap_or_else(|| container(widget::space()).width(Length::Fill).into()),
            self.tick_label_size,
            &self.x_axis_label,
            &self.y_axis_label,
            self.axis_label_size,
            &self.style,
        ))
        .padding(3.0)
        .style(|theme: &Theme| self.update_style(theme).frame)
        .into()
    }

    fn view_plot_area<'a, Message, MapPlot>(
        &'a self,
        shapes_bottom: impl Iterator<Item = Element<'a, Message>>,
        shapes_top: impl Iterator<Item = Element<'a, Message>>,
        map_plot: MapPlot,
    ) -> Element<'a, Message>
    where
        Message: 'a,
        MapPlot: Fn(PlotUiMessage) -> Message + Copy + 'a,
    {
        let plot: Element<'a, PlotUiMessage> = match self.render_strategy {
            PlotRenderStrategy::Shader => widget::shader(self)
                .width(Length::Fill)
                .height(Length::Fill)
                .into(),
            #[cfg(feature = "canvas")]
            PlotRenderStrategy::Canvas => widget::canvas(self)
                .width(Length::Fill)
                .height(Length::Fill)
                .into(),
        };
        let plot = plot.map(map_plot);

        let plot_layers = shapes_bottom.chain(std::iter::once(plot)).chain(shapes_top);

        container(stack(plot_layers).width(Length::Fill).height(Length::Fill))
            .clip(true)
            .padding(PLOT_CONTENT_PADDING)
            .style(|theme: &Theme| self.update_style(theme).plot_area)
            .into()
    }

    fn view_shape_overlay<'a, Message>(
        &'a self,
        shape: plot_overlay::PlotOverlay<'a, Message>,
    ) -> Option<Element<'a, Message>>
    where
        Message: 'a,
    {
        let camera_bounds = self.camera_bounds.as_ref()?;
        let mut screen_xy = Self::world_to_screen_position_unclipped(
            shape.anchor_position,
            camera_bounds,
            self.x_axis_scale,
            self.y_axis_scale,
            &shape.anchor_position_transform,
        )?;
        screen_xy[0] += shape.anchor_offset[0];
        screen_xy[1] -= shape.anchor_offset[1];

        Some(plot_overlay::positioned_overlay(
            shape.element,
            screen_xy,
            shape.align_to_anchor_horizontal,
            shape.align_to_anchor_vertical,
        ))
    }

    /// Enable or disable autoscaling on updates (default: enabled)
    pub fn autoscale_on_updates(&mut self, enabled: bool) {
        self.autoscale_on_updates = enabled;
    }

    /// Set hover radius in logical pixels for picking markers (default: 8 px)
    pub fn hover_radius_px(&mut self, radius: f32) {
        self.hover_radius_px = radius.max(0.0);
    }

    /// Set a custom highlighter for picked point.
    pub fn set_pick_highlight_provider(&mut self, provider: HighlightPointProvider) {
        self.pick_highlight_provider = Some(provider);
    }

    /// Set a custom highlighter for hovered point.
    pub fn set_hover_highlight_provider(&mut self, provider: HighlightPointProvider) {
        self.hover_highlight_provider = Some(provider);
    }

    /// Enable or disable the small cursor-position overlay shown in the
    /// lower-left corner of the plot. Disabled by default.
    pub fn set_cursor_overlay(&mut self, enabled: bool) {
        self.cursor_overlay = enabled;
    }

    /// Provide a custom formatter for the cursor overlay. Called with
    /// (x, y) world coordinates and should return the formatted string.
    pub fn set_cursor_provider(&mut self, provider: CursorProvider) {
        self.cursor_provider = Some(provider);
    }

    /// Enable or disable crosshairs that follow the cursor position.
    pub fn set_crosshairs(&mut self, enabled: bool) {
        self.crosshairs_enabled = enabled;
    }

    /// Set the rendering strategy used by the plot.
    pub fn set_render_strategy(&mut self, strategy: PlotRenderStrategy) {
        self.render_strategy = strategy;
        #[cfg(feature = "canvas")]
        {
            self.canvas_caches.static_layer.clear();
            self.canvas_caches.overlay_layer.clear();
        }
    }

    /// Set full interaction controls behavior for the plot.
    pub fn set_controls(&mut self, controls: PlotControls) {
        self.controls = controls;
    }

    /// Get the full interaction controls behavior for the plot.
    pub fn get_controls(&self) -> &PlotControls {
        &self.controls
    }

    /// Get the mutable full interaction controls behavior for the plot.
    pub fn get_controls_mut(&mut self) -> &mut PlotControls {
        &mut self.controls
    }

    /// Enable or disable point highlighting while hovering.
    pub fn set_highlight_on_hover(&mut self, enabled: bool) {
        self.highlight_on_hover = enabled;
    }

    /// Return whether point highlighting while hovering is enabled.
    pub fn highlight_on_hover(&self) -> bool {
        self.highlight_on_hover
    }

    /// Enable or disable the in-canvas controls/help UI (`?` button + panel).
    pub fn set_controls_help(&mut self, enabled: bool) {
        self.show_controls_help = enabled;
    }

    /// Return whether the in-canvas controls/help UI is enabled.
    pub fn controls_help_enabled(&self) -> bool {
        self.show_controls_help
    }

    /// Set a custom formatter for the x-axis tick labels.
    /// The formatter receives a GridMark (containing the tick value and step size)
    /// and the current visible range on the x-axis.
    pub fn set_x_axis_formatter(&mut self, formatter: TickFormatter) {
        self.x_axis_formatter = Some(formatter);
    }

    /// Set a custom formatter for the y-axis tick labels.
    /// The formatter receives a GridMark (containing the tick value and step size)
    /// and the current visible range on the y-axis.
    pub fn set_y_axis_formatter(&mut self, formatter: TickFormatter) {
        self.y_axis_formatter = Some(formatter);
    }

    /// Set a custom tick producer for generating tick positions along both axes.
    pub fn set_x_tick_producer(&mut self, producer: TickProducer) {
        self.x_tick_producer = Some(producer);
        self.data_version = self.data_version.wrapping_add(1);
    }

    /// Set a custom tick producer for generating tick positions along the y-axis.
    pub fn set_y_tick_producer(&mut self, producer: TickProducer) {
        self.y_tick_producer = Some(producer);
        self.data_version = self.data_version.wrapping_add(1);
    }

    /// Set the positions of an existing series.
    pub fn set_series_positions(&mut self, id: &ShapeId, positions: &[[f64; 2]]) {
        if let Some(series) = self.series.get_mut(id) {
            series.positions.clear();
            series.positions.extend_from_slice(positions);
            if let Some(colors) = &mut series.point_colors
                && colors.len() != series.positions.len()
            {
                colors.resize(series.positions.len(), series.color);
            }
            self.data_version += 1;
        }
    }

    /// Append new points to an existing series, dropping the oldest points so
    /// the series never exceeds `cap` points (`cap == 0` means unbounded).
    ///
    /// Intended for streaming telemetry: callers push only the freshly
    /// arrived samples instead of rebuilding and re-setting the full window
    /// on every update. Non-finite points (e.g. `[x, f64::NAN]`) are allowed
    /// and render as gaps in line series.
    pub fn append_series_points(&mut self, id: &ShapeId, points: &[[f64; 2]], cap: usize) {
        if points.is_empty() {
            return;
        }
        if let Some(series) = self.series.get_mut(id) {
            series.positions.extend_from_slice(points);
            // Per-point colors must stay aligned with positions: extend with the
            // series color for the new points, then drop from the FRONT together
            // with the positions (a plain resize would truncate from the back).
            if let Some(colors) = &mut series.point_colors {
                colors.resize(series.positions.len(), series.color);
            }
            let len = series.positions.len();
            if cap > 0 && len > cap {
                let excess = len - cap;
                series.positions.drain(..excess);
                if let Some(colors) = &mut series.point_colors {
                    colors.drain(..excess);
                }
            }
            self.data_version += 1;
        }
    }

    /// Set per-point colors for an existing series.
    pub fn set_series_point_colors(&mut self, id: &ShapeId, mut colors: Vec<Color>) {
        if let Some(series) = self.series.get_mut(id) {
            if colors.len() != series.positions.len() {
                colors.resize(series.positions.len(), series.color);
            }
            series.point_colors = Some(colors);
            self.data_version += 1;
        }
    }

    pub(crate) fn legend_entries(&self) -> Vec<LegendEntry> {
        let mut out = Vec::new();
        for (id, s) in &self.series {
            if let Some(ref label) = s.label {
                if label.is_empty() {
                    continue;
                }
                if s.positions.is_empty() {
                    continue;
                }
                // Include series that have either markers or lines
                if s.marker_style.is_some() || s.line_style.is_some() {
                    let marker = if let Some(ref marker_style) = s.marker_style {
                        marker_style.marker_type as u32
                    } else {
                        u32::MAX
                    };
                    out.push(LegendEntry {
                        id: *id,
                        label: label.clone(),
                        color: s.color,
                        _marker: marker,
                        _line_style: s.line_style,
                        hidden: self.hidden_shapes.contains(id),
                    });
                }
            }
        }
        // Add vertical reference lines to legend
        for (id, vline) in &self.vlines {
            if let Some(ref label) = vline.label
                && !label.is_empty()
            {
                out.push(LegendEntry {
                    id: *id,
                    label: label.clone(),
                    color: vline.color,
                    _marker: u32::MAX,
                    _line_style: Some(vline.line_style),
                    hidden: self.hidden_shapes.contains(id),
                });
            }
        }
        // Add horizontal reference lines to legend
        for (id, hline) in &self.hlines {
            if let Some(ref label) = hline.label
                && !label.is_empty()
            {
                out.push(LegendEntry {
                    id: *id,
                    label: label.clone(),
                    color: hline.color,
                    _marker: u32::MAX,
                    _line_style: Some(hline.line_style),
                    hidden: self.hidden_shapes.contains(id),
                });
            }
        }
        // Add fills to legend
        for (id, fill) in &self.fills {
            if let Some(ref label) = fill.label
                && !label.is_empty()
            {
                out.push(LegendEntry {
                    id: *id,
                    label: label.clone(),
                    color: fill.color,
                    _marker: u32::MAX,
                    _line_style: None,
                    hidden: self.hidden_shapes.contains(id),
                });
            }
        }
        out
    }

    fn view_tooltip_overlay<'a>(
        &'a self,
        payload: &'a Option<TooltipUiPayload>,
        camera_bounds: &Option<(Camera, Rectangle)>,
    ) -> Option<Element<'a, PlotUiMessage>> {
        // Offset a bit from point position
        const OFFSET: f32 = 8.0;
        let payload = payload.as_ref()?;
        let [screen_x, screen_y] = payload.screen_xy?;

        let mut anchor = [screen_x + OFFSET, screen_y + OFFSET];
        let mut horizontal_position = Horizontal::Right;
        let mut vertical_position = Vertical::Bottom;

        // flip the tooltip if the point is outside this percentage of the bounds
        const FLIP_PCT: f32 = 0.8;
        if let Some((_, bounds)) = &camera_bounds {
            if screen_y > bounds.height * FLIP_PCT {
                // Place the tooltip above the point.
                anchor[1] = screen_y - OFFSET;
                vertical_position = Vertical::Top;
            }
            if screen_x > bounds.width * FLIP_PCT {
                // Place the tooltip to the left of the point.
                anchor[0] = screen_x - OFFSET;
                horizontal_position = Horizontal::Left;
            }
        }

        let tooltip_bubble = container(
            widget::text(&payload.text)
                .size(14.0)
                .wrapping(widget::text::Wrapping::None),
        )
        .padding(6.0)
        .style(|theme| self.update_style(theme).tooltip);

        // Position tooltip at fixed location relative to point, not following cursor
        Some(plot_overlay::positioned_overlay(
            tooltip_bubble.into(),
            anchor,
            horizontal_position,
            vertical_position,
        ))
    }

    fn view_cursor_overlay(&self) -> Option<Element<'_, PlotUiMessage>> {
        if !self.cursor_overlay {
            return None;
        }

        let Some(payload) = &self.cursor_ui else {
            return None;
        };

        let bubble = container(widget::text(payload.text.clone()).size(12.0))
            .padding(6.0)
            .style(|theme| self.update_style(theme).cursor_overlay);

        Some(bubble.into())
    }

    fn view_top_right_overlay(&self, has_legend: bool) -> Element<'_, PlotUiMessage> {
        let help_btn = self.show_controls_help.then(|| {
            let help_label = if self.controls_overlay_open {
                "×"
            } else {
                "?"
            };

            widget::button(widget::text(help_label).size(12.0))
                .padding(6.0)
                .on_press(PlotUiMessage::ToggleControlsOverlay)
        });

        let top_row = widget::row![self.view_cursor_overlay(), help_btn].spacing(6.0);
        let col = widget::column![top_row, self.view_controls_overlay_panel(has_legend)]
            .spacing(6.0)
            .width(Length::Shrink)
            .height(Length::Shrink)
            .align_x(Horizontal::Right);

        container(col)
            .width(Length::Fill)
            .height(Length::Fill)
            .padding(Padding {
                top: 4.0,
                right: 4.0,
                ..Padding::ZERO
            })
            .align_x(Horizontal::Right)
            .align_y(Vertical::Top)
            .style(container::transparent)
            .into()
    }

    fn view_controls_overlay_panel(&self, has_legend: bool) -> Option<Element<'_, PlotUiMessage>> {
        if !self.controls_overlay_open {
            return None;
        }

        Some(
            container(self.controls.view_controls_overlay_panel(has_legend))
                .padding(8.0)
                .style(|theme| self.update_style(theme).controls_panel)
                .into(),
        )
    }

    fn view_tick_labels(&self, y_axis: bool) -> Option<Element<'_, PlotUiMessage>> {
        if self.x_ticks.is_empty() && self.y_ticks.is_empty() {
            return None;
        }

        let mut tick_elements = Vec::with_capacity(self.x_ticks.len() + self.y_ticks.len());
        let tick_text = |text| {
            widget::text(text)
                .size(self.tick_label_size)
                .style(|theme| widget::text::Style { color: Some((self.style)(theme).tick_label_color) })
        };

        if let Some(formatter) = &self.x_axis_formatter && !y_axis {
            for tick in self.x_ticks.iter() {
                let label_text = formatter(tick.tick);
                let centering_offset = 2.0 * (label_text.len() as f32); // A bit of a fudge.
                let text_widget = tick_text(label_text);
                let positioned_label = container(text_widget)
                    .width(Length::Fill)
                    .height(Length::Fill)
                    .padding(padding::left((tick.screen_pos - centering_offset).max(0.0).min(self.camera_bounds.map_or(f32::MAX, |(_, bounds)| (bounds.width - centering_offset * 2.0).max(0.0)))))
                    .align_x(Horizontal::Left)
                    .align_y(Vertical::Bottom)
                    .style(container::transparent);
                tick_elements.push(positioned_label.into());
            }
        }

        if let Some(formatter) = &self.y_axis_formatter && y_axis {
            for tick in self.y_ticks.iter() {
                let label_text = formatter(tick.tick);
                let text_widget = tick_text(label_text);
                let positioned_label = widget::container(text_widget)
                    .width(Length::Fill)
                    .height(Length::Fill)
                    .padding(padding::top((tick.screen_pos - self.tick_label_size * 0.5).max(0.0).min(self.camera_bounds.map_or(f32::MAX, |(_, bounds)| (bounds.height - self.tick_label_size).max(0.0)))))
                    .align_x(alignment::Horizontal::Right)
                    .align_y(Vertical::Top)
                    .style(container::transparent);
                tick_elements.push(positioned_label.into());
            }
        }

        if tick_elements.is_empty() {
            return None;
        }

        Some(stack(tick_elements).into())
    }

    pub(crate) fn visible_highlighted_points(
        &self,
    ) -> impl Iterator<Item = &(HighlightPoint, Option<TooltipUiPayload>)> {
        self.hovered_points
            .iter()
            .chain(self.picked_points.iter())
            .filter_map(|(point_id, point_ctx)| {
                (!self.hidden_shapes.contains(&point_id.series_id)).then_some(point_ctx)
            })
    }

    fn toggle_visibility(&mut self, id: &ShapeId) {
        let exists = self.series.contains_key(id)
            || self.fills.contains_key(id)
            || self.vlines.contains_key(id)
            || self.hlines.contains_key(id);

        if !exists {
            return;
        }
        if self.vlines.contains_key(id) || self.hlines.contains_key(id) {
            self.reference_version = self.reference_version.wrapping_add(1);
        }
        // toggle the visibility of the shape
        if !self.hidden_shapes.remove(id) {
            self.hidden_shapes.insert(*id);
        }
        self.data_version += 1;
    }

    fn is_fill_endpoint_available(&self, id: ShapeId) -> bool {
        self.series.contains_key(&id)
            || self.vlines.contains_key(&id)
            || self.hlines.contains_key(&id)
    }

    fn shape_uses_axes_transform(&self, id: ShapeId) -> bool {
        self.series
            .get(&id)
            .map(|series| &series.transform)
            .is_some_and(PositionTransform::uses_axes_coordinates)
            || self
                .vlines
                .get(&id)
                .and_then(|vline| vline.transform.as_ref())
                .is_some_and(|transform| transform.uses_axes_coordinates())
            || self
                .hlines
                .get(&id)
                .and_then(|hline| hline.transform.as_ref())
                .is_some_and(|transform| transform.uses_axes_coordinates())
    }

    fn has_visible_dynamic_geometry_transforms(&self) -> bool {
        self.series.iter().any(|(id, series)| {
            !self.hidden_shapes.contains(id) && series.transform.uses_axes_coordinates()
        }) || self.fills.iter().any(|(id, fill)| {
            !self.hidden_shapes.contains(id)
                && !self.hidden_shapes.contains(&fill.begin)
                && !self.hidden_shapes.contains(&fill.end)
                && (self.shape_uses_axes_transform(fill.begin)
                    || self.shape_uses_axes_transform(fill.end))
        })
    }
}

#[doc(hidden)]
pub struct Primitive {
    instance_id: u64,
    plot_widget: PlotState,
}

impl std::fmt::Debug for Primitive {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Primitive")
            .field("instance_id", &self.instance_id)
            .finish_non_exhaustive()
    }
}

#[derive(Default, Debug)]
struct UpdateEffects {
    needs_redraw: bool,
    hover_pick: Option<HoverPickEvent>,
    drag_event: Option<DragEvent>,
    cursor_ui: Option<CursorPositionUiPayload>,
    clear_cursor_position: bool,
    /// Request publishing `camera_bounds` even when ticks didn't change.
    /// This keeps overlays in sync when tick producers are disabled.
    publish_camera_bounds: bool,
}

#[derive(Default, Debug)]
struct CanvasInvalidation {
    static_layer: bool,
    overlay_layer: bool,
}

impl CanvasInvalidation {
    fn static_layer(&mut self) {
        self.static_layer = true;
    }

    fn overlay_layer(&mut self) {
        self.overlay_layer = true;
    }

    fn all(&mut self) {
        self.static_layer = true;
        self.overlay_layer = true;
    }

    fn apply(self, widget: &PlotWidget) {
        if self.static_layer {
            invalidate_static_canvas(widget);
        }
        if self.overlay_layer {
            invalidate_overlay_canvas(widget);
        }
    }
}

fn widget_has_any_tooltips(widget: &PlotWidget) -> bool {
    widget
        .hovered_points
        .values()
        .chain(widget.picked_points.values())
        .any(|(_, tooltip)| tooltip.is_some())
}

fn widget_needs_camera_bounds(widget: &PlotWidget) -> bool {
    widget_has_any_tooltips(widget) || widget.shape_overlays_enabled.load(Ordering::Relaxed)
}

fn clear_hover_effect(widget: &PlotWidget, state: &mut PlotState, effects: &mut UpdateEffects) {
    let should_clear_hover =
        state.picking.last_hover_cache.is_some() || !widget.hovered_points.is_empty();

    if effects.hover_pick.is_none() && should_clear_hover {
        state.picking.last_hover_cache = None;
        effects.hover_pick = Some(HoverPickEvent::ClearHover);
    }
}

fn maybe_submit_hover_request(
    widget: &PlotWidget,
    state: &mut PlotState,
    effects: &mut UpdateEffects,
) {
    if !state.hover_enabled || state.pan.active || state.selection.active {
        return;
    }
    if !state.cursor_inside() {
        clear_hover_effect(widget, state, effects);
        return;
    }

    if effects.hover_pick.is_some() {
        return;
    }

    let PlotState {
        picking: pick_state,
        cursor_position,
        hover_radius_px,
        points,
        series,
        camera,
        bounds,
        ..
    } = state;
    let force_cpu_picking = widget.render_strategy.force_cpu_picking();

    match pick_state.request_hover(
        widget.instance_id,
        *cursor_position,
        *hover_radius_px,
        force_cpu_picking,
        points.as_ref(),
        series.as_ref(),
        camera,
        bounds,
        |pid| widget.valid_point_id(pid),
    ) {
        picking::HoverRequest::CpuHit(point) => {
            effects.hover_pick = Some(HoverPickEvent::Hover(point));
        }
        picking::HoverRequest::CpuMiss => {
            clear_hover_effect(widget, state, effects);
        }
        picking::HoverRequest::RequestedGpu => {
            // Keep drawing until the result arrives.
            effects.needs_redraw = true;
        }
    }
}

fn update_cursor_overlay_on_move(
    widget: &PlotWidget,
    state: &PlotState,
    effects: &mut UpdateEffects,
) {
    if !widget.cursor_overlay {
        return;
    }
    if state.cursor_inside() {
        let viewport = Vec2::new(state.bounds.width, state.bounds.height);
        let plot = state.camera.screen_to_world(
            DVec2::new(
                state.cursor_position.x as f64,
                state.cursor_position.y as f64,
            ),
            DVec2::new(viewport.x as f64, viewport.y as f64),
        );
        let Some(world) =
            plot_point_to_data([plot.x, plot.y], widget.x_axis_scale, widget.y_axis_scale)
        else {
            effects.clear_cursor_position = true;
            return;
        };
        let text = if let Some(p) = &widget.cursor_provider {
            (p)(world[0], world[1])
        } else {
            format!("{:.4}, {:.4}", world[0], world[1])
        };

        effects.cursor_ui = Some(CursorPositionUiPayload {
            x: world[0],
            y: world[1],
            text,
        });
    } else {
        effects.clear_cursor_position = true;
    }
}

fn invalidate_static_canvas(widget: &PlotWidget) {
    _ = widget;
    #[cfg(feature = "canvas")]
    {
        widget.canvas_caches.static_layer.clear();
    }
}

fn invalidate_overlay_canvas(widget: &PlotWidget) {
    _ = widget;
    #[cfg(feature = "canvas")]
    {
        widget.canvas_caches.overlay_layer.clear();
    }
}

fn consume_gpu_pick_results(
    widget: &PlotWidget,
    state: &mut PlotState,
    effects: &mut UpdateEffects,
) {
    if !(state.hover_enabled || state.pick_enabled)
        || state.points.len() < picking::CPU_PICK_THRESHOLD
    {
        return;
    }

    if effects.hover_pick.is_some() {
        return;
    }

    match state
        .picking
        .consume_gpu_result(widget.instance_id, |pid| widget.valid_point_id(pid))
    {
        Some(picking::GpuResultEvent::Pick(point)) => {
            effects.hover_pick = Some(HoverPickEvent::Pick(point));
        }
        Some(picking::GpuResultEvent::Hover(point)) => {
            effects.hover_pick = Some(HoverPickEvent::Hover(point));
        }
        Some(picking::GpuResultEvent::HoverMiss) => {
            clear_hover_effect(widget, state, effects);
        }
        None => {}
    }
}

fn update_ticks_for_publication(
    widget: &PlotWidget,
    state: &mut PlotState,
    effects: &mut UpdateEffects,
    first_time_widget_view: bool,
) -> bool {
    if !effects.needs_redraw {
        return false;
    }

    let old_x = state.x_ticks.clone();
    let old_y = state.y_ticks.clone();
    state.update_ticks(
        widget.x_tick_producer.as_ref(),
        widget.y_tick_producer.as_ref(),
    );

    let ticks_changed = first_time_widget_view || state.x_ticks != old_x || state.y_ticks != old_y;

    // If tick producers are disabled, ticks might never change. Still publish camera/bounds
    // when overlays need them so screen-space positions stay in sync.
    if !ticks_changed
        && widget_needs_camera_bounds(widget)
        && widget.camera_bounds != Some((state.camera, state.bounds))
    {
        effects.publish_camera_bounds = true;
    }

    ticks_changed
}

fn update_plot_program<const IS_CANVAS: bool>(
    widget: &PlotWidget,
    state: &mut PlotState,
    event: &iced::Event,
    bounds: Rectangle,
    cursor: mouse::Cursor,
) -> Option<shader::Action<PlotUiMessage>> {
    let mut effects = UpdateEffects::default();
    let mut invalidation = CanvasInvalidation::default();
    let prev_camera = state.camera;
    let prev_bounds = state.bounds;

    // Keep these in sync early, since other phases depend on them.
    state.bounds = bounds;
    state.hover_enabled = widget.highlight_on_hover
        && (widget.hover_highlight_provider.is_some() || widget.pick_highlight_provider.is_some());
    state.pick_enabled = widget.controls.has_pick_action();
    state.hover_radius_px = widget.hover_radius_px;
    state.crosshairs_enabled = widget.crosshairs_enabled;

    if IS_CANVAS {
        let grid_style = widget.cached_style().grid;
        if state.grid_style != grid_style {
            state.grid_style = grid_style;
            effects.needs_redraw = true;
            invalidation.static_layer();
        }
    }

    // Sync highlight overlay data without rebuilding plot geometry.
    if state.sync_highlighted_points_from_widget(widget) {
        effects.needs_redraw = true;
        invalidation.overlay_layer();
    }

    let instance_switched = state.source_instance_id != Some(widget.instance_id);
    let first_time_widget_view = instance_switched && widget.settled_view.is_none();
    // Compare configuration to the camera's source, never to a fresh tree's defaults.
    let previous_source = if instance_switched {
        widget.settled_view.map(|view| view.source)
    } else {
        Some(ViewSource { instance: widget.instance_id, data: state.data_src_version,
            x_lim: state.x_lim, y_lim: state.y_lim,
            x_scale: state.x_axis_scale, y_scale: state.y_axis_scale })
    };
    let source = widget.view_source();
    let data_changed = previous_source.is_none_or(|previous| previous.data != source.data);
    let limits_changed = previous_source.is_some_and(|previous|
        previous.x_lim != source.x_lim || previous.y_lim != source.y_lim
        || previous.x_scale != source.x_scale || previous.y_scale != source.y_scale);

    if instance_switched {
        // Keep projection origin/counters and GPU settlement in their existing
        // owners. Only transient pointer state is discarded on chart replacement.
        state.reset_interaction();
        state.x_link_version = 0;
        state.y_link_version = 0;
        state.prev_data_max_x = None;
        state.prev_last_point_y = None;
        state.follow_reset_seen = widget.follow_reset_counter;
        if let Some((camera, _)) = widget.camera_bounds { state.camera = camera; }
        if let Some(view) = widget.settled_view {
            state.prev_data_max_x = view.prev_data_max_x;
            state.prev_last_point_y = view.prev_last_point_y;
            state.follow_reset_seen = view.follow_reset_seen;
        }
    }

    let projection_changed = widget.data_version != state.data_src_version || instance_switched;
    if projection_changed || limits_changed {
        if projection_changed {
            state.rebuild_from_widget(widget);
            if let Some(position) = state.available_cursor_local_position_inside(cursor) {
                state.cursor_position = position;
                maybe_submit_hover_request(widget, state, &mut effects);
            } else {
                clear_hover_effect(widget, state, &mut effects);
            }
        } else {
            // Limits affect the view, not ordinary series geometry or picking custody.
            state.x_lim = widget.x_lim;
            state.y_lim = widget.y_lim;
        }

        if first_time_widget_view || limits_changed || (data_changed && widget.autoscale_on_updates) {
            state.autoscale(!first_time_widget_view);
        } else if data_changed && widget.autoscale_y_on_updates {
            state.autoscale_y_only();
        }

        if data_changed && widget.follow_right_edge && !widget.autoscale_on_updates {
            if state.follow_reset_seen != widget.follow_reset_counter {
                state.prev_data_max_x = None;
                state.prev_last_point_y = None;
                state.follow_reset_seen = widget.follow_reset_counter;
            }
            if let Some(data_max) = state.data_max {
                if let Some(prev_x) = state.prev_data_max_x {
                    state.camera.position.x += data_max.x - prev_x;
                }
                state.prev_data_max_x = Some(data_max.x);
            }
            if let Some(last_y) = state.last_point_y {
                if let Some(prev_y) = state.prev_last_point_y {
                    state.camera.position.y += last_y - prev_y;
                }
                state.prev_last_point_y = Some(last_y);
            }
        }

        state.data_src_version = widget.data_version;
        state.source_instance_id = Some(widget.instance_id);
        // Even an unchanged numerical camera must publish the source it now represents.
        effects.publish_camera_bounds = true;
        effects.needs_redraw = true;
        invalidation.all();
    }

    // Check if axis links have been updated by other plots.
    if let Some(ref link) = state.x_axis_link {
        let link_version = link.version();
        if link_version != state.x_link_version {
            let (position, half_extent, version) = link.get();
            state.camera.position.x = position;
            state.camera.half_extents.x = half_extent;
            state.x_link_version = version;
            effects.needs_redraw = true;
            invalidation.all();
        }
    }
    if let Some(ref link) = state.y_axis_link {
        let link_version = link.version();
        if link_version != state.y_link_version {
            let (position, half_extent, version) = link.get();
            state.camera.position.y = position;
            state.camera.half_extents.y = half_extent;
            state.y_link_version = version;
            effects.needs_redraw = true;
            invalidation.all();
        }
    }

    match event {
        iced::Event::Mouse(mouse_event) => {
            let mouse_redraw = state.handle_mouse_event(
                *mouse_event,
                cursor,
                widget,
                &mut effects.hover_pick,
                &mut effects.drag_event,
            );
            effects.needs_redraw |= mouse_redraw;
            if mouse_redraw {
                invalidation.overlay_layer();
            }

            match mouse_event {
                iced::mouse::Event::CursorMoved { .. } => {
                    if state.available_cursor_is_inside(cursor) {
                        maybe_submit_hover_request(widget, state, &mut effects);
                        update_cursor_overlay_on_move(widget, state, &mut effects);
                    } else {
                        clear_hover_effect(widget, state, &mut effects);
                        if widget.cursor_overlay {
                            effects.clear_cursor_position = true;
                        }
                    }
                    invalidation.overlay_layer();
                }
                iced::mouse::Event::CursorLeft => {
                    clear_hover_effect(widget, state, &mut effects);
                    invalidation.overlay_layer();
                }
                _ => {}
            }
        }
        iced::Event::Keyboard(keyboard_event) => {
            if let keyboard::Event::KeyPressed { key, .. } = keyboard_event
                && state.available_cursor_is_inside(cursor)
                && widget.controls.key_action(key) == Some(KeyAction::ClearPick)
            {
                effects.hover_pick = Some(HoverPickEvent::ClearPick);
                invalidation.overlay_layer();
            }
            effects.needs_redraw |= state.handle_keyboard_event(keyboard_event, widget, cursor);
        }
        _ => {}
    }

    if let Some(aspect) = widget.data_aspect
        && apply_data_aspect(&mut state.camera, &state.bounds, aspect)
    {
        effects.needs_redraw = true;
        invalidation.all();
    }

    if (state.camera != prev_camera || state.bounds != prev_bounds)
        && widget.has_visible_dynamic_geometry_transforms()
    {
        state.rebuild_from_widget(widget);
        effects.needs_redraw = true;
        invalidation.all();
    }

    // Hover/pick highlight mask boxes are baked in clip space. Rebuild them through the
    // existing highlight_version path whenever camera or viewport bounds change.
    if !state.highlighted_points.is_empty()
        && (state.camera != prev_camera || state.bounds != prev_bounds)
    {
        state.highlight_version = state.highlight_version.wrapping_add(1);
        effects.needs_redraw = true;
        invalidation.overlay_layer();
    }

    if state.camera != prev_camera || state.bounds != prev_bounds {
        effects.needs_redraw = true;
        effects.publish_camera_bounds = true;
        invalidation.all();
    }

    let had_hover_pick = effects.hover_pick.is_some();
    // Process picking results after event handling (works for both mouse events and data updates).
    consume_gpu_pick_results(widget, state, &mut effects);
    if !had_hover_pick && effects.hover_pick.is_some() {
        invalidation.overlay_layer();
    }

    // Keep servicing both current logical requests and retained maps from an old projection.
    if !IS_CANVAS {
        effects.needs_redraw |= state.picking.has_outstanding_gpu_request();
    }

    let ticks_changed =
        update_ticks_for_publication(widget, state, &mut effects, first_time_widget_view);
    if ticks_changed {
        invalidation.static_layer();
    }

    let needs_publish = effects.hover_pick.is_some()
        || effects.drag_event.is_some()
        || effects.cursor_ui.is_some()
        || ticks_changed
        || effects.clear_cursor_position
        || effects.publish_camera_bounds;

    if IS_CANVAS {
        invalidation.apply(widget);
    }

    if needs_publish {
        Some(shader::Action::publish(PlotUiMessage::RenderUpdate(
            PlotRenderUpdate {
                publication: widget.next_publication.fetch_add(1, Ordering::Relaxed),
                view: SettledView { source,
                    prev_data_max_x: state.prev_data_max_x,
                    prev_last_point_y: state.prev_last_point_y,
                    follow_reset_seen: state.follow_reset_seen },
                hover_pick: effects.hover_pick,
                drag_event: effects.drag_event,
                clear_cursor_position: effects.clear_cursor_position,
                cursor_position_ui: effects.cursor_ui,
                x_ticks: Arc::clone(&state.x_ticks),
                y_ticks: Arc::clone(&state.y_ticks),
                // Publish one coherent view even for cursor-only messages, so
                // reordered reductions never settle metadata without its camera.
                camera_bounds: (state.camera, state.bounds),
            },
        )))
    } else {
        effects.needs_redraw.then(shader::Action::request_redraw)
    }
}

fn plot_mouse_interaction(state: &PlotState) -> Interaction {
    if state.pan.active {
        Interaction::Grabbing
    } else if state.selection.active {
        Interaction::Crosshair
    } else if state.picking.last_hover_cache.is_some() {
        Interaction::Pointer
    } else {
        Interaction::None
    }
}

impl shader::Program<PlotUiMessage> for PlotWidget {
    type State = PlotState;
    type Primitive = Primitive;

    fn draw(
        &self,
        state: &Self::State,
        _cursor: mouse::Cursor,
        _bounds: Rectangle,
    ) -> Self::Primitive {
        let mut plot_widget = state.clone();
        plot_widget.grid_style = self.cached_style().grid;

        Primitive {
            instance_id: self.instance_id,
            plot_widget,
        }
    }
    fn update(
        &self,
        state: &mut Self::State,
        event: &iced::Event,
        bounds: Rectangle,
        cursor: mouse::Cursor,
    ) -> Option<shader::Action<PlotUiMessage>> {
        update_plot_program::<false>(self, state, event, bounds, cursor)
    }

    fn mouse_interaction(
        &self,
        state: &Self::State,
        _bounds: Rectangle,
        _cursor: mouse::Cursor,
    ) -> Interaction {
        plot_mouse_interaction(state)
    }
}

#[cfg(feature = "canvas")]
impl iced::widget::canvas::Program<PlotUiMessage, Theme> for PlotWidget {
    type State = PlotState;

    fn update(
        &self,
        state: &mut Self::State,
        event: &iced::Event,
        bounds: Rectangle,
        cursor: mouse::Cursor,
    ) -> Option<iced::widget::canvas::Action<PlotUiMessage>> {
        update_plot_program::<true>(self, state, event, bounds, cursor)
    }

    fn draw(
        &self,
        state: &Self::State,
        renderer: &iced::Renderer,
        _theme: &Theme,
        bounds: Rectangle,
        _cursor: mouse::Cursor,
    ) -> Vec<iced::widget::canvas::Geometry> {
        crate::plot_renderer::canvas::draw(renderer, &self.canvas_caches, state, bounds)
    }

    fn mouse_interaction(
        &self,
        state: &Self::State,
        _bounds: Rectangle,
        _cursor: mouse::Cursor,
    ) -> Interaction {
        plot_mouse_interaction(state)
    }
}

#[doc(hidden)]
pub struct PlotRendererState {
    renderers: HashMap<u64, PlotRenderer>,
    format: TextureFormat,
}

impl shader::Primitive for Primitive {
    type Pipeline = PlotRendererState;

    fn prepare(
        &self,
        renderer_state: &mut Self::Pipeline,
        device: &iced::wgpu::Device,
        queue: &iced::wgpu::Queue,
        bounds: &Rectangle,
        viewport: &Viewport,
    ) {
        // Get or create renderer for this widget instance.
        let renderer = renderer_state
            .renderers
            .entry(self.instance_id)
            .or_insert_with(|| PlotRenderer::new(device, queue, renderer_state.format));
        renderer.prepare_frame(device, queue, viewport, bounds, &self.plot_widget);
        renderer.service_picking(self.instance_id, device, queue, &self.plot_widget);
    }

    /// Draws the plot inside iced's main surface render pass.
    ///
    /// iced sets the pass viewport/scissor to this primitive's bounds/clip
    /// before calling — the same state `PlotRenderer::encode` established on
    /// its own passes. Drawing in-pass avoids per-plot render-pass
    /// fragmentation of the surface, which corrupted frames (vanishing text)
    /// on tile-based mobile GPUs. GPU picking is unaffected: it runs from
    /// `prepare` with its own encoder/submission.
    ///
    /// Only valid when the pipelines are single-sampled: iced's surface pass
    /// has sample count 1, so with MSAA pipelines (desktop, 4x) this returns
    /// `false` and iced falls back to `render`, which draws into a dedicated
    /// MSAA offscreen target and composites the resolved texture.
    fn draw(
        &self,
        renderer_state: &Self::Pipeline,
        render_pass: &mut iced::wgpu::RenderPass<'_>,
        _resources: &mut shader::Resources,
    ) -> bool {
        if crate::plot_renderer::MSAA_SAMPLE_COUNT > 1 {
            return false;
        }
        if let Some(renderer) = renderer_state.renderers.get(&self.instance_id) {
            renderer.draw_in_pass(render_pass);
        }
        true
    }

    /// MSAA path (desktop): dedicated offscreen MSAA pass + resolve +
    /// composite. Android (MSAA=1) never reaches this — `draw` handles it
    /// in-pass.
    fn render(
        &self,
        renderer_state: &Self::Pipeline,
        encoder: &mut iced::wgpu::CommandEncoder,
        target: &iced::wgpu::TextureView,
        clip_bounds: &Rectangle<u32>,
        _resources: &mut shader::Resources,
    ) {
        if let Some(renderer) = renderer_state.renderers.get(&self.instance_id) {
            renderer.encode(RenderParams {
                encoder,
                target,
                clip_bounds,
            });
        }
    }
}

impl PlotWidget {
    /// Check if the point index is valid for the series
    fn valid_point_id(&self, point_id: &PointId) -> bool {
        self.series
            .get(&point_id.series_id)
            .map(|series| series.pickable && point_id.point_index < series.positions.len())
            .unwrap_or(false)
    }
}

fn apply_data_aspect(camera: &mut Camera, bounds: &Rectangle, aspect: f64) -> bool {
    let width = bounds.width.max(1.0) as f64;
    let height = bounds.height.max(1.0) as f64;
    let target_half_y = aspect * camera.half_extents.x * (height / width);
    if (camera.half_extents.y - target_half_y).abs() > f64::EPSILON {
        camera.half_extents.y = target_half_y;
        return true;
    }
    false
}

impl PlotWidget {
    pub(crate) fn pick_hit(&self, state: &mut PlotState) -> Option<PointId> {
        let PlotState {
            picking: pick_state,
            cursor_position,
            hover_radius_px,
            points,
            series,
            camera,
            bounds,
            ..
        } = state;
        let force_cpu_picking = self.render_strategy.force_cpu_picking();

        pick_state.request_pick_hit(
            self.instance_id,
            *cursor_position,
            *hover_radius_px,
            force_cpu_picking,
            points.as_ref(),
            series.as_ref(),
            camera,
            bounds,
            |pid| self.valid_point_id(pid),
        )
    }
}

impl Pipeline for PlotRendererState {
    fn new(
        _device: &iced::wgpu::Device,
        _queue: &iced::wgpu::Queue,
        format: iced::wgpu::TextureFormat,
    ) -> Self
    where
        Self: Sized,
    {
        PlotRendererState {
            renderers: HashMap::new(),
            format,
        }
    }
}

// Global unique ID generator for widget instances
static NEXT_ID: AtomicU64 = AtomicU64::new(1);

/// The highlight context for a point. You can modify
///
/// + `x` and `y` to change the position of the highlight point (not recommended);
/// + `color` to change the color of the highlight point;
/// + `transform` to change how the highlight x/y values are interpreted before drawing;
/// + `marker_style` to change the marker style of the highlight point;
/// + `mask_padding` to change the mask padding of the highlight point;
///
///  to change the highlight point.
#[derive(Debug, Clone, PartialEq)]
pub struct HighlightPoint {
    /// Raw x value for the highlight point.
    pub x: f64,
    /// Raw y value for the highlight point.
    pub y: f64,
    /// How to interpret or convert the x/y values before drawing.
    pub transform: PositionTransform,
    pub color: Color,
    /// Optional marker style for the series. If None, no markers are drawn.
    pub marker_style: Option<MarkerStyle>,
    /// Mask padding in pixels. If None, no mask is drawn.
    pub mask_padding: Option<f32>,
}

pub struct PositionDisplay<'a> {
    pos: f64,
    transform: &'a Option<Transform>,
}
impl fmt::Display for PositionDisplay<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if let Some(transform) = self.transform
            && transform.uses_axes_coordinates()
        {
            write!(f, "{:.1}%", self.pos * 100.0)
        } else {
            write!(f, "{:.2}", self.pos)
        }
    }
}

impl HighlightPoint {
    /// Resize the marker of the highlight point.
    /// For both pixel-based and world-based markers, the size will be multiplied by the factor.
    pub fn resize_marker(&mut self, factor: f64) {
        if let Some(marker_style) = &mut self.marker_style {
            match &mut marker_style.size {
                Size::Pixels(size) => {
                    *size *= factor as f32;
                }
                Size::World(size) => {
                    *size *= factor;
                }
            }
        }
    }
    pub fn display_x(&self) -> PositionDisplay<'_> {
        PositionDisplay {
            pos: self.x,
            transform: &self.transform.x,
        }
    }
    pub fn display_y(&self) -> PositionDisplay<'_> {
        PositionDisplay {
            pos: self.y,
            transform: &self.transform.y,
        }
    }
}

/// Convert world position to screen position
pub(crate) fn world_to_screen_position_x(
    x: f64,
    camera: &Camera,
    bounds: &Rectangle,
) -> Option<f32> {
    let ndc_x = (x - camera.position.x) / camera.half_extents.x;
    let screen_x = (ndc_x as f32 + 1.0) * 0.5 * bounds.width;

    if screen_x < 0.0 || screen_x > bounds.width {
        None
    } else {
        Some(screen_x)
    }
}

/// Convert world position to screen position
pub(crate) fn world_to_screen_position_y(
    y: f64,
    camera: &Camera,
    bounds: &Rectangle,
) -> Option<f32> {
    let ndc_y = (y - camera.position.y) / camera.half_extents.y;
    let screen_y = (1.0 - ndc_y as f32) * 0.5 * bounds.height;

    if screen_y < 0.0 || screen_y > bounds.height {
        None
    } else {
        Some(screen_y)
    }
}

#[cfg(test)]
mod retained_data_tests {
    use super::*;
    fn bounds() -> Rectangle { Rectangle::with_size(iced::Size::new(640.0, 360.0)) }
    fn redraw() -> iced::Event {
        iced::Event::Window(iced::window::Event::RedrawRequested(iced::time::Instant::now()))
    }
    fn publish(plot: &PlotWidget, state: &mut PlotState, event: &iced::Event,
        bounds: Rectangle, cursor: mouse::Cursor) -> Option<PlotUiMessage> {
        use iced::widget::shader::Program;
        Program::update(plot, state, event, bounds, cursor).and_then(|action| action.into_inner().0)
    }
    fn update(plot: &mut PlotWidget, state: &mut PlotState, event: &iced::Event,
        bounds: Rectangle, cursor: mouse::Cursor) {
        if let Some(message) = publish(plot, state, event, bounds, cursor) { plot.update(message); }
    }
    fn mount(plot: &mut PlotWidget, state: &mut PlotState, bounds: Rectangle) {
        update(plot, state, &redraw(), bounds, mouse::Cursor::Unavailable);
    }
    fn fixture() -> (PlotWidget, ShapeId) {
        let mut plot = crate::PlotWidgetBuilder::new().with_autoscale_on_updates(true).build().unwrap();
        let series = Series::line_only(vec![[1.0, 2.0], [3.0, 8.0]], crate::LineStyle::solid());
        let id = series.id;
        plot.add_series(series).unwrap();
        (plot, id)
    }
    fn pan(plot: &mut PlotWidget, state: &mut PlotState) {
        for (event, position) in [
            (mouse::Event::ButtonPressed(mouse::Button::Left), iced::Point::new(320.0, 180.0)),
            (mouse::Event::CursorMoved { position: iced::Point::new(380.0, 200.0) }, iced::Point::new(380.0, 200.0)),
            (mouse::Event::ButtonReleased(mouse::Button::Left), iced::Point::new(380.0, 200.0)),
        ] { update(plot, state, &iced::Event::Mouse(event), bounds(), mouse::Cursor::Available(position)); }
    }

    #[test]
    fn published_camera_survives_remount_and_equal_version_tree_reuse() {
        let (mut plot, id) = fixture();
        let mut state = PlotState::default();
        mount(&mut plot, &mut state, bounds());
        let initial = state.camera;
        assert_eq!(initial.position, DVec2::new(2.0, 5.0));
        pan(&mut plot, &mut state);
        let panned = state.camera;
        assert_ne!(panned, initial);
        plot.update(PlotUiMessage::ToggleLegend);
        let large = Rectangle::with_size(iced::Size::new(900.0, 500.0));
        let mut remount = PlotState::default();
        mount(&mut plot, &mut remount, large);
        assert_eq!(remount.camera, panned);
        assert!(plot.legend_collapsed());
        assert_eq!(remount.series[0].id, id);
        assert!(!state.origin().same_as(remount.origin()));
        let points = Arc::clone(&remount.points);
        mount(&mut plot, &mut remount, large);
        assert_eq!(remount.camera, panned);
        assert!(Arc::ptr_eq(&points, &remount.points));

        let (mut other, other_id) = fixture();
        assert_eq!(other.data_version, plot.data_version);
        // An unfinished gesture belongs to the old chart, not its replacement.
        update(&mut plot, &mut remount, &iced::Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
            large, mouse::Cursor::Available(iced::Point::new(400.0, 300.0)));
        assert!(remount.pan.active);
        mount(&mut other, &mut remount, large);
        assert!(!remount.pan.active && !remount.drag.active && !remount.press.active);
        assert_eq!(remount.camera, initial);
        assert_eq!(remount.series[0].id, other_id);
        mount(&mut plot, &mut remount, large);
        assert_eq!(remount.camera, panned);
        assert_eq!(remount.series[0].id, id);
    }

    #[test]
    fn hidden_updates_apply_autoscale_and_limits_against_retained_source() {
        let (mut plot, id) = fixture();
        let mut state = PlotState::default();
        mount(&mut plot, &mut state, bounds());
        pan(&mut plot, &mut state);
        plot.set_series_positions(&id, &[[10.0, 20.0], [30.0, 80.0]]);
        let mut remount = PlotState::default();
        mount(&mut plot, &mut remount, bounds());
        assert_eq!(remount.camera.position, DVec2::new(20.0, 50.0));
        plot.autoscale_on_updates(false);
        pan(&mut plot, &mut remount);
        plot.set_x_lim(100.0, 200.0);
        mount(&mut plot, &mut PlotState::default(), bounds());
        assert_eq!(plot.camera_bounds.unwrap().0.position.x, 150.0);
        // A non-default limit must not look newly changed on every mount.
        let mut limited = PlotState::default();
        mount(&mut plot, &mut limited, bounds());
        pan(&mut plot, &mut limited);
        let panned = limited.camera;
        let mut again = PlotState::default();
        mount(&mut plot, &mut again, bounds());
        assert_eq!(again.camera, panned);
        let points = Arc::clone(&again.points);
        let markers_version = again.markers_version;
        plot.clear_x_lim();
        mount(&mut plot, &mut again, bounds());
        assert_eq!(again.camera.position.x, 20.0);
        assert!(Arc::ptr_eq(&points, &again.points));
        assert_eq!(again.markers_version, markers_version);
        plot.set_y_axis_scale(AxisScale::Log { base: 10.0 });
        mount(&mut plot, &mut again, bounds());
        assert_eq!(again.camera.position.x, 20.0);
        assert!((again.camera.position.y - (20.0_f64.log10() + 80.0_f64.log10()) / 2.0).abs() < 1e-12);
    }

    #[test]
    fn retained_visibility_and_manual_camera_remain_independent_of_projection_lifetime() {
        let (mut plot, first) = fixture();
        let second = Series::line_only(vec![[20.0, 30.0], [40.0, 90.0]], crate::LineStyle::solid()).with_label("second");
        let second_id = second.id;
        plot.add_series(second).unwrap();
        let mut state = PlotState::default();
        mount(&mut plot, &mut state, bounds());
        plot.update(PlotUiMessage::ToggleSeriesVisibility(second_id));
        mount(&mut plot, &mut state, bounds());
        assert_eq!(state.camera.position, DVec2::new(2.0, 5.0));
        pan(&mut plot, &mut state);
        let camera = state.camera;
        plot.autoscale_on_updates(false);
        plot.set_series_positions(&first, &[[100.0, 200.0], [300.0, 800.0]]);
        let mut remount = PlotState::default();
        mount(&mut plot, &mut remount, bounds());
        assert_eq!(remount.camera, camera);
        assert_eq!(remount.series.len(), 1);
        assert_eq!(remount.series[0].id, first);
        assert!(plot.legend_entries().iter().any(|entry| entry.id == second_id && entry.hidden));
        plot.update(PlotUiMessage::ToggleSeriesVisibility(second_id));
        mount(&mut plot, &mut remount, bounds());
        assert_eq!(remount.series.len(), 2);
        assert_eq!(remount.camera, camera);
    }

    #[test]
    fn stale_publications_cannot_settle_new_data_or_overwrite_newer_gestures() {
        let (mut plot, id) = fixture();
        let mut state = PlotState::default();
        let old = publish(&plot, &mut state, &redraw(), bounds(), mouse::Cursor::Unavailable).unwrap();
        plot.set_series_positions(&id, &[[10.0, 20.0], [30.0, 80.0]]);
        plot.update(old);
        assert!(plot.settled_view.is_none());
        mount(&mut plot, &mut state, bounds());
        assert_eq!(state.camera.position, DVec2::new(20.0, 50.0));
        let delayed = publish(&plot, &mut state, &redraw(), Rectangle::with_size(iced::Size::new(800.0, 400.0)), mouse::Cursor::Unavailable).unwrap();
        let mut remount = PlotState::default();
        mount(&mut plot, &mut remount, bounds());
        pan(&mut plot, &mut remount);
        let camera = remount.camera;
        plot.update(delayed);
        assert_eq!(plot.camera_bounds.unwrap().0, camera);
        plot.set_y_lim(0.0, 100.0);
        let stale_limits = publish(&plot, &mut remount, &redraw(), bounds(), mouse::Cursor::Unavailable).unwrap();
        plot.set_y_lim(200.0, 400.0);
        plot.update(stale_limits);
        mount(&mut plot, &mut PlotState::default(), bounds());
        assert_eq!(plot.camera_bounds.unwrap().0.position.y, 300.0);
    }

    #[test]
    fn delayed_explicit_controls_preserve_fresh_view_and_require_matching_source() {
        for clear in [false, true] {
            for change_source in [false, true] {
                let (mut plot, id) = fixture();
                plot.set_series_positions(&id, &[[1.0, 2.0], [3.0, 8.0], [2.0, 5.0]]);
                plot.set_pick_highlight_provider(Arc::new(|_, _| Some("picked".into())));
                plot.set_hover_highlight_provider(Arc::new(|_, _| Some("hovered".into())));
                plot.set_highlight_on_hover(true);
                plot.set_cursor_overlay(true);
                plot.controls.bind_click(mouse::Button::Middle, crate::ClickAction::Pick);
                let mut state = PlotState::default();
                mount(&mut plot, &mut state, bounds());
                let position = |plot: &PlotWidget, index: usize| {
                    let series = &plot.series[&id];
                    let [x, y] = PlotWidget::world_to_screen_position(
                        series.positions[index], &plot.camera_bounds.unwrap(),
                        plot.x_axis_scale, plot.y_axis_scale, &series.transform,
                    ).unwrap();
                    iced::Point::new(x, y)
                };
                let click = |plot: &mut PlotWidget, state: &mut PlotState, point| {
                    let cursor = mouse::Cursor::Available(point);
                    update(plot, state, &iced::Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Middle)), bounds(), cursor);
                    publish(plot, state, &iced::Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Middle)), bounds(), cursor).unwrap()
                };
                let target = PointId { series_id: id, point_index: 0 };
                let point = position(&plot, 0);
                if clear {
                    let pick = click(&mut plot, &mut state, point);
                    assert!(matches!(pick.get_hover_pick_event(), Some(HoverPickEvent::Pick(found)) if found == target));
                    plot.update(pick);
                    assert!(plot.picked_points.contains_key(&target));
                    plot.controls.bind_click(mouse::Button::Middle, crate::ClickAction::ClearPick);
                }
                let old_hover = publish(&plot, &mut state,
                    &iced::Event::Mouse(mouse::Event::CursorMoved { position: point }),
                    bounds(), mouse::Cursor::Available(point)).unwrap();
                let delayed = click(&mut plot, &mut state, point);
                assert!(match delayed.get_hover_pick_event() {
                    Some(HoverPickEvent::ClearPick) => clear,
                    Some(HoverPickEvent::Pick(found)) => !clear && found == target,
                    _ => false,
                });
                let old_camera_bounds = plot.camera_bounds;
                if change_source {
                    plot.set_x_lim(-2.0, 6.0);
                    mount(&mut plot, &mut state, bounds());
                }
                pan(&mut plot, &mut state);
                let larger_bounds = Rectangle::with_size(iced::Size::new(800.0, 450.0));
                mount(&mut plot, &mut state, larger_bounds);
                let point = position(&plot, 2);
                update(&mut plot, &mut state,
                    &iced::Event::Mouse(mouse::Event::CursorMoved { position: point }),
                    larger_bounds, mouse::Cursor::Available(point));
                let camera_bounds = plot.camera_bounds;
                assert_ne!(camera_bounds.unwrap().0, old_camera_bounds.unwrap().0);
                assert_ne!(camera_bounds.unwrap().1, old_camera_bounds.unwrap().1);
                let ticks = (Arc::clone(&plot.x_ticks), Arc::clone(&plot.y_ticks));
                let cursor = plot.cursor_ui.as_ref().map(|value| (value.x, value.y, value.text.clone()));
                let hovered = plot.hovered_points.keys().copied().collect::<Vec<_>>();
                assert_eq!(hovered, [PointId { series_id: id, point_index: 2 }]);
                let publication = plot.accepted_publication;
                for message in [old_hover, delayed] {
                    plot.update(message);
                    assert_eq!(plot.camera_bounds, camera_bounds);
                    assert_eq!(plot.accepted_publication, publication);
                    assert!(Arc::ptr_eq(&plot.x_ticks, &ticks.0));
                    assert!(Arc::ptr_eq(&plot.y_ticks, &ticks.1));
                    assert_eq!(plot.cursor_ui.as_ref().map(|value| (value.x, value.y, value.text.clone())), cursor);
                    assert_eq!(plot.hovered_points.keys().copied().collect::<Vec<_>>(), hovered);
                }
                assert_eq!(plot.picked_points.contains_key(&target), if change_source { clear } else { !clear });
            }
        }
    }

    #[test]
    fn newer_cursor_publication_contains_its_complete_camera_before_first_reduction() {
        let (mut plot, _) = fixture();
        plot.set_cursor_overlay(true);
        let mut state = PlotState::default();
        let initial = publish(&plot, &mut state, &redraw(), bounds(), mouse::Cursor::Unavailable).unwrap();
        let point = iced::Point::new(320.0, 180.0);
        let cursor = publish(&plot, &mut state,
            &iced::Event::Mouse(mouse::Event::CursorMoved { position: point }),
            bounds(), mouse::Cursor::Available(point)).unwrap();
        plot.update(cursor);
        assert!(Arc::ptr_eq(&plot.x_ticks, &state.x_ticks));
        assert!(Arc::ptr_eq(&plot.y_ticks, &state.y_ticks));
        assert!(!plot.x_ticks.is_empty());
        let expected = state.camera;
        plot.update(initial);
        mount(&mut plot, &mut PlotState::default(), bounds());
        assert_eq!(plot.camera_bounds.unwrap().0, expected);
    }

    #[test]
    fn box_zoom_without_tick_or_cursor_overlays_is_retained() {
        let (mut plot, id) = fixture();
        plot.x_tick_producer = None;
        plot.y_tick_producer = None;
        let mut state = PlotState::default();
        mount(&mut plot, &mut state, bounds());
        let initial = state.camera;
        for (event, position) in [
            (mouse::Event::ButtonPressed(mouse::Button::Right), iced::Point::new(150.0, 90.0)),
            (mouse::Event::CursorMoved { position: iced::Point::new(450.0, 270.0) }, iced::Point::new(450.0, 270.0)),
            (mouse::Event::ButtonReleased(mouse::Button::Right), iced::Point::new(450.0, 270.0)),
        ] {
            update(&mut plot, &mut state, &iced::Event::Mouse(event), bounds(), mouse::Cursor::Available(position));
        }
        let zoomed = state.camera;
        assert!(zoomed.half_extents.x < initial.half_extents.x);
        assert!(zoomed.half_extents.y < initial.half_extents.y);
        let mut remount = PlotState::default();
        mount(&mut plot, &mut remount, Rectangle::with_size(iced::Size::new(900.0, 500.0)));
        assert_eq!(remount.camera, zoomed);
        assert_eq!(remount.series[0].id, id);
    }

    #[test]
    fn remounted_follow_modes_and_explicit_autoscale_retain_their_policy() {
        let (mut plot, id) = fixture();
        plot.autoscale_on_updates(false);
        plot.set_follow_right_edge(true);
        let mut state = PlotState::default();
        mount(&mut plot, &mut state, bounds());
        pan(&mut plot, &mut state);
        let camera = state.camera;
        plot.set_series_positions(&id, &[[2.0, 4.0], [4.0, 10.0]]);
        let mut remount = PlotState::default();
        mount(&mut plot, &mut remount, bounds());
        assert_eq!(remount.camera.position, camera.position + DVec2::new(1.0, 2.0));
        assert_eq!(remount.camera.half_extents, camera.half_extents);
        plot.set_follow_right_edge(false);
        plot.set_autoscale_y_on_updates(true);
        let x = remount.camera.position.x;
        plot.set_series_positions(&id, &[[3.0, 10.0], [5.0, 30.0]]);
        mount(&mut plot, &mut PlotState::default(), bounds());
        assert_eq!(plot.camera_bounds.unwrap().0.position, DVec2::new(x, 20.0));
        plot.get_controls_mut().bind_key(keyboard::Key::Named(keyboard::key::Named::Home), KeyAction::Autoscale);
        mount(&mut plot, &mut remount, bounds());
        let event = iced::Event::Keyboard(keyboard::Event::KeyPressed {
            key: keyboard::Key::Named(keyboard::key::Named::Home),
            modified_key: keyboard::Key::Named(keyboard::key::Named::Home),
            physical_key: keyboard::key::Physical::Code(keyboard::key::Code::Home),
            location: keyboard::Location::Standard, modifiers: keyboard::Modifiers::NONE,
            text: None, repeat: false,
        });
        update(&mut plot, &mut remount, &event, bounds(), mouse::Cursor::Available(iced::Point::new(300.0, 150.0)));
        assert_eq!(remount.camera.position, DVec2::new(4.0, 20.0));
        let link = AxisLink::new();
        plot.set_x_axis_link(link.clone());
        mount(&mut plot, &mut PlotState::default(), bounds());
        link.set(77.0, 5.0);
        mount(&mut plot, &mut PlotState::default(), bounds());
        assert_eq!(plot.camera_bounds.unwrap().0.position.x, 77.0);
    }

    #[test]
    fn shader_draw_shares_tree_projection_origin_but_remount_does_not() {
        use iced::widget::shader::Program;

        let mut plot = crate::PlotWidgetBuilder::new().build().unwrap();
        plot.add_series(Series::line_only(
            vec![[0.0, 0.0], [1.0, 0.25], [2.0, 1.0]],
            crate::LineStyle::solid(),
        )).unwrap();
        let bounds = Rectangle::with_size(iced::Size::new(640.0, 360.0));
        let event = iced::Event::Window(iced::window::Event::RedrawRequested(iced::time::Instant::now()));
        let mut state = PlotState::default();
        let _ = Program::update(&plot, &mut state, &event, bounds, mouse::Cursor::Unavailable);
        let first = Program::draw(&plot, &state, mouse::Cursor::Unavailable, bounds);
        let _ = Program::update(&plot, &mut state, &event, bounds, mouse::Cursor::Unavailable);
        let redraw = Program::draw(&plot, &state, mouse::Cursor::Unavailable, bounds);
        assert!(first.plot_widget.origin().same_as(redraw.plot_widget.origin()));
        assert!(Arc::ptr_eq(&first.plot_widget.points, &redraw.plot_widget.points));
        assert_eq!(first.plot_widget.lines_version, redraw.plot_widget.lines_version);
        let mut remounted = PlotState::default();
        let _ = Program::update(&plot, &mut remounted, &event, bounds, mouse::Cursor::Unavailable);
        let replacement = Program::draw(&plot, &remounted, mouse::Cursor::Unavailable, bounds);
        assert_eq!(first.instance_id, replacement.instance_id);
        assert_eq!(first.plot_widget.lines_version, replacement.plot_widget.lines_version);
        assert!(!first.plot_widget.origin().same_as(replacement.plot_widget.origin()));
    }

    #[test]
    fn replacement_points_reuse_storage_and_the_plot_instance() {
        let mut plot = crate::PlotWidgetBuilder::new().build().unwrap();
        let series = Series::line_only(vec![[0.0, 1.0]; 128], crate::LineStyle::solid());
        let id = series.id;
        plot.add_series(series).unwrap();
        let instance = plot.instance_id;
        let allocation = plot.series[&id].positions.as_ptr();
        plot.set_series_positions(&id, &[[1.0, 2.0], [2.0, 3.0]]);
        assert_eq!(plot.series[&id].positions.as_ptr(), allocation);
        plot.set_series_positions(&id, &[]);
        assert_eq!(plot.series[&id].positions.capacity(), 128);
        assert_eq!(plot.instance_id, instance);
    }
    #[test]
    fn reference_projection_survives_series_updates_and_tracks_real_mutations() {
        let mut plot = crate::PlotWidgetBuilder::new().build().unwrap();
        let series = Series::line_only(vec![[0.0, 1.0]], crate::LineStyle::solid());
        let series_id = series.id;
        plot.add_series(series).unwrap();
        let mut vertical = VLine::new(2.0);
        let horizontal = HLine::new(3.0);
        plot.add_vline(vertical.clone()); plot.add_hline(horizontal.clone());
        let mut state = PlotState::default(); state.rebuild_from_widget(&plot);
        let original = state.clone();
        plot.set_series_positions(&series_id, &[[1.0, 4.0]]);
        state.rebuild_from_widget(&plot);
        assert_eq!(state.reference_version, original.reference_version);
        assert!(Arc::ptr_eq(&state.vlines, &original.vlines));
        assert!(Arc::ptr_eq(&state.hlines, &original.hlines));
        plot.add_vline(vertical.clone()); state.rebuild_from_widget(&plot);
        assert_eq!(state.reference_version, original.reference_version);
        vertical.x = 5.0; plot.add_vline(vertical.clone()); state.rebuild_from_widget(&plot);
        assert_eq!(state.reference_version, original.reference_version + 1);
        assert_eq!(state.vlines[0].x, 5.0);
        plot.toggle_visibility(&series_id); state.rebuild_from_widget(&plot);
        assert_eq!(state.reference_version, original.reference_version + 1);
        plot.toggle_visibility(&vertical.id); state.rebuild_from_widget(&plot);
        assert!(state.vlines.is_empty());
        assert_eq!(state.reference_version, original.reference_version + 2);
        plot.toggle_visibility(&horizontal.id); state.rebuild_from_widget(&plot);
        assert!(state.hlines.is_empty());
        let hidden = state.clone(); state.rebuild_from_widget(&plot);
        assert_eq!(state.reference_version, hidden.reference_version);
        assert!(Arc::ptr_eq(&state.hlines, &hidden.hlines));
        plot.toggle_visibility(&vertical.id); state.rebuild_from_widget(&plot);
        assert_eq!(state.vlines.len(), 1);
        let version = state.reference_version;
        vertical = vertical.with_axes_transform().with_width(4.0).with_color(Color::BLACK);
        plot.add_vline(vertical.clone()); state.rebuild_from_widget(&plot);
        assert_eq!(state.reference_version, version + 1);
        assert_eq!(state.vlines[0], vertical);
        let mut horizontal = horizontal; horizontal.y = 7.0;
        plot.add_hline(horizontal.clone()); plot.toggle_visibility(&horizontal.id);
        state.rebuild_from_widget(&plot);
        assert_eq!(state.hlines[0], horizontal);
        plot.toggle_visibility(&series_id);
        plot.set_series_positions(&series_id, &[]); state.rebuild_from_widget(&plot);
        assert!(state.points.is_empty()); assert!(state.series.is_empty());
    }

    #[test]
    fn reference_labels_update_legend_without_geometry_or_fill_invalidation() {
        let mut plot = crate::PlotWidgetBuilder::new().build().unwrap();
        plot.set_x_lim(-1.0, 1.0);
        let mut vertical = VLine::new(0.0).with_label("old vertical");
        let mut horizontal = HLine::new(0.0).with_label("old horizontal");
        let upper = HLine::new(0.5);
        plot.add_vline(vertical.clone()); plot.add_hline(horizontal.clone()); plot.add_hline(upper.clone());
        plot.add_fill(crate::Fill::new(horizontal.id, upper.id)).unwrap();
        let mut state = PlotState::default(); state.rebuild_from_widget(&plot);
        let before = state.clone(); let data_version = plot.data_version;
        let reference_version = plot.reference_version;
        vertical.label = Some("new vertical".into()); horizontal.label = Some("new horizontal".into());
        plot.add_vline(vertical.clone()); plot.add_hline(horizontal.clone());
        let legend = plot.legend_entries();
        assert!(legend.iter().any(|entry| entry.id == vertical.id && entry.label == "new vertical"));
        assert!(legend.iter().any(|entry| entry.id == horizontal.id && entry.label == "new horizontal"));
        assert_eq!(plot.data_version, data_version);
        assert_eq!(plot.reference_version, reference_version);
        // Even a later unrelated data projection retains reference geometry.
        state.rebuild_from_widget(&plot);
        assert_eq!(state.reference_version, before.reference_version);
        assert!(Arc::ptr_eq(&state.vlines, &before.vlines));
        assert!(Arc::ptr_eq(&state.hlines, &before.hlines));
        assert!(!state.fills.is_empty());
        let fill_vertices = state.fills[0].vertices.clone();
        horizontal.y = 0.25; plot.add_hline(horizontal.clone());
        assert_ne!(plot.data_version, data_version);
        state.rebuild_from_widget(&plot);
        assert_ne!(state.reference_version, before.reference_version);
        assert_ne!(state.fills[0].vertices.as_ref(), fill_vertices.as_ref());
        plot.toggle_visibility(&horizontal.id); state.rebuild_from_widget(&plot);
        assert!(state.fills.is_empty());
    }

    #[test]
    fn unchanged_tick_and_primitive_facts_share_allocations() {
        let plot = crate::PlotWidgetBuilder::new().build().unwrap();
        let mut state = PlotState::default();
        state.bounds = Rectangle { x: 0.0, y: 0.0, width: 640.0, height: 360.0 };
        state.camera = Camera::new(640, 360);
        state.update_ticks(plot.x_tick_producer.as_ref(), plot.y_tick_producer.as_ref());
        let first = state.clone();
        state.update_ticks(plot.x_tick_producer.as_ref(), plot.y_tick_producer.as_ref());
        assert!(Arc::ptr_eq(&first.x_ticks, &state.x_ticks));
        assert!(Arc::ptr_eq(&first.y_ticks, &state.y_ticks));
        assert!(Arc::ptr_eq(&first.points, &state.points));
        assert!(!state.sync_highlighted_points_from_widget(&plot));
        state.bounds.width = 800.0;
        state.update_ticks(plot.x_tick_producer.as_ref(), plot.y_tick_producer.as_ref());
        assert!(!Arc::ptr_eq(&first.x_ticks, &state.x_ticks));
    }
}
