#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include "audit_facts.h"
#include "surface_audit.h"
namespace mmltk::acceptance::wayland {
struct AtlasDrawAudit final {
 using SourceKey = std::array<std::uint64_t, 4U>;
 using SampleKey = std::pair<std::string, std::uint64_t>;
 using AllocationKey = std::tuple<std::string, std::uint64_t, std::uint64_t>;
 static constexpr std::initializer_list<const char*> source_fields{"content_session", "source_kind", "source_instance", "source_revision", "content_width", "content_height", "dataset_identity",
  "columns", "rows", "first_row", "matching_count", "visible_indices", "row_capacity", "row_origin", "card_extent"};
 static constexpr std::initializer_list<const char*> image_fields{"surface", "width", "height", "presentation_revision", "frame_revision", "content_session", "source_kind", "source_instance",
  "source_revision", "content_width", "content_height", "columns", "rows", "first_row", "matching_count", "visible_indices", "row_capacity", "row_origin", "card_extent"};
 static constexpr std::array stage_names{"fractional", "row1", "row2", "row10", "row9", "end", "restored"};
 static constexpr std::array held_names{"held-visible", "held-return", "held-aligned", "held-extra", "held-restored", "held-complete"};
 static constexpr std::array return_names{"return-cached", "return-aligned", "return-extra", "return-restored"};
 std::map<SourceKey, nlohmann::json> sources;
 std::map<std::pair<std::uint64_t, std::uint64_t>, SourceKey> sessions;
 std::map<SampleKey, nlohmann::json> acquisitions;
 std::set<std::uint64_t> drawn_rows;
 std::optional<AllocationKey> staged_allocation;
 std::uint64_t previous_staged_rows = 0U;
 unsigned staged_transitions = 0U;
 std::optional<nlohmann::json> last_draw;
 std::optional<nlohmann::json> last_draw_reset;
 std::set<std::string> stages;
 bool grid_round_trip = false;
 std::optional<nlohmann::json> overlap_draw;
 std::optional<nlohmann::json> return_baseline;
 unsigned return_stage = 0U;
 bool return_round_trip = false;
 std::vector<nlohmann::json> resize_stages;
 bool away_return = false;
 std::map<SampleKey, std::set<std::uint64_t>> ready_cell_samples;
 struct RetainedReadiness final {
  bool ready = false;
  SampleKey sample;
 };
 struct RetainedPixel final {
  std::uint64_t rgba = 0U;
  SampleKey sample;
 };
 std::map<std::uint64_t, RetainedReadiness> retained_ready;
 std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, RetainedPixel> retained_cell_pixels;
 std::optional<nlohmann::json> pixel_meaning;
 std::map<std::string, nlohmann::json> held_stages;
 bool seen = false;
 bool empty_seen = false;
 bool valid = true;
 FirstAuditFailure failure;
 [[nodiscard]] const nlohmann::json* acquisition_for(const SampleKey& key) const;
 [[nodiscard]] bool empty_draw_submitted(const SurfaceAudit& physical) const;
 [[nodiscard]] bool check(
  const bool passed, const std::string_view reason, const nlohmann::json& record, const nlohmann::json* prior = nullptr, const std::string_view field = {}, const nlohmann::json* expected = nullptr);
 [[nodiscard]] bool check_fields(const nlohmann::json& record, const nlohmann::json& prior, const std::initializer_list<const char*> fields, const std::string_view reason);
 [[nodiscard]] static SourceKey source_key(const nlohmann::json& record);
 [[nodiscard]] static SampleKey sample_key(const nlohmann::json& record);
 [[nodiscard]] static AllocationKey allocation_key(const nlohmann::json& record);
 void observe_staged_rows(const AllocationKey& allocation, const std::uint64_t rows);
 [[nodiscard]] static const char* differing_field(const nlohmann::json& left, const nlohmann::json& right, const std::initializer_list<const char*> fields);
 [[nodiscard]] static bool same_fields(const nlohmann::json& left, const nlohmann::json& right, const std::initializer_list<const char*> fields);
 [[nodiscard]] bool ready_pixels_complete(const nlohmann::json& record);
 [[nodiscard]] bool source_stage_matches_draw(const nlohmann::json& record);
 void clear_last_draw(const nlohmann::json& record);
 void stage(const nlohmann::json& record);
 [[nodiscard]] static bool visible(const nlohmann::json& record);
 void consume(const nlohmann::json& record);
};
struct BrowserAudit final {
 AtlasDrawAudit atlas_draws;
 bool owned_atlas_seen = false;
 bool owned_atlas_current = false;
 bool owned_atlas_interrupted = false;
 FirstAuditFailure owned_atlas_failure;
 std::optional<nlohmann::json> prior_scenario_atlas_draw;
 std::map<std::string, std::size_t, std::less<>> renderer_reconstructions;
 struct Bounds final {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
  [[nodiscard]] bool valid() const noexcept { return width > 0.0 && height > 0.0; }
  [[nodiscard]] bool contains_horizontally(const Bounds& inner, const double tolerance = 1.0) const noexcept { return inner.x >= x - tolerance && inner.x + inner.width <= x + width + tolerance; }
  [[nodiscard]] bool contains(const Bounds& inner, const double tolerance = 1.0) const noexcept {
   return contains_horizontally(inner, tolerance) && inner.y >= y - tolerance && inner.y + inner.height <= y + height + tolerance;
  }
 };
 struct SurfaceGeometry final {
  std::uint64_t presentation_revision = 0U;
  std::uint64_t source_revision = 0U;
  double width = 0.0;
  double height = 0.0;
 };
 struct SurfaceContainer final {
  std::uint64_t presentation_revision = 0U;
  std::uint64_t source_revision = 0U;
  double width = 0.0;
  double height = 0.0;
  std::string control;
 };
 std::set<std::string, std::less<>> controls;
 std::map<std::string, Bounds, std::less<>> page_bounds;
 Bounds train_setup;
 Bounds train_center;
 Bounds train_workspace;
 Bounds train_advanced;
 Bounds train_diagnostics;
 Bounds explore_dataset;
 Bounds explore_gallery;
 Bounds explore_details;
 Bounds compile_action;
 Bounds compile_progress;
 Bounds model_card;
 Bounds model_progress;
 std::array<Bounds, 6> model_parts;
 Bounds settings_modal;
 Bounds settings_footer;
 Bounds settings_reset;
 Bounds settings_close;
 Bounds settings_appearance;
 Bounds settings_typography;
 Bounds settings_environment;
 Bounds settings_show_fps;
 Bounds error_modal;
 Bounds error_copy;
 Bounds error_dismiss;
 Bounds benchmark_override;
 bool perceptual_controls_round_trip = false;
 Bounds advanced_container;
 std::array<Bounds, 8> advanced_fixed;
 Bounds advanced_assignment;
 std::array<Bounds, 3> advanced_match_free;
 Bounds advanced_denoising_toggle;
 std::array<Bounds, 4> advanced_denoising;
 Bounds annotation_sidebar;
 Bounds annotation_timeline;
 Bounds annotation_operation;
 Bounds annotation_stop;
 Bounds annotation_brush;
 Bounds annotation_tool_control;
 std::array<Bounds, 5> settings_numeric_controls;
 std::array<Bounds, 5> settings_numeric_labels;
 std::array<Bounds, 5> settings_numeric_values;
 bool bounds_valid = true;
 bool fluent = false;
 std::set<std::string, std::less<>> primary_idle_labels;
 std::map<std::string, double, std::less<>> primary_phases;
 std::set<std::string, std::less<>> primary_phase_progress;
 std::set<bool> primary_active_themes;
 std::set<std::string, std::less<>> shared_primary;
 std::map<std::string, std::array<double, 4>, std::less<>> shared_primary_colors;
 std::map<std::string, std::uint64_t, std::less<>> rendered_style_keys;
 std::set<std::string, std::less<>> rendered_controls;
 std::map<std::string, std::uint64_t, std::less<>> rendered_control_keys;
 std::map<std::string, double, std::less<>> rendered_control_scales;
 std::map<std::uint64_t, std::map<std::uint64_t, std::uint64_t>> explore_slots;
 struct GalleryGenerationEvidence final {
  std::map<std::uint64_t, std::uint64_t> slots;
  std::uint64_t digest = 0U;
  std::uint64_t source_revision = 0U;
  std::uint64_t snapshot_revision = 0U;
  std::vector<bool> ready_slots;
 };
 std::map<std::uint64_t, GalleryGenerationEvidence> gallery_generations;
 // CLEANUP-IGNORE: Rendered slot-frame identity precedes cursor facts; later booleans track workflow outcomes.
 std::map<std::uint64_t, std::uint64_t> explore_slot_frames;
 std::uint64_t final_cursor_revision = 0U;
 std::uint64_t final_cursor_frame_revision = 0U;
 std::uint64_t final_cursor_generation = 0U;
 // CLEANUP-IGNORE: Rendered cursor evidence and UI assertions are separate from native lifecycle audit flags.
 std::uint64_t final_cursor_slot_count = 0U;
 bool benchmark_purple = false;
 bool model_copy = false;
 std::set<std::string> explore_integer_controls;
 bool explore_integer_precision = false;
 std::string explore_seed_control;
 std::uint64_t explore_seed_target = 0U;
 std::optional<nlohmann::json> explore_paste_baseline;
 std::optional<nlohmann::json> explore_paste_value;
 bool explore_paste_restored = false;
 std::set<std::uint64_t> detail_resize_measurements;
 std::set<std::string> measured_resize_returns;
 bool spinnerless_integer = false;
 bool spinnerless_floating = false;
 bool advanced_integer_persisted = false;
 bool advanced_floating_persisted = false;
 bool show_fps_round_trip = false;
 bool workspace_fps_text = false;
 bool workspace_fps_pixels = false;
 bool benchmark_round_trip = false;
 bool benchmark_inactive = false;
 std::optional<std::array<double, 4>> benchmark_baseline;
 std::map<std::size_t, std::pair<std::string, std::array<double, 4>>> benchmark_choices;
 std::map<std::size_t, std::string> benchmark_clicks;
 std::map<std::pair<std::size_t, std::string>, std::array<double, 4>> benchmark_visibility;
 [[nodiscard]] bool benchmark_choices_complete() const;
 bool bootstrap = false;
 bool dataset_configured = false;
 bool progress = false;
 bool compile_metrics = false;
 std::uint64_t compile_completed = 0U;
 std::uint64_t compile_total = 0U;
 // CLEANUP-IGNORE: Browser workflow outcomes do not duplicate the native process audit's lifecycle flags.
 std::uint64_t compile_dropped = 0U;
 bool dataset_complete = false;
 bool explore_ready = false;
 bool sweep = false;
 bool scrolled = false;
 bool detail = false;
 bool augmentation_enabled = false;
 bool augmentation_rerolled = false;
 std::map<std::uint64_t, std::uint64_t> augmentation_frames;
 bool reshuffle_order_only = false;
 bool detail_source = false;
 bool reopened = false;
 bool detail_fit = false;
 bool viewer_complete = false;
 bool gallery_no_input_complete = false;
 bool initial_atlas_complete = false;
 std::set<std::pair<std::uint64_t, std::uint64_t>> atlas_canvas_pixels;
 std::set<std::uint64_t> atlas_visibility_modes;
 std::set<std::string> atlas_scroll_stages;
 std::set<std::string> atlas_window_draws;
 std::set<double> atlas_device_scales;
 std::set<std::string> atlas_themes;
 std::set<std::string> atlas_notices;
 std::set<std::pair<std::uint64_t, std::uint64_t>> square_atlas_frames;
 bool atlas_geometry_valid = true;
 bool atlas_native_capacity = false;
 std::uint64_t capacity_retry_publication = 0U;
 std::uint64_t capacity_retry_frame = 0U;
 struct SharedExploreMotion final {
  double x = 0.0, y = 0.0;
  std::size_t ordinal = 0U;
 };
 std::vector<SharedExploreMotion> shared_explore_motion;
 std::size_t held_visible_ordinal = 0U, pending_hover_ordinal = 0U;
 std::uint64_t pending_hover_index = std::numeric_limits<std::uint64_t>::max();
 std::uint64_t pending_selection_index = std::numeric_limits<std::uint64_t>::max();
 std::uint64_t pending_read_generation = 0U;
 std::set<std::pair<std::uint64_t, std::uint64_t>> atlas_scaled_frames;
 std::set<std::string> viewer_import_edits;
 std::set<std::string> annotation_shapes;
 std::set<std::pair<std::uint64_t, std::uint64_t>> annotation_pixel_frames;
 std::map<std::string, std::size_t> annotation_product_operations;
 std::set<std::string> annotation_layouts;
 std::map<std::string, std::set<std::string>> annotation_reachable;
 std::map<std::string, std::map<std::string, std::uint64_t>> annotation_tails;
 std::set<std::string> annotation_capabilities;
 std::size_t annotation_swatches = 0U;
 std::size_t annotation_previews = 0U;
 bool annotation_pixels_valid = true;
 bool annotation_layout_valid = true;
 std::uint64_t viewer_presentation = 0U;
 std::set<std::uint64_t> viewer_overlay_modes;
 std::map<std::uint64_t, std::array<double, 3U>> viewer_label_colors;
 bool viewer_class_colors = true;
 bool viewer_labels_without_boxes = false;
 std::set<std::pair<std::uint64_t, std::uint64_t>> viewer_label_products;
 bool original_detail_drawn = false;
 bool ui_scale_drag = false;
 bool ui_scale_released = false;
 bool ui_scale_restored = false;
 std::set<std::string, std::less<>> ui_scale_pointer_stages;
 // CLEANUP-IGNORE: Modal usability and following rendered-grid facts are independent end-to-end UI evidence,
 // not streaming-algorithm control flags.
 bool error_modal_usable = false;
 bool exact_grid = false;
 std::uint64_t exact_grid_revision = 0U;
 std::uint64_t exact_grid_frame_revision = 0U;
 std::uint64_t exact_grid_capacity_width = 0U;
 std::uint64_t exact_grid_capacity_height = 0U;
 std::uint64_t exact_grid_width = 0U;
 std::uint64_t exact_grid_height = 0U;
 bool newest_placeholder = false;
 bool pointer_inverse = false;
 bool pointer_dispatched = false;
 bool pointer_selected = false;
 std::uint64_t pointer_revision = 0U;
 std::uint64_t pointer_frame_revision = 0U;
 std::uint64_t pending_pointer_frame_revision = 0U;
 std::uint64_t pointer_slot = 0U;
 std::uint64_t pointer_compiled_index = 0U;
 bool upscale_growth = false;
 bool upscale_presentation = false;
 bool upscale_later_frame = false;
 std::array<std::uint64_t, 2U> viewer_navigation_draws{};
 std::set<std::string, std::less<>> upscale_modes;
 std::set<std::string, std::less<>> upscale_presentations;
 std::set<std::string, std::less<>> upscale_completed_pixels;
 std::set<std::string, std::less<>> upscale_same_method;
 bool atlas_identities = false;
 bool annotation_ready = false;
 bool annotation_tool = false;
 bool annotation_pointer = false;
 std::set<std::uint64_t> surface_draws;
 std::set<std::uint64_t> surface_redraws;
 std::map<std::uint64_t, std::size_t> surface_redraw_counts;
 std::vector<SurfaceGeometry> surface_geometries;
 std::vector<SurfaceContainer> surface_containers;
 std::map<std::pair<std::uint64_t, std::uint64_t>, std::pair<std::uint64_t, std::uint64_t>> surface_contents;
 std::map<std::pair<std::uint64_t, std::uint64_t>, double> surface_scales;
 bool complete = false;
 bool failed = false;
 bool firefox_import = false;
 bool firefox_claim = false;
 bool firefox_ready = false;
 bool workspace_protocol_failure = false;
 bool panic = false;
 std::size_t ordinal = 0U;
 std::size_t phase_progress_revision = 0U;
 std::size_t work_progress_revision = 0U;
 std::array<std::pair<std::uint64_t, std::uint64_t>, 4U> workflow_progress{};
 std::uint64_t reopen_snapshot_progress = 0U;
 std::uint64_t reopen_draw_progress = 0U;
 std::string phase_progress_class;
 std::string phase_progress_name;
 std::size_t progress_ordinal = 0U;
 std::size_t dataset_complete_ordinal = 0U;
 std::uint64_t compiled_images = 0U;
 std::uint64_t compiled_width = 0U;
 std::uint64_t compiled_height = 0U;
 std::uint64_t presentation_receipt = 0U;
 [[nodiscard]] bool held_placeholder_motion(const std::uint64_t index, const std::uint64_t generation) const;
 void consume_gallery_generation(const nlohmann::json& record);
 void consume(const nlohmann::json& record);
 [[nodiscard]] std::string_view readiness_blocker() const;
 [[nodiscard]] bool product_ready() const;
 [[nodiscard]] bool terminal_evidence_settled() const noexcept;
 [[nodiscard]] bool rendered_frame_for_slots(const std::map<std::uint64_t, std::uint64_t>& native_slots, const std::uint64_t source_revision, const std::size_t minimum_redraws) const noexcept;
 [[nodiscard]] bool observed_frame_revision_for_slots(const std::map<std::uint64_t, std::uint64_t>& native_slots, const std::uint64_t source_revision) const noexcept;
 [[nodiscard]] const std::map<std::uint64_t, std::uint64_t>* final_cursor_slots() const noexcept;
 [[nodiscard]] const std::map<std::uint64_t, std::uint64_t>* pointer_slots() const noexcept;
 [[nodiscard]] bool failed_before_termination() const noexcept;
 [[nodiscard]] std::string_view failure_blocker() const noexcept;

private:
 static constexpr const char* TRAIN_CARD = "train.card.dataset";
 static constexpr const char* DATASET_SOURCE = "train.dataset.source";
 static constexpr const char* DATASET_BROWSE = "train.dataset.browse";
 static constexpr const char* COMPILED_DIRECTORY = "train.dataset.compiled_directory";
 static constexpr const char* COMPILE_RESOLUTION = "train.dataset.resolution";
 static constexpr const char* COMPILE_DIMENSIONS = "train.dataset.compile_dimensions";
 static constexpr const char* COMPILE_DATASET = "train.compile_dataset";
 static constexpr const char* COMPILE_PROGRESS = "train.compile_dataset.progress";
 static constexpr const char* DATASET_STATUS = "train.dataset.status";
 static constexpr const char* TRAIN_MODEL_CARD = "train.card.model";
 static constexpr const char* TRAIN_MODEL_PROGRESS = "train.card.model.progress";
 static constexpr const char* TRAIN_MODEL_SELECTOR = "train.model.selector";
 static constexpr const char* TRAIN_MODEL_PRESETS = "train.model.presets";
 static constexpr const char* TRAIN_MODEL_DIVIDER = "train.model.divider";
 static constexpr const char* TRAIN_MODEL_CUSTOM = "train.model.custom_weights";
 static constexpr const char* TRAIN_MODEL_STATUS = "train.model.status";
 static constexpr const char* TRAIN_MODEL_ACTION = "train.model.action";
 static constexpr const char* MATCH_FREE_ASSIGNMENT = "train.advanced.assignment.match_free";
 static constexpr const char* BENCHMARK_OVERRIDE = "train.dataset.benchmark_override";
 static constexpr const char* EXPLORE_OPEN = "explore.open";
 static constexpr const char* EXPLORE_STOP = "explore.stop";
 static constexpr const char* EXPLORE_DATASET_PANE = "explore.pane.dataset_filters";
 static constexpr const char* EXPLORE_DETAILS_PANE = "explore.pane.dataset_details";
 static constexpr const char* EXPLORE_CARD = "explore.card.status";
 static constexpr const char* EXPLORE_LATER = "explore.gallery.later";
 static constexpr const char* EXPLORE_AUGMENTATION_TOGGLE = "explore.gallery.augmentation";
 static constexpr const char* EXPLORE_AUGMENTATION_REROLL = "explore.gallery.augmentation.reroll";
 static constexpr const char* EXPLORE_RESHUFFLE = "explore.gallery.reshuffle";
 static constexpr const char* EXPLORE_DETAIL_ORIGINAL = "explore.detail.source.original";
 static constexpr const char* EXPLORE_DETAIL_FIT = "explore.detail.fit";
 static constexpr const char* EXPLORE_DETAIL_WORKSPACE = "explore.detail.workspace";
 static constexpr const char* EXPLORE_UPSCALE_BASIC = "explore.detail.upscale.basic";
 static constexpr const char* EXPLORE_UPSCALE_FAST = "explore.detail.upscale.fast";
 static constexpr const char* EXPLORE_UPSCALE_NEURAL = "explore.detail.upscale.neural";
 static constexpr const char* EXPLORE_NEXT = "explore.detail.next";
 static constexpr const char* EXPLORE_PREVIOUS = "explore.detail.previous";
 static constexpr const char* EXPLORE_ANNOTATE = "explore.detail.open_annotation";
 static constexpr const char* ANNOTATION_SURFACE = "annotation.workspace.surface";
 static constexpr const char* ANNOTATION_SIDEBAR = "annotation.sidebar";
 static constexpr const char* ANNOTATION_TIMELINE = "annotation.timeline";
 static constexpr const char* ANNOTATION_OPERATION = "annotation.operation";
 static constexpr const char* ANNOTATION_STOP = "annotation.stop";
 static constexpr const char* ANNOTATION_BRUSH_RADIUS = "annotation.brush_radius";
 static constexpr const char* SETTINGS_MODAL = "settings.modal";
 static constexpr const char* ERROR_MODAL = "error.modal";
 static constexpr const char* ERROR_COPY = "error.copy";
 static constexpr const char* ERROR_DISMISS = "error.dismiss";
 static constexpr const char* SETTINGS_FOOTER = "settings.footer";
 static constexpr const char* SETTINGS_RESET = "settings.reset";
 static constexpr const char* SETTINGS_CLOSE = "settings.close";
 static constexpr std::array<std::string_view, 5> SETTINGS_NUMERIC_CONTROLS{
  "settings.ui_scale",
  "settings.font_size",
  "settings.secondary_font_size",
  "settings.mono_font_size",
  "settings.text_input_font_size",
 };
};
}  // namespace mmltk::acceptance::wayland
