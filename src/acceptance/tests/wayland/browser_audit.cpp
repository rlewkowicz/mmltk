#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/dataset_compiler.h"
#include <charconv>
#include <system_error>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include "src/backend/imaging/raster/class_palette.h"
#include "src/controller/contracts/workspace_input.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/presentation/annotation_palette.h"
#include "browser_audit.h"
namespace mmltk::acceptance::wayland {
auto AtlasDrawAudit::acquisition_for(const SampleKey& key) const -> const nlohmann::json* {
 const auto found = acquisitions.find(key);
 return found != acquisitions.end() ? &found->second : nullptr;
}
auto AtlasDrawAudit::empty_draw_submitted(const SurfaceAudit& physical) const -> bool {
 if (!valid || !empty_seen) return false;
 return std::ranges::any_of(acquisitions, [&](const auto& entry) {
  const auto& [key, record] = entry;
  const auto count = record.find("matching_count");
  if (count == record.end() || !count->is_number_unsigned() || *count != 0U) return false;
  const auto& [identity, publication] = key;
  const auto surface = physical.surfaces.find(identity);
  if (surface == physical.surfaces.end() || surface->second.width != scalar(record, "width") || surface->second.height != scalar(record, "height")) return false;
  const auto draw = surface->second.custody.find(publication);
  return draw != surface->second.custody.end() && draw->second.acquired && draw->second.settled != 0U;
 });
}
auto AtlasDrawAudit::check(const bool passed, const std::string_view reason, const nlohmann::json& record, const nlohmann::json* prior, const std::string_view field, const nlohmann::json* expected)
 -> bool {
 if (!passed)
  failure.capture(reason, record, [&] {
   nlohmann::json context{{"completed_scroll_stages", stages}, {"completed_held_stages", held_stages.size()}, {"return_stage", return_stage}, {"away_return", away_return},
    {"source_count", sources.size()}, {"acquisition_count", acquisitions.size()}, {"ready_sample_count", ready_cell_samples.size()}, {"retained_pixel_count", retained_cell_pixels.size()},
    {"retained_readiness_count", retained_ready.size()}, {"record_limit", kAcceptanceRecordLimit}};
   if (stages.size() < stage_names.size()) context["expected_scroll_stage"] = stage_names[stages.size()];
   if (held_stages.size() < held_names.size()) context["expected_held_stage"] = held_names[held_stages.size()];
   if (return_stage < return_names.size()) context["expected_return_stage"] = return_names[return_stage];
   if (staged_allocation) context["staged_allocation"] = *staged_allocation;
   if (!last_draw && last_draw_reset) context["last_draw_reset"] = *last_draw_reset;
   if (prior) context["prior"] = *prior;
   if (!field.empty()) context["field"] = field;
   if (expected) context["expected"] = *expected;
   return context;
  });
 return passed;
}
auto AtlasDrawAudit::check_fields(const nlohmann::json& record, const nlohmann::json& prior, const std::initializer_list<const char*> fields, const std::string_view reason) -> bool {
 const auto* field = differing_field(record, prior, fields);
 return !field || check(false, reason, record, &prior, field, prior.contains(field) ? &prior[field] : nullptr);
}
auto AtlasDrawAudit::source_key(const nlohmann::json& record) -> SourceKey {
 return {scalar(record, "content_session"), scalar(record, "source_kind"), scalar(record, "source_instance"), scalar(record, "source_revision")};
}
auto AtlasDrawAudit::sample_key(const nlohmann::json& record) -> SampleKey { return {record.value("surface", ""), scalar(record, "presentation_revision")}; }
auto AtlasDrawAudit::allocation_key(const nlohmann::json& record) -> AllocationKey { return {record.value("surface", ""), scalar(record, "width"), scalar(record, "height")}; }
auto AtlasDrawAudit::observe_staged_rows(const AllocationKey& allocation, const std::uint64_t rows) -> void {
 if (!staged_allocation) {
  staged_allocation = allocation;
 } else if (*staged_allocation != allocation) {
  return;
 } else {
  if (rows == previous_staged_rows + 1U) staged_transitions |= 1U;
  if (previous_staged_rows == rows + 1U) staged_transitions |= 2U;
 }
 grid_round_trip = staged_transitions == 3U;
 previous_staged_rows = rows;
}
auto AtlasDrawAudit::differing_field(const nlohmann::json& left, const nlohmann::json& right, const std::initializer_list<const char*> fields) -> const char* {
 const auto found = std::ranges::find_if(fields, [&](const auto* field) { return !left.contains(field) || !right.contains(field) || left[field] != right[field]; });
 return found != fields.end() ? *found : nullptr;
}
auto AtlasDrawAudit::same_fields(const nlohmann::json& left, const nlohmann::json& right, const std::initializer_list<const char*> fields) -> bool {
 return differing_field(left, right, fields) == nullptr;
}
auto AtlasDrawAudit::ready_pixels_complete(const nlohmann::json& record) -> bool {
 const auto samples = ready_cell_samples.find(sample_key(record));
 if (!check(samples != ready_cell_samples.end(), "stage_ready_pixel_samples_missing", record) || !check(visible(record), "stage_ready_pixels_not_visible", record) ||
     !check(
      record.contains("ready_slots") && record["ready_slots"].is_array() && record.contains("visible_indices") && record["visible_indices"].is_array(), "stage_ready_pixel_metadata_invalid", record))
  return false;
 const auto columns = scalar(record, "columns");
 if (!check(columns != 0U, "stage_ready_pixel_columns_zero", record)) return false;
 const double side = record["image"][2].get<double>() / static_cast<double>(columns);
 for (std::size_t slot = 0U; slot < record["visible_indices"].size(); ++slot) {
  if (slot >= record["ready_slots"].size() || record["ready_slots"][slot] != true) continue;
  const double left = record["image"][0].get<double>() + (static_cast<double>(slot % columns) + .2) * side;
  const double top = record["image"][1].get<double>() + (static_cast<double>(slot / columns) + .2) * side;
  const double width = std::min(left + .6 * side, record["clip"][0].get<double>() + record["clip"][2].get<double>()) - std::max(left, record["clip"][0].get<double>());
  const double height = std::min(top + .6 * side, record["clip"][1].get<double>() + record["clip"][3].get<double>()) - std::max(top, record["clip"][1].get<double>());
  if (width >= 2.0 && height >= 2.0) {
   const nlohmann::json& index = record["visible_indices"][slot];
   if (!check(samples->second.contains(index.get<std::uint64_t>()), "stage_ready_cell_sample_missing", record, nullptr, "compiled_index", &index)) return false;
  }
 }
 return true;
}
auto AtlasDrawAudit::source_stage_matches_draw(const nlohmann::json& record) -> bool {
 return check(last_draw.has_value(), "stage_last_draw_missing", record) && check_fields(record, *last_draw, source_fields, "stage_source_differs_from_draw") &&
        check(sample_key(record) == sample_key(*last_draw), "stage_sample_differs_from_draw", record, &*last_draw) && ready_pixels_complete(record);
}
auto AtlasDrawAudit::clear_last_draw(const nlohmann::json& record) -> void {
 if (last_draw) last_draw_reset = record;
 last_draw.reset();
}
auto AtlasDrawAudit::stage(const nlohmann::json& record) -> void {
 const auto name = record.value("control", "");
 if (name.starts_with("resize-")) {
  static constexpr std::array names{"resize-landscape", "resize-portrait-return", "resize-landscape-return"};
  bool matching = check(resize_stages.size() < names.size() && name == names[resize_stages.size()], "resize_stage_out_of_order", record) && source_stage_matches_draw(record);
  if (matching) {
   const auto& clip = record.at("clip");
   const auto& image = record.at("image");
   matching = check(image[1].get<double>() + image[3].get<double>() >= clip[1].get<double>() + clip[3].get<double>() - 1.0, "resized_atlas_does_not_cover_lower_rows", record);
   if (!resize_stages.empty()) {
    const auto& previous = resize_stages.back();
    const bool portrait = name == "resize-portrait-return";
    matching = matching &&
               check(portrait ? scalar(record, "rows") > scalar(previous, "rows") : scalar(record, "rows") < scalar(previous, "rows"), "resize_row_direction_mismatch", record, &previous) &&
               check_fields(record, previous, {"dataset_identity", "columns", "first_row"}, "resize_changed_gallery_identity");
   }
  }
  valid = valid && matching;
  if (matching) resize_stages.push_back(record);
  clear_last_draw(record);
  return;
 }
 if (name == "away-return") {
  const bool matching = check(!away_return, "away_return_repeated", record) && source_stage_matches_draw(record);
  valid = valid && matching;
  away_return = matching;
  clear_last_draw(record);
  return;
 }
 if (name.starts_with("held-")) {
  bool matching = check(held_stages.size() < held_names.size() && name == held_names[held_stages.size()], "held_stage_out_of_order", record) && source_stage_matches_draw(record);
  if (matching && held_stages.contains("held-visible")) {
   const auto& baseline = held_stages.at("held-visible");
   matching = check_fields(record, baseline, {"columns", "card_extent", "dataset_identity", "first_row"}, "held_stage_differs_from_baseline") &&
              check(record["clip"][3] == baseline["clip"][3], "held_stage_clip_height_changed", record, &baseline);
   if (name == "held-aligned" || name == "held-extra" || name == "held-restored")
    matching = matching && check(return_baseline.has_value(), "held_stage_return_baseline_missing", record) &&
               check(scalar(record, "rows") == scalar(*return_baseline, "rows") + (name == "held-extra" ? 1U : 0U), "held_stage_row_count_mismatch", record, &*return_baseline);
  }
  valid = valid && matching;
  if (matching) held_stages.emplace(name, record);
  clear_last_draw(record);
  return;
 }
 if (name.starts_with("return-")) {
  bool matching = check(return_stage < return_names.size() && name == return_names[return_stage], "return_stage_out_of_order", record) && source_stage_matches_draw(record);
  if (matching && return_baseline) {
   matching = check_fields(record, *return_baseline, {"columns", "card_extent", "dataset_identity"}, "return_stage_differs_from_baseline") &&
              check(record["clip"][3] == (*return_baseline)["clip"][3], "return_stage_clip_height_changed", record, &*return_baseline) &&
              check(scalar(record, "first_row") == 0U, "return_stage_first_row_not_zero", record) &&
              check(scalar(record, "rows") == scalar(*return_baseline, "rows") + (return_stage == 2U ? 1U : 0U), "return_stage_row_count_mismatch", record, &*return_baseline);
  }
  valid = valid && matching;
  if (matching) {
   if (!return_baseline) return_baseline = record;
   ++return_stage;
   return_round_trip = return_stage == return_names.size();
  }
  clear_last_draw(record);
  return;
 }
 bool matching = check(stages.size() < stage_names.size() && name == stage_names[stages.size()], "scroll_stage_out_of_order", record) &&
                 check(last_draw.has_value(), "stage_last_draw_missing", record) && check_fields(record, *last_draw, image_fields, "scroll_stage_image_differs_from_draw") &&
                 check_fields(record, *last_draw, {"bounds", "image", "clip"}, "scroll_stage_geometry_differs_from_draw");
 const auto allocation = allocation_key(record);
 matching = matching && check(!staged_allocation || *staged_allocation == allocation, "scroll_stage_allocation_changed", record);
 if (matching) {
  const auto row = scalar(record, "first_row");
  const double image_top = record["image"][1].get<double>();
  const double clip_top = record["clip"][1].get<double>();
  if (name == "fractional") matching = check(row == 0U && image_top < clip_top, "fractional_stage_row_or_clip_mismatch", record);
  if (name == "row1") matching = check(row == 1U, "scroll_stage_expected_row1", record);
  if (name == "row2") matching = check(row == 2U, "scroll_stage_expected_row2", record);
  if (name == "row10") matching = check(row == 10U, "scroll_stage_expected_row10", record);
  if (name == "row9") matching = check(row == 9U, "scroll_stage_expected_row9", record);
  if (name == "restored") matching = check(row == 0U && std::abs(image_top - clip_top) < 1.0, "restored_stage_row_or_clip_mismatch", record);
  if (name == "end") {
   const auto columns = scalar(record, "columns");
   const auto total = scalar(record, "matching_count");
   matching = check(columns != 0U && row + scalar(record, "rows") == (total + columns - 1U) / columns, "end_stage_row_mismatch", record) &&
              check(std::abs(image_top + record["image"][3].get<double>() - clip_top - record["clip"][3].get<double>()) < 1.0, "end_stage_clip_mismatch", record);
  }
 }
 valid = valid && matching;
 if (matching) {
  stages.emplace(name);
  observe_staged_rows(allocation, scalar(record, "rows"));
 }
 clear_last_draw(record);
}
auto AtlasDrawAudit::visible(const nlohmann::json& record) -> bool {
 const auto rectangle = [&](const char* field) {
  if (!record.contains(field) || !record[field].is_array() || record[field].size() != 4U) return std::optional<std::array<double, 4U>>{};
  std::array<double, 4U> result{};
  for (std::size_t i = 0U; i != result.size(); ++i) {
   if (!record[field][i].is_number()) return std::optional<std::array<double, 4U>>{};
   result[i] = record[field][i].get<double>();
   if (!std::isfinite(result[i])) return std::optional<std::array<double, 4U>>{};
  }
  if (result[2] <= 0.0 || result[3] <= 0.0) return std::optional<std::array<double, 4U>>{};
  return std::optional{result};
 };
 const auto image = rectangle("image"), clip = rectangle("clip"), bounds = rectangle("bounds");
 if (!image || !clip || !bounds || (*clip)[0] < 0.0 || (*clip)[1] < 0.0) return false;
 return std::max((*image)[0], (*clip)[0]) < std::min((*image)[0] + (*image)[2], (*clip)[0] + (*clip)[2]) &&
        std::max((*image)[1], (*clip)[1]) < std::min((*image)[1] + (*image)[3], (*clip)[1] + (*clip)[3]);
}
auto AtlasDrawAudit::consume(const nlohmann::json& record) -> void {
 const std::string event = record.value("event", "");
 if (event == "integration.atlas_ready_cell") {
  const auto acquisition = acquisitions.find(sample_key(record));
  bool matching = check(acquisition != acquisitions.end(), "ready_cell_acquisition_missing", record) && check(record.value("matched", false), "ready_cell_pixels_mismatched", record) &&
                  check(scalar(record, "sampled_pixels") != 0U, "ready_cell_sampled_pixels_zero", record) &&
                  check_fields(record, acquisition->second, source_fields, "ready_cell_source_differs_from_acquisition");
  const auto index = scalar(record, "compiled_index");
  if (matching) {
   const auto& indices = record["visible_indices"];
   const auto found = std::ranges::find(indices, nlohmann::json(index));
   const auto slot = static_cast<std::size_t>(found - indices.begin());
   matching = check(found != indices.end(), "ready_cell_index_not_visible", record) &&
              check(record.contains("ready_slots") && slot < record["ready_slots"].size() && record["ready_slots"][slot] == true, "ready_cell_slot_not_ready", record);
  }
  matching = matching && check(record.contains("cell_sample_x") && record.contains("cell_sample_y") && record.contains("cell_sample_rgba"), "ready_cell_sample_point_missing", record);
  if (matching) {
   if (pixel_meaning && !same_fields(record, *pixel_meaning, {"dataset_identity", "card_extent", "augmentation_enabled", "augmentation_seed", "overlay_boxes", "overlay_masks", "overlay_labels"}))
    retained_cell_pixels.clear();
   pixel_meaning = record;
   const auto point = std::tuple{index, scalar(record, "cell_sample_x"), scalar(record, "cell_sample_y")};
   const auto prior = retained_cell_pixels.find(point);
   if (prior != retained_cell_pixels.end() && return_stage > 0U && stages.empty() && !away_return && prior->second.rgba != scalar(record, "cell_sample_rgba")) {
    const nlohmann::json expected = prior->second.rgba;
    matching = check(false, "retained_ready_cell_pixel_changed", record, acquisition_for(prior->second.sample), "cell_sample_rgba", &expected);
   }
   if (retained_cell_pixels.size() >= kAcceptanceRecordLimit && prior == retained_cell_pixels.end())
    matching = check(false, "retained_cell_pixel_capacity_exhausted", record);
   else {
    auto& retained = retained_cell_pixels[point];
    retained.rgba = scalar(record, "cell_sample_rgba");
    retained.sample = acquisition->first;
   }
  }
  valid = valid && matching;
  if (matching) {
   if (ready_cell_samples.size() >= kAcceptanceRecordLimit && !ready_cell_samples.contains(sample_key(record)))
    valid = check(false, "ready_cell_sample_capacity_exhausted", record);
   else
    ready_cell_samples[sample_key(record)].emplace(index);
  }
  return;
 }
 if (event == "iced.gallery.source") {
  const auto key = source_key(record);
  const auto session = std::pair{key[0], key[3]};
  const auto existing = sources.find(key);
  if (std::ranges::any_of(key, [](const auto value) { return value == 0U; })) {
   valid = check(false, "gallery_source_identity_zero", record);
  } else if (existing != sources.end()) {
   // Cached pixels retain their source revision when a new work
   // generation observes them. Only their physical source facts
   // are immutable; readiness is checked on each actual draw.
   valid = valid && check_fields(record, existing->second, source_fields, "gallery_source_facts_changed");
  } else if (sources.size() >= kAcceptanceRecordLimit || sessions.contains(session)) {
   const auto conflicting = sessions.find(session);
   valid = check(false, sources.size() >= kAcceptanceRecordLimit ? "gallery_source_capacity_exhausted" : "gallery_source_session_revision_reused", record,
    conflicting != sessions.end() ? &sources.at(conflicting->second) : nullptr);
  } else {
   sources.emplace(key, record);
   sessions.emplace(session, key);
  }
  return;
 }
 if (event == "iced.surface.sample_acquired") {
  const auto key = sample_key(record);
  if (acquisitions.size() >= kAcceptanceRecordLimit || acquisitions.contains(key)) {
   const auto prior = acquisitions.find(key);
   valid = check(
    false, acquisitions.size() >= kAcceptanceRecordLimit ? "sample_acquisition_capacity_exhausted" : "sample_acquisition_repeated", record, prior != acquisitions.end() ? &prior->second : nullptr);
   return;
  }
  acquisitions.emplace(key, record);
  return;
 }
 if (event == "iced.surface.scroll_stage") {
  stage(record);
  return;
 }
 if (record.value("control", "") != kExploreGalleryControl) return;
 if (event == "iced.surface.sample_draw_clipped" || event == "iced.surface.sample_draw_rejected") {
  clear_last_draw(record);
  return;
 }
 if (event != "iced.surface.draw_encoded") return;
 const auto source = sources.find(source_key(record));
 const auto capture = acquisitions.find(sample_key(record));
 const auto columns = scalar(record, "columns"), rows = scalar(record, "rows");
 const auto width = scalar(record, "content_width"), height = scalar(record, "content_height");
 bool matching = check(source != sources.end(), "draw_gallery_source_missing", record) && check(capture != acquisitions.end(), "draw_sample_acquisition_missing", record) &&
                 check(visible(record), "draw_visible_geometry_invalid", record) &&
                 check(SurfaceAudit::valid_identity(record.value("surface", "")) && scalar(record, "presentation_revision") != 0U, "draw_sample_identity_invalid", record) &&
                 check(width <= scalar(record, "width") && height <= scalar(record, "height"), "draw_content_exceeds_allocation", record) &&
                 check(scalar(record, "source_revision") == scalar(record, "frame_revision"), "draw_source_frame_revision_mismatch", record) &&
                 check(columns != 0U && rows != 0U && width != 0U && height != 0U && scalar(record, "card_extent") != 0U, "draw_atlas_geometry_empty", record) &&
                 check(scalar(record, "row_capacity") >= rows && scalar(record, "row_origin") < scalar(record, "row_capacity"), "draw_atlas_row_capacity_invalid", record) &&
                 check(width / columns == scalar(record, "card_extent") && width % columns == 0U, "draw_atlas_column_extent_mismatch", record) &&
                 check(height == scalar(record, "row_capacity") * scalar(record, "card_extent"), "draw_atlas_capacity_height_mismatch", record);
 if (matching) {
  matching = check_fields(record, source->second, source_fields, "draw_source_facts_mismatch") && check_fields(record, capture->second, image_fields, "draw_acquisition_facts_mismatch") &&
             check_fields(record, capture->second, {"layer", "slot"}, "draw_acquisition_slot_mismatch");
  const auto& image = record["image"];
  const auto& bounds = record["bounds"];
  matching = matching && check(std::abs(image[2].get<double>() / static_cast<double>(columns) - image[3].get<double>() / static_cast<double>(rows)) < 0.01, "draw_card_aspect_mismatch", record) &&
             check(image[0] == bounds[0] && image[1] == bounds[1] && image[2] == bounds[2], "draw_image_bounds_mismatch", record);
 }
 if (matching) {
  matching = check(record.contains("visible_indices") && record["visible_indices"].is_array() && record.contains("ready_slots") && record["ready_slots"].is_array() &&
                    record["ready_slots"].size() == record["visible_indices"].size(),
   "draw_readiness_cardinality_mismatch", record);
 }
 if (matching) {
  const auto count = record.find("matching_count");
  matching = check(count != record.end() && count->is_number_unsigned(), "draw_matching_count_invalid", record) &&
             check(scalar(record, "matching_count") != 0U || record["visible_indices"].empty(), "draw_empty_gallery_has_visible_cells", record);
 }
 if (matching && overlap_draw && !same_fields(record, *overlap_draw, {"dataset_identity", "card_extent", "augmentation_enabled", "augmentation_seed"})) retained_ready.clear();
 if (matching) {
  for (std::size_t slot = 0U; slot < record["visible_indices"].size(); ++slot) {
   if (!record["visible_indices"][slot].is_number_unsigned() || !record["ready_slots"][slot].is_boolean()) {
    const nlohmann::json expected{{"slot", slot}, {"visible_indices_type", "unsigned"}, {"ready_slots_type", "boolean"}};
    matching = check(false, "draw_slot_metadata_type_invalid", record, nullptr, "visible_indices/ready_slots", &expected);
    break;
   }
   const auto index = record["visible_indices"][slot].get<std::uint64_t>();
   const bool ready = record["ready_slots"][slot].get<bool>();
   const auto found = retained_ready.find(index);
   // The return window is deliberately retained. Other distant
   // scrolls can legitimately evict cells from the bounded cache.
   if (return_stage > 0U && !away_return && stages.empty() && !held_stages.contains("held-complete") && found != retained_ready.end() && found->second.ready && !ready) {
    const nlohmann::json expected{{"compiled_index", index}, {"ready", true}};
    matching = check(false, "retained_ready_cell_became_placeholder", record, acquisition_for(found->second.sample), "ready_slots", &expected);
   }
   if (retained_ready.size() >= kAcceptanceRecordLimit && found == retained_ready.end())
    matching = check(false, "retained_readiness_capacity_exhausted", record);
   else {
    auto& retained = retained_ready[index];
    retained.ready = ready;
    retained.sample = capture->first;
   }
  }
 }
 valid = valid && matching;
 if (matching) {
  overlap_draw = record;
  seen = true;
  empty_seen = empty_seen || scalar(record, "matching_count") == 0U;
  if (staged_allocation) observe_staged_rows(allocation_key(record), rows);
  last_draw = record;
  last_draw_reset.reset();
  drawn_rows.emplace(scalar(record, "first_row"));
 } else {
  clear_last_draw(record);
 }
}
auto BrowserAudit::held_placeholder_motion(const std::uint64_t index, const std::uint64_t generation) const -> bool {
 const auto held = atlas_draws.held_stages.find("held-visible");
 if (!atlas_draws.valid || held == atlas_draws.held_stages.end() || pending_hover_index != index || pending_read_generation != generation || held_visible_ordinal == 0U ||
     pending_hover_ordinal <= held_visible_ordinal)
  return false;
 const auto& draw = held->second;
 const auto columns = scalar(draw, "columns"), side = scalar(draw, "card_extent");
 const auto& indices = draw["visible_indices"];
 const auto image = std::ranges::find(indices, nlohmann::json(index));
 if (columns == 0U || side == 0U || image == indices.end()) return false;
 const auto slot = static_cast<std::size_t>(image - indices.begin());
 return std::ranges::any_of(shared_explore_motion, [&](const auto& motion) {
  return motion.ordinal > held_visible_ordinal && motion.ordinal < pending_hover_ordinal && motion.x >= static_cast<double>((slot % columns) * side) &&
         motion.x < static_cast<double>((slot % columns + 1U) * side) && motion.y >= static_cast<double>((slot / columns) * side) && motion.y < static_cast<double>((slot / columns + 1U) * side);
 });
}
auto BrowserAudit::consume_gallery_generation(const nlohmann::json& record) -> void {
 const auto generation = scalar(record, "gallery_generation");
 const auto indices = record.find("visible_indices");
 if (generation == 0U || indices == record.end() || !indices->is_array() || indices->size() > mmltk::controller::kExploreVisibleItemCapacity) {
  bounds_valid = false;
  return;
 }
 std::vector<std::uint32_t> visible_indices;
 visible_indices.reserve(indices->size());
 std::map<std::uint64_t, std::uint64_t> slots;
 for (std::size_t slot = 0U; slot != indices->size(); ++slot) {
  const auto& compiled_index = (*indices)[slot];
  if (!compiled_index.is_number_unsigned() || compiled_index.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
   bounds_valid = false;
   return;
  }
  const auto value = compiled_index.get<std::uint32_t>();
  visible_indices.push_back(value);
  slots.emplace(slot, value);
 }
 std::vector<bool> ready_slots;
 if (const auto readiness = record.find("ready_slots"); readiness != record.end()) {
  if (!readiness->is_array() || readiness->size() != slots.size() || !std::ranges::all_of(*readiness, [](const auto& ready) { return ready.is_boolean(); })) {
   bounds_valid = false;
   return;
  }
  ready_slots = readiness->get<std::vector<bool>>();
 }
 const GalleryGenerationEvidence evidence{
  .slots = std::move(slots),
  .digest = mmltk::controller::explore_visible_indices_digest(visible_indices),
  .source_revision = scalar(record, "source_revision"),
  .snapshot_revision = scalar(record, "source_observation_revision"),
  .ready_slots = std::move(ready_slots),
 };
 const auto existing = gallery_generations.find(generation);
 if (existing != gallery_generations.end()) {
  bounds_valid = bounds_valid && existing->second.slots == evidence.slots && existing->second.digest == evidence.digest;
  if (evidence.source_revision >= existing->second.source_revision) {
   existing->second.source_revision = evidence.source_revision;
   existing->second.snapshot_revision = evidence.snapshot_revision;
   existing->second.ready_slots = evidence.ready_slots;
  }
 } else if (gallery_generations.size() == kAcceptanceGenerationLimit) {
  bounds_valid = false;
 } else {
  gallery_generations.emplace(generation, evidence);
 }
}
auto BrowserAudit::consume(const nlohmann::json& record) -> void {
 atlas_draws.consume(record);
 const auto surface_event = record.value("event", "");
 if (surface_event == "firefox.adapter.selected") {
  const auto display = record.value("display_pci_bus_id", "");
  if (display.empty() || record.value("adapter_pci_bus_id", "") != display) workspace_protocol_failure = true;
 } else if (surface_event == "iced.surface.renderer_reconstructed")
  ++renderer_reconstructions[record.value("requested_surface", "")];
 else if (surface_event == "iced.surface.draw_encoded") {
  owned_atlas_seen = atlas_draws.seen;
  const auto control = record.value("control", "");
  if (control == kExploreGalleryControl && atlas_draws.last_draw)
   owned_atlas_current = true;
  else if (control == EXPLORE_DETAIL_WORKSPACE || control == "workflow.visual.workspace")
   owned_atlas_current = false;
 } else if (surface_event == "iced.surface.sample_draw_missing") {
  const auto control = record.value("control", "");
  if (!owned_atlas_interrupted && owned_atlas_current && (control.empty() || control == kExploreGalleryControl))
   owned_atlas_failure.capture("owned_atlas_sample_draw_missing", record, [&] {
    const auto& prior = atlas_draws.overlap_draw ? atlas_draws.overlap_draw : prior_scenario_atlas_draw;
    nlohmann::json context{{"browser_ordinal", ordinal + 1U}, {"owned_atlas_current", owned_atlas_current}};
    if (prior) context["prior"] = *prior;
    return context;
   });
  owned_atlas_interrupted = owned_atlas_interrupted || (owned_atlas_current && (control.empty() || control == kExploreGalleryControl));
 }
 ++ordinal;
 const std::string event = record.value("event", "");
 if (event == "iced.surface.scroll_stage" && record.value("control", "") == "held-visible" && atlas_draws.valid && atlas_draws.held_stages.contains("held-visible")) held_visible_ordinal = ordinal;
 if (event == "iced.gallery.source") consume_gallery_generation(record);
 if (event == "integration.phase_progress") {
  const std::string deadline_class = record.value("control", "");
  const std::string phase = record.value("detail", "");
  const bool valid_class = deadline_class == "startup" || deadline_class == "interaction" || deadline_class == "work";
  bounds_valid = bounds_valid && valid_class && !phase.empty();
  if (valid_class && !phase.empty()) {
   if (phase == phase_progress_name) {
    bounds_valid = bounds_valid && phase_progress_class == deadline_class;
   } else {
    phase_progress_class = deadline_class;
    phase_progress_name = phase;
    ++phase_progress_revision;
   }
  }
 } else if (event == "integration.workflow.pixels" && record.value("detail", "") == "validate-to-explore") {
  const auto sampled = scalar(record, "c"), colored = scalar(record, "d");
  const auto width = scalar(record, "sample_width"), height = scalar(record, "sample_height");
  validate_to_explore_pixels = record.value("control", "") == kExploreGalleryControl && scalar(record, "a") != 0U && scalar(record, "b") != 0U &&
                              record.value("ready_tile", false) && record.value("matched", false) && record.contains("compiled_index") &&
                              width > 0U && width <= 8U && height > 0U && height <= 8U && sampled == width * height &&
                              colored >= 12U && colored <= sampled && colored * 2U >= sampled;
 } else if (event == "integration.validation_layout") {
  validation_layout[{record.value("detail", ""), record.value("control", "")}] = {numeric(record, "a"), numeric(record, "b"), numeric(record, "c"), numeric(record, "d")};
 } else if (event == "integration.validation_text") {
  validation_text.insert(record.value("control", ""));
 } else if (event == "integration.validation_confidence_edit") {
  validation_confidence_edits.push_back(record);
 } else if (event == "integration.validation_confidence_pixels") {
  validation_confidence_pixels.push_back(record);
 } else if (event == "integration.workflow.operation_progress") {
  constexpr std::array<std::string_view, 4U> primary_controls{"train.primary", "validate.primary", "predict.primary", "export.primary"};
  const auto control = std::ranges::find(primary_controls, record.value("control", ""));
  const auto native_progress = std::pair{scalar(record, "a"), scalar(record, "b")};
  if (control != primary_controls.end() && native_progress.first != 0U && native_progress.second != 0U && phase_progress_class == "work" &&
      phase_progress_name == "Workflows(" + record.value("detail", "") + ")") {
   auto& prior = workflow_progress[static_cast<std::size_t>(control - primary_controls.begin())];
   if (native_progress > prior) {
    prior = native_progress;
    ++work_progress_revision;
   }
  }
 } else if (event == "integration.explore_reopen_wait") {
  const auto revision = scalar(record, "a");
  if (revision > reopen_snapshot_progress) {
   reopen_snapshot_progress = revision;
   ++work_progress_revision;
  }
 } else if (event == "integration.explore_reopen_draw") {
  const auto presentation_revision = scalar(record, "d");
  if (presentation_revision > reopen_draw_progress) {
   reopen_draw_progress = presentation_revision;
   ++work_progress_revision;
  }
 } else if (event == "integration.control_bounds") {
  const std::string control = record.value("control", "");
  const double width = numeric(record, "c");
  const double height = numeric(record, "d");
  const Bounds bounds{
   .x = numeric(record, "a"),
   .y = numeric(record, "b"),
   .width = width,
   .height = height,
  };
  bounds_valid = bounds_valid && !control.empty() && width > 0.0 && height > 0.0;
  if (!control.empty()) controls.insert(control);
  // CLEANUP-IGNORE: Each stable visual control maps to a distinct acceptance-evidence slot.
  if (control == EXPLORE_DATASET_PANE)
   explore_dataset = bounds;
  else if (control == kExploreGalleryControl)
   explore_gallery = bounds;
  else if (control == EXPLORE_DETAILS_PANE)
   explore_details = bounds;
  else if (control == COMPILE_DATASET)
   compile_action = bounds;
  else if (control == COMPILE_PROGRESS)
   compile_progress = bounds;
  else if (control == TRAIN_MODEL_CARD)
   model_card = bounds;
  else if (control == TRAIN_MODEL_PROGRESS)
   model_progress = bounds;
  else if (control == TRAIN_MODEL_SELECTOR)
   model_parts[0] = bounds;
  else if (control == TRAIN_MODEL_PRESETS)
   model_parts[1] = bounds;
  else if (control == TRAIN_MODEL_DIVIDER)
   model_parts[2] = bounds;
  else if (control == TRAIN_MODEL_CUSTOM)
   model_parts[3] = bounds;
  else if (control == TRAIN_MODEL_STATUS)
   model_parts[4] = bounds;
  else if (control == TRAIN_MODEL_ACTION)
   model_parts[5] = bounds;
  else if (control == SETTINGS_MODAL)
   settings_modal = bounds;
  else if (control == SETTINGS_FOOTER)
   settings_footer = bounds;
  else if (control == SETTINGS_RESET)
   settings_reset = bounds;
  else if (control == SETTINGS_CLOSE)
   settings_close = bounds;
  else if (control == "settings.group.appearance")
   settings_appearance = bounds;
  else if (control == "settings.group.typography")
   settings_typography = bounds;
  else if (control == "settings.group.environment")
   settings_environment = bounds;
  else if (control == "settings.show_fps")
   settings_show_fps = bounds;
  else if (control == ERROR_MODAL)
   error_modal = bounds;
  else if (control == ERROR_COPY)
   error_copy = bounds;
  else if (control == ERROR_DISMISS)
   error_dismiss = bounds;
  else if (control == BENCHMARK_OVERRIDE)
   benchmark_override = bounds;
  else if (control == ANNOTATION_SIDEBAR)
   annotation_sidebar = bounds;
  else if (control == ANNOTATION_TIMELINE)
   annotation_timeline = bounds;
  else if (control == ANNOTATION_OPERATION)
   annotation_operation = bounds;
  else if (control == ANNOTATION_STOP)
   annotation_stop = bounds;
  else if (control == ANNOTATION_BRUSH_RADIUS)
   annotation_brush = bounds;
  else if (control.starts_with("annotation.tool."))
   annotation_tool_control = bounds;
  for (std::size_t index = 0; index < SETTINGS_NUMERIC_CONTROLS.size(); ++index) {
   if (control == SETTINGS_NUMERIC_CONTROLS[index])
    settings_numeric_controls[index] = bounds;
   else if (control == std::string{SETTINGS_NUMERIC_CONTROLS[index]} + ".label")
    settings_numeric_labels[index] = bounds;
   else if (control == std::string{SETTINGS_NUMERIC_CONTROLS[index]} + ".value")
    settings_numeric_values[index] = bounds;
  }
 } else if (event == "integration.advanced_field") {
  const Bounds bounds{
   .x = numeric(record, "a"),
   .y = numeric(record, "b"),
   .width = numeric(record, "c"),
   .height = numeric(record, "d"),
  };
  const std::string field_detail = record.value("detail", "");
  const auto indexed = [&field_detail, &bounds](const std::string_view prefix, auto& output) {
   if (!field_detail.starts_with(prefix)) return false;
   std::size_t index = 0U;
   const std::string_view suffix{field_detail.data() + prefix.size(), field_detail.size() - prefix.size()};
   const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
   if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() || index >= output.size()) return false;
   output[index] = bounds;
   return true;
  };
  if (field_detail == "container")
   advanced_container = bounds;
  else if (field_detail == "assignment")
   advanced_assignment = bounds;
  else if (field_detail == "dn-toggle")
   advanced_denoising_toggle = bounds;
  else if (!indexed("fixed-", advanced_fixed) && !indexed("match-free-", advanced_match_free))
   static_cast<void>(indexed("dn-", advanced_denoising));
 } else if (event == "integration.page_region") {
  const std::string control = record.value("control", "");
  const std::string page = record.value("detail", "");
  const Bounds bounds{
   .x = numeric(record, "a"),
   .y = numeric(record, "b"),
   .width = numeric(record, "c"),
   .height = numeric(record, "d"),
  };
  bounds_valid = bounds_valid && !control.empty() && !page.empty() && bounds.valid();
  const std::string identity = page + ":" + control;
  page_bounds.insert_or_assign(identity, bounds);
  if (page == "Train") {
   if (control == "workflow.setup")
    train_setup = bounds;
   else if (control == "workflow.workspace_and_advanced")
    train_center = bounds;
   else if (control == "workflow.workspace")
    train_workspace = bounds;
   else if (control == "workflow.advanced")
    train_advanced = bounds;
   else if (control == "workflow.diagnostics")
    train_diagnostics = bounds;
  }
 } else if (event == "integration.fluent_shell") {
  fluent = record.value("control", "") == "navigation" && (record.value("detail", "") == "light" || record.value("detail", "") == "dark") && numeric(record, "a") > 0.0 && numeric(record, "b") > 0.0 &&
           numeric(record, "c") > 0.0 && numeric(record, "d") == 1.0;
 } else if (event == "integration.primary_action.pixels") {
  const std::string control = record.value("control", "");
  const std::string label = record.value("label", "");
  const bool active = record.value("active", false);
  const auto labels = [&]() -> std::pair<std::string_view, std::string_view> {
   if (control == "train.primary") return {"Start Training", "Stop Training"};
   if (control == "validate.primary") return {"Start Validation", "Stop Validation"};
   if (control == "predict.primary") return {"Run Predict", "Stop Predict"};
   if (control == "export.primary") return {"Run Export", "Stop Export"};
   if (control == "live.primary") return {"Start Live", "Stop Live"};
   if (control == "annotation.save") return {"Save Annotations", "Save Annotations"};
   return {};
  }();
  if (labels.first.empty() || label != (active ? labels.second : labels.first) || std::abs(record.value("height", 0.0) - 46.0) > 0.01 || record.value("segments", -1) != (active ? 10 : 0) ||
      record.value("band_leaks", -1) != 0) {
   failed = true;
  } else {
   if (!active)
    primary_idle_labels.insert(label);
   else {
    const bool dark = record.value("dark", false);
    primary_active_themes.insert(dark);
    const std::string identity = control + (dark ? ":dark" : ":light");
    const double phase = record.value("phase", -1.0);
    if (const auto found = primary_phases.find(identity); found != primary_phases.end()) {
     const double advance = std::fmod(phase - found->second + 1.0, 1.0);
     if (advance >= 0.02 && advance < 0.25) primary_phase_progress.insert(identity);
    }
    primary_phases.insert_or_assign(identity, phase);
   }
  }
 } else if (event == "integration.rendered_style") {
  if (record.value("detail", "") == "shared-primary" && numeric(record, "d") == 1.0 && (numeric(record, "a") > 0.0 || numeric(record, "b") > 0.0 || numeric(record, "c") > 0.0)) {
   shared_primary.insert(record.value("control", ""));
   shared_primary_colors.insert_or_assign(record.value("control", ""), std::array{numeric(record, "a"), numeric(record, "b"), numeric(record, "c"), numeric(record, "d")});
   rendered_style_keys.insert_or_assign(record.value("control", ""), scalar(record, "render_key"));
  }
  benchmark_purple =
   benchmark_purple || (record.value("control", "") == BENCHMARK_OVERRIDE && record.value("detail", "") == "benchmark-purple" && std::abs(numeric(record, "a") - 138.0 / 255.0) < 0.001 &&
                        std::abs(numeric(record, "b") - 43.0 / 255.0) < 0.001 && std::abs(numeric(record, "c") - 226.0 / 255.0) < 0.001 && numeric(record, "d") == 1.0);
  if (record.value("detail", "") == "benchmark-purple") rendered_style_keys.insert_or_assign(record.value("control", ""), scalar(record, "render_key"));
 } else if (event == "integration.rendered_control") {
  const std::string control = record.value("control", "");
  const auto key = scalar(record, "a");
  const double scale = numeric(record, "d");
  bounds_valid = bounds_valid && !control.empty() && key != 0U && numeric(record, "b") > 0.0 && numeric(record, "c") > 0.0 && scale > 0.0 && rendered_style_keys.contains(control) &&
                 rendered_style_keys.at(control) == key && (!rendered_control_keys.contains(control) || rendered_control_keys.at(control) == key);
  if (!control.empty() && key != 0U) {
   rendered_controls.insert(control);
   rendered_control_keys.insert_or_assign(control, key);
   rendered_control_scales.insert_or_assign(control, scale);
  }
 } else if (event == "integration.model_copy") {
  model_copy = record.value("control", "") == TRAIN_MODEL_CARD && record.value("detail", "") == "RF-DETR Weights" && numeric(record, "a") == 1.0;
 } else if (event == "integration.explore_integer") {
  if (scalar(record, "b") > scalar(record, "a") && scalar(record, "c") == 1U && scalar(record, "d") == 1U) {
   const auto control = record.value("control", "");
   explore_integer_controls.insert(control);
   const auto target = scalar(record, "detail");
   if (target == (std::uint64_t{1} << 53U) + 1U || target == (std::uint64_t{1} << 53U) + 3U) {
    explore_integer_precision = true;
    explore_seed_control = control;
    explore_seed_target = target;
   }
  }
 } else if (event == "integration.explore_integer_paste_baseline") {
  if (!explore_seed_control.empty() && record.value("control", "") == explore_seed_control && scalar(record, "detail") != explore_seed_target && scalar(record, "a") != 0U &&
      scalar(record, "c") == 1U && scalar(record, "d") == 1U)
   explore_paste_baseline = record;
 } else if (event == "integration.explore_integer_paste") {
  if (explore_paste_baseline && record.value("control", "") == explore_seed_control && scalar(record, "detail") == explore_seed_target && scalar(record, "a") == scalar(*explore_paste_baseline, "a") &&
      scalar(record, "b") > scalar(record, "a") && scalar(record, "c") == 1U && scalar(record, "d") == 1U)
   explore_paste_value = record;
 } else if (event == "integration.explore_integer_paste_restored") {
  explore_paste_restored = explore_paste_baseline && explore_paste_value && record.value("control", "") == explore_seed_control &&
                           scalar(record, "detail") == scalar(*explore_paste_baseline, "detail") && scalar(record, "a") == scalar(*explore_paste_value, "b") &&
                           scalar(record, "b") > scalar(record, "a") && scalar(record, "c") == 1U && scalar(record, "d") == 1U;
 } else if (event == "integration.atlas_resize_measured") {
  if (record.value("detail", "") == "detail-layout" && numeric(record, "b") > 0.0 && numeric(record, "c") > 0.0 && scalar(record, "d") == 1U) detail_resize_measurements.insert(scalar(record, "a"));
 } else if (event == "integration.atlas_resize") {
  if (numeric(record, "a") > 0.0 && numeric(record, "b") > 0.0 && scalar(record, "c") > 0U && scalar(record, "d") >= scalar(record, "c")) measured_resize_returns.insert(record.value("detail", ""));
 } else if (event == "integration.spinnerless") {
  const bool valid = numeric(record, "b") == 1.0 && numeric(record, "c") == 1.0 && numeric(record, "d") == 1.0;
  if (record.value("detail", "") == "integer-upper-lower-edges")
   spinnerless_integer = valid;
  else if (record.value("detail", "") == "floating-upper-lower-edges")
   spinnerless_floating = valid;
 } else if (event == "integration.advanced_edit") {
  const bool persisted = numeric(record, "c") > numeric(record, "b") && numeric(record, "d") == 1.0;
  if (record.value("detail", "") == "integer")
   advanced_integer_persisted = persisted;
  else if (record.value("detail", "") == "floating")
   advanced_floating_persisted = persisted;
 } else if (event == "integration.show_fps") {
  show_fps_round_trip = record.value("control", "") == "settings.show_fps" && record.value("detail", "") == "round-trip" && numeric(record, "a") == 1.0 && numeric(record, "b") == 1.0 &&
                        numeric(record, "d") > numeric(record, "c");
 } else if (event == "integration.workspace_fps") {
  auto label = textual(record, "detail");
  double displayed = 0.0;
  bool valid_text = false;
  if (label.ends_with(" FPS")) {
   label.remove_suffix(4U);
   const auto parsed = std::from_chars(label.data(), label.data() + label.size(), displayed);
   valid_text = parsed.ec == std::errc{} && parsed.ptr == label.data() + label.size();
  }
  workspace_fps_text |= record.value("control", "") == kExploreGalleryControl && valid_text && numeric(record, "a") > 0.0 && numeric(record, "b") >= 0.5 &&
                        std::abs(displayed - numeric(record, "a") / numeric(record, "b")) <= 0.5 && std::abs(numeric(record, "c") - 6.0) < 0.01 && std::abs(numeric(record, "d") - 6.0) < 0.01;
 } else if (event == "integration.workspace_fps_pixels") {
  workspace_fps_pixels |= record.value("control", "") == kExploreGalleryControl && record.value("detail", "") == "visible-counter" && numeric(record, "a") > 0.0 && numeric(record, "b") >= 0.5 &&
                          numeric(record, "c") >= 12.0 && numeric(record, "d") > 0.0;
 } else if (event == "integration.perceptual_controls") {
  perceptual_controls_round_trip = true;
 } else if (event == "integration.benchmark_inactive") {
  benchmark_inactive =
   record.value("control", "") == DATASET_BROWSE && record.value("detail", "") == "unchanged" && numeric(record, "a") == 7.0 && numeric(record, "b") == 1.0 && numeric(record, "c") > 0.0;
 } else if (event == "integration.benchmark_baseline") {
  if (record.value("detail", "") == "native-settled") benchmark_baseline = std::array{numeric(record, "a"), numeric(record, "b"), numeric(record, "c"), numeric(record, "d")};
 } else if (event == "integration.benchmark_choice") {
  if (record.value("detail", "") == "native-settled")
   benchmark_choices.insert_or_assign(scalar(record, "a"), std::pair{record.value("control", ""), std::array{numeric(record, "a"), numeric(record, "b"), numeric(record, "c"), numeric(record, "d")}});
 } else if (event == "integration.benchmark_click") {
  if (record.value("detail", "") == "real-click" && numeric(record, "b") == 1.0) benchmark_clicks.insert_or_assign(scalar(record, "a"), record.value("control", ""));
 } else if (event == "integration.benchmark_visibility") {
  if (record.value("detail", "") == "current-tree")
   benchmark_visibility.insert_or_assign(
    std::pair{scalar(record, "a"), record.value("control", "")}, std::array{numeric(record, "a"), numeric(record, "b"), numeric(record, "c"), numeric(record, "d")});
 } else if (event == "integration.benchmark_override") {
  benchmark_round_trip = record.value("control", "") == BENCHMARK_OVERRIDE && record.value("detail", "") == "round-trip" && numeric(record, "a") == 1.0 && numeric(record, "b") == 1.0 &&
                         numeric(record, "d") > numeric(record, "c");
 } else if (event == "integration.bootstrap") {
  bootstrap = record.value("detail", "") == "typed-bootstrap";
 } else if (event == "integration.dataset_configured") {
  dataset_configured = record.value("detail", "") == "typed-settings" && record.value("control", "") == COMPILE_RESOLUTION && scalar(record, "a") != 0U && scalar(record, "b") == 1U;
 } else if (event == "integration.compile_progress") {
  compile_completed = scalar(record, "b");
  compile_total = scalar(record, "c");
  compile_dropped = scalar(record, "d");
  progress = record.value("control", "") == COMPILE_PROGRESS && scalar(record, "a") != 0U && !record.value("detail", "").empty() && compile_total != 0U && compile_completed <= compile_total;
  if (progress && progress_ordinal == 0U) progress_ordinal = ordinal;
 } else if (event == "integration.compile_track_text") {
  const auto control = record.value("control", "");
  const auto detail = record.value("detail", "");
  const bool label = control == "Acquisition" || control == "Labels/masks" || control == "Pixels";
  const bool zero_acquisition = control != "Acquisition" || detail == "Acquisition · unnecessary · 0 / 0";
  if (label && zero_acquisition && detail.starts_with(control + " · ") && detail.find(" / ") != std::string::npos && numeric(record, "c") > 0 && numeric(record, "d") > 0)
   compile_tracks.insert(control);
 } else if (event == "integration.compile_metrics") {
  const std::uint64_t elapsed = scalar(record, "a");
  const auto expected = mmltk::backend::data::estimate_progress(compile_completed, compile_total, elapsed);
  compile_metrics = record.value("control", "") == COMPILE_PROGRESS && record.value("detail", "") == "elapsed-eta-throughput-dropped" && progress &&
                    scalar(record, "b") == expected.remaining_seconds && scalar(record, "c") == expected.throughput_per_second && scalar(record, "d") == compile_dropped;
 } else if (event == "integration.dataset_complete") {
  dataset_complete = true;
  dataset_complete_ordinal = ordinal;
  compiled_images = scalar(record, "b");
  compiled_width = scalar(record, "c");
  compiled_height = scalar(record, "d");
 } else if (event == "integration.explore_ready") {
  explore_ready = scalar(record, "a") != 0U && scalar(record, "b") != 0U && scalar(record, "c") != 0U && scalar(record, "d") != 0U;
 } else if (event == "integration.explore_sweep_observed") {
  sweep = record.value("detail", "") == "typed-viewport" && scalar(record, "b") > scalar(record, "a") && scalar(record, "c") != 0U && scalar(record, "d") != 0U;
 } else if (event == "integration.explore_scrolled") {
  scrolled = scalar(record, "b") > scalar(record, "a");
 } else if (event == "integration.explore_detail") {
  detail = scalar(record, "b") != 0U;
 } else if (event == "integration.explore_augmentation") {
  const auto frame_revision = scalar(record, "d");
  const bool valid = scalar(record, "b") > scalar(record, "a") && frame_revision > scalar(record, "c");
  if (record.value("detail", "") == "enabled-rendered-seed-zero") {
   augmentation_enabled = augmentation_enabled || valid;
   if (valid) augmentation_frames.insert_or_assign(0U, frame_revision);
  } else if (record.value("detail", "") == "rerolled-distinct-seed") {
   augmentation_rerolled = augmentation_rerolled || valid;
   if (valid) augmentation_frames.insert_or_assign(1U, frame_revision);
  }
 } else if (event == "integration.explore_reshuffle") {
  reshuffle_order_only = record.value("detail", "") == "order-only" && scalar(record, "b") > scalar(record, "a") && scalar(record, "c") == scalar(record, "d");
 } else if (event == "integration.annotation_pixel" || event == "integration.annotation_swatch" || event == "integration.annotation_capability") {
  const auto expected = record.value("expected", std::vector<double>{});
  const auto observed = record.value("observed", std::vector<double>{});
  double tolerance = event == "integration.annotation_pixel" ? 24.0 : 3.0;
  bool matched = record.value("matched", false) && expected.size() == 3U && observed.size() >= 3U;
  if (matched) {
   double gain = 1.0;
   double minimum = 0.0;
   const double peak = std::max({observed[0], observed[1], observed[2]});
   const double low = std::min({observed[0], observed[1], observed[2]});
   const double scale = event == "integration.annotation_pixel" ? record.value("source_to_screen", 0.0) : 1.0;
   matched = matched && std::isfinite(scale) && scale > 0.0;
   if (event == "integration.annotation_pixel" && scale > 0.0 && scale < 1.0 && std::ranges::max(expected) == 255.0 && std::ranges::min(expected) == 0.0 && peak - low >= 127.5) {
    minimum = low;
    gain = 255.0 / (peak - low);
    tolerance = 48.0;
   }
   double error = 0.0;
   for (std::size_t channel = 0U; channel < 3U; ++channel) {
    matched = matched && std::isfinite(expected[channel]) && std::isfinite(observed[channel]) && expected[channel] >= 0.0 && expected[channel] <= 255.0 && observed[channel] >= 0.0 &&
              observed[channel] <= 255.0;
    error = std::max(error, std::abs(expected[channel] - (observed[channel] - minimum) * gain));
   }
   bool blended = false;
   if (matched && event == "integration.annotation_pixel" && scale < 1.0 && std::ranges::max(expected) != std::ranges::min(expected)) {
    // Independently verify filtered coverage over the pixel-evidence PNG fixture.
    constexpr std::array background{48.0, 80.0, 112.0};
    double dot = 0.0, norm = 0.0;
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
     const double delta = expected[channel] - background[channel];
     dot += (observed[channel] - background[channel]) * delta;
     norm += delta * delta;
    }
    const double coverage = norm > 0.0 ? dot / norm : 0.0;
    blended = coverage >= scale / 2.0 && coverage <= 1.0;
    for (std::size_t channel = 0U; channel < 3U; ++channel) blended = blended && std::abs(observed[channel] - std::lerp(background[channel], expected[channel], coverage)) <= 3.0;
   }
   matched = matched && (error <= tolerance || blended);
  }
  annotation_pixels_valid = annotation_pixels_valid && matched;
  if (event == "integration.annotation_pixel") {
   annotation_pixels_valid = annotation_pixels_valid && scalar(record, "a") != 0U && scalar(record, "b") != 0U;
   if (annotation_pixel_frames.size() < kAcceptanceRecordLimit) annotation_pixel_frames.emplace(scalar(record, "a"), scalar(record, "b"));
  } else if (event == "integration.annotation_swatch")
   ++annotation_swatches;
  else
   annotation_capabilities.insert(record.value("detail", ""));
 } else if (event == "integration.annotation_tail") {
  if (scalar(record, "a") >= 32U && numeric(record, "b") > 0.0 && numeric(record, "c") >= -0.01 && numeric(record, "d") >= -0.01)
   annotation_tails[record.value("detail", "")].emplace(record.value("control", ""), scalar(record, "a"));
 } else if (event == "integration.annotation_reachable") {
  if (numeric(record, "a") >= -1.0 && numeric(record, "b") >= -1.0 && numeric(record, "c") > 0.0 && numeric(record, "d") > 0.0)
   annotation_reachable[record.value("detail", "")].insert(record.value("control", ""));
 } else if (event == "integration.annotation_layout") {
  annotation_layouts.insert(record.value("detail", ""));
  annotation_layout_valid = annotation_layout_valid && numeric(record, "a") > 0.0 && numeric(record, "b") > 0.0 && std::abs(numeric(record, "a") / numeric(record, "c") - 0.62) < 0.002 &&
                            std::abs(numeric(record, "b") / numeric(record, "c") - 0.19) < 0.002 && numeric(record, "c") >= 1020.0 && numeric(record, "c") <= 1500.0 &&
                            (record.value("detail", "") == "narrow" ? numeric(record, "d") < 1020.0 : numeric(record, "d") >= 1020.0);
 } else if (event == "integration.annotation_product") {
  ++annotation_product_operations[record.value("detail", "")];
  annotation_pixels_valid = annotation_pixels_valid && annotation_pixel_frames.contains({scalar(record, "a"), scalar(record, "b")});
 } else if (event == "integration.annotation_preview") {
  if (scalar(record, "b") > scalar(record, "a") && scalar(record, "c") != 0U) ++annotation_previews;
 } else if (event == "integration.annotation_shape") {
  annotation_shapes.insert(record.value("detail", ""));
 } else if (event == "integration.viewer_import_edit") {
  viewer_import_edits.insert(record.value("detail", ""));
 } else if (event == "integration.atlas_scale") {
  const double x = numeric(record, "c"), y = numeric(record, "d");
  if (x <= 0.0 || y <= 0.0 || std::abs(x - y) > 0.0001)
   atlas_geometry_valid = false;
  else if (atlas_scaled_frames.size() < kAcceptanceRecordLimit)
   atlas_scaled_frames.emplace(scalar(record, "a"), scalar(record, "b"));
 } else if (event == "integration.explore_mouse") {
  if (record.value("detail", "") == "shared-input-admitted" && scalar(record, "c") == static_cast<std::uint64_t>(mmltk::controller::WorkspaceMouseKind::Motion)) {
   if (shared_explore_motion.size() < kAcceptanceRecordLimit)
    shared_explore_motion.push_back({numeric(record, "a"), numeric(record, "b"), ordinal});
   else
    bounds_valid = false;
  }
 } else if (event == "integration.pending_hover") {
  pending_hover_index = scalar(record, "a");
  pending_read_generation = scalar(record, "b");
  pending_hover_ordinal = ordinal;
 } else if (event == "integration.pending_selection") {
  pending_selection_index = scalar(record, "a");
  bounds_valid = bounds_valid && pending_read_generation == scalar(record, "b");
 } else if (event == "integration.capacity_retry") {
  capacity_retry_frame = scalar(record, "a");
  capacity_retry_publication = scalar(record, "b");
 } else if (event == "integration.atlas_capacity") {
  atlas_native_capacity = record.value("detail", "") == "native-visible-capacity-exceeded";
 } else if (event == "integration.atlas_geometry") {
  const double width = numeric(record, "c"), height = numeric(record, "d");
  if (width <= 0.0 || height <= 0.0 || std::abs(width - height) > 0.01)
   atlas_geometry_valid = false;
  else if (square_atlas_frames.size() < kAcceptanceRecordLimit)
   square_atlas_frames.emplace(scalar(record, "a"), scalar(record, "b"));
 } else if (event == "integration.atlas_checkbox") {
  atlas_visibility_modes.emplace(scalar(record, "b"));
 } else if (event == "integration.atlas_notice") {
  if (numeric(record, "c") > 0.0 && numeric(record, "d") > 0.0) atlas_notices.emplace(record.value("control", ""));
 } else if (event == "integration.atlas_window") {
  atlas_device_scales.emplace(numeric(record, "c"));
 } else if (event == "integration.atlas_window_draw") {
  if (numeric(record, "b") > 0.0 && numeric(record, "c") > 0.0) atlas_window_draws.emplace(record.value("detail", ""));
 } else if (event == "integration.atlas_visibility") {
  atlas_themes.emplace(record.value("detail", ""));
 } else if (event == "integration.atlas_scroll") {
  atlas_scroll_stages.emplace(record.value("detail", ""));
 } else if (event == "integration.gallery_no_input_complete") {
  gallery_no_input_complete = scalar(record, "a") != 0U && scalar(record, "b") != 0U && scalar(record, "c") != 0U && scalar(record, "d") != 0U;
 } else if (event == "integration.atlas_canvas_pixels") {
  if (scalar(record, "c") != 0U && scalar(record, "c") == scalar(record, "d")) atlas_canvas_pixels.emplace(scalar(record, "a"), scalar(record, "b"));
 } else if (event == "integration.initial_atlas_complete") {
  initial_atlas_complete =
   record.value("detail", "") == "no-input-canvas-pixels" && scalar(record, "a") != 0U && scalar(record, "b") != 0U && atlas_canvas_pixels.contains({scalar(record, "c"), scalar(record, "d")});
 } else if (event == "integration.viewer_complete") {
  viewer_presentation = scalar(record, "a");
  viewer_complete = scalar(record, "a") != 0U && scalar(record, "b") != 0U && scalar(record, "c") == 1U;
 } else if (event == "integration.viewer_overlay") {
  if (scalar(record, "c") != 0U && scalar(record, "d") != 0U) viewer_overlay_modes.insert(scalar(record, "b"));
 } else if (event == "integration.viewer_label_rgb") {
  if (viewer_label_colors.size() < 256U || viewer_label_colors.contains(scalar(record, "a")))
   viewer_label_colors.insert_or_assign(scalar(record, "a"), std::array{numeric(record, "b"), numeric(record, "c"), numeric(record, "d")});
 } else if (event == "integration.viewer_label_frame") {
  if (record.value("detail", "") == "exact-scene-product" && scalar(record, "d") == 0U && scalar(record, "b") != 0U) viewer_label_products.emplace(scalar(record, "c"), scalar(record, "a"));
 } else if (event == "integration.viewer_label_catalog") {
  const auto found = viewer_label_colors.find(scalar(record, "a"));
  if (found != viewer_label_colors.end() && scalar(record, "b") != 0U) {
   std::uint8_t red = 0U, green = 0U, blue = 0U;
   mmltk::backend::imaging::raster::color::class_color(static_cast<int>(scalar(record, "a")), static_cast<int>(scalar(record, "b")), red, green, blue);
   const std::array expected{red, green, blue};
   for (std::size_t channel = 0U; channel != expected.size(); ++channel) viewer_class_colors = viewer_class_colors && std::abs(found->second[channel] - expected[channel] / 255.0) <= 1.0 / 255.0;
   viewer_labels_without_boxes = viewer_labels_without_boxes || scalar(record, "c") == 0U;
  }
 } else if (event == "integration.explore_detail_source") {
  detail_source = record.value("detail", "") == "padded-to-original-sampling" && scalar(record, "a") == scalar(record, "c") && scalar(record, "b") >= scalar(record, "d") &&
                  scalar(record, "c") != 0U && scalar(record, "d") != 0U;
 } else if (event == "integration.explore_detail_fit") {
  detail_fit = record.value("detail", "") == "centered-contained" && numeric(record, "c") <= numeric(record, "a") + 1.0 && numeric(record, "d") <= numeric(record, "b") + 1.0 &&
               (std::abs(numeric(record, "c") - numeric(record, "a")) < 1.0 || std::abs(numeric(record, "d") - numeric(record, "b")) < 1.0);
 } else if (event == "integration.viewer_sample" && numeric(record, "d") > 0.0) {
  original_detail_drawn = original_detail_drawn || (scalar(record, "a") == 512U && scalar(record, "b") == 256U && std::abs(numeric(record, "c") / numeric(record, "d") - 2.0) < 0.01);
 } else if (event == "integration.explore_reopened") {
  reopened = record.value("detail", "") == "usable-after-reopen" && scalar(record, "a") != 0U && scalar(record, "b") != 0U && scalar(record, "c") != 0U && scalar(record, "d") != 0U;
 } else if (event == "integration.ui_scale_drag") {
  if (record.value("detail", "") == "second-position")
   ui_scale_drag = numeric(record, "a") == numeric(record, "c") && numeric(record, "b") > 0.85 && numeric(record, "a") != numeric(record, "b") && numeric(record, "d") == 2.0;
  else if (record.value("detail", "") == "released-and-settled")
   ui_scale_released = numeric(record, "b") > 0.85 && numeric(record, "b") == numeric(record, "c") && numeric(record, "d") != 0.0;
 } else if (event == "integration.ui_scale_pointer") {
  const std::string stage = record.value("detail", "");
  const bool pressed = stage != "released";
  if (record.value("control", "") == "settings.ui_scale" && numeric(record, "b") == (pressed ? 1.0 : 0.0) && numeric(record, "c") > 0.0 && numeric(record, "d") > 0.0)
   ui_scale_pointer_stages.insert(stage);
 } else if (event == "integration.ui_scale_restored") {
  ui_scale_restored = record.value("control", "") == "settings.ui_scale" && record.value("detail", "") == "baseline" && numeric(record, "a") == numeric(record, "b") &&
                      numeric(record, "b") == numeric(record, "c") && numeric(record, "d") != 0.0;
 } else if (event == "integration.error_modal") {
  error_modal_usable = record.value("detail", "") == "copy-and-dismiss" && numeric(record, "a") == 1.0 && numeric(record, "b") == 1.0 && numeric(record, "c") == 1.0;
 } else if (event == "integration.explore_exact_grid") {
  const auto columns = scalar(record, "c");
  const auto rows = scalar(record, "d");
  exact_grid = record.value("detail", "") == "oversized-logical-fill" && columns != 0U && rows != 0U && scalar(record, "a") % columns == 0U && scalar(record, "b") % rows == 0U &&
               scalar(record, "a") / columns == scalar(record, "b") / rows;
  exact_grid_width = scalar(record, "a");
  exact_grid_height = scalar(record, "b");
 } else if (event == "integration.explore_exact_grid_capacity") {
  exact_grid_revision = scalar(record, "a");
  exact_grid_frame_revision = scalar(record, "b");
  exact_grid_capacity_width = scalar(record, "c");
  exact_grid_capacity_height = scalar(record, "d");
 } else if (event == "integration.explore_scroll_placeholder") {
  newest_placeholder = record.value("detail", "") == "newest-without-wait-all" && scalar(record, "a") != 0U && scalar(record, "b") != 0U && scalar(record, "c") != 0U && scalar(record, "d") != 0U;
 } else if (event == "integration.explore_final_cursor") {
  final_cursor_revision = scalar(record, "a");
  final_cursor_frame_revision = scalar(record, "b");
  final_cursor_generation = scalar(record, "c");
  final_cursor_slot_count = scalar(record, "d");
 } else if (event == "integration.explore_pointer_inverse") {
  pointer_inverse = record.value("detail", "") == "rendered-grid-slot" && scalar(record, "a") == scalar(record, "b") && scalar(record, "d") != 0U;
  pointer_slot = scalar(record, "a");
  pointer_compiled_index = scalar(record, "c");
  pointer_revision = scalar(record, "d");
 } else if (event == "integration.explore_pointer_scheduled") {
  pending_pointer_frame_revision = scalar(record, "a");
 } else if (event == "integration.surface_click_dispatched" && record.value("control", "") == kExploreGalleryControl) {
  const auto dispatched_frame_revision = scalar(record, "a");
  const bool dispatched = record.value("detail", "") == "real-canvas-pointer" && pending_pointer_frame_revision != 0U && dispatched_frame_revision >= pending_pointer_frame_revision &&
                          surface_draws.contains(scalar(record, "b")) && surface_scales.contains(std::pair{scalar(record, "b"), dispatched_frame_revision}) && numeric(record, "c") > 0.0 &&
                          numeric(record, "d") > 0.0;
  if (dispatched) {
   pointer_dispatched = true;
   pending_pointer_frame_revision = dispatched_frame_revision;
  }
 } else if (event == "integration.explore_pointer_selected") {
  const bool selected = record.value("detail", "") == "selected-from-dispatched-pointer" && scalar(record, "a") == pointer_revision && scalar(record, "b") == pointer_slot &&
                        scalar(record, "c") == pointer_compiled_index && scalar(record, "c") == scalar(record, "d") && explore_slots.contains(pointer_revision) &&
                        explore_slots.at(pointer_revision).contains(pointer_slot) && explore_slots.at(pointer_revision).at(pointer_slot) == pointer_compiled_index &&
                        observed_frame_revision_for_slots(explore_slots.at(pointer_revision), pending_pointer_frame_revision);
  if (selected) {
   pointer_selected = true;
   pointer_frame_revision = pending_pointer_frame_revision;
  }
 } else if (event == "integration.explore_slot") {
  const auto revision = scalar(record, "a");
  const auto frame_revision = scalar(record, "b");
  const auto slot = scalar(record, "c");
  const auto compiled_index = scalar(record, "d");
  if (revision != 0U && frame_revision != 0U) {
   if (!explore_slots.contains(revision) && explore_slots.size() == kAcceptanceSnapshotLimit) {
    bounds_valid = false;
    return;
   }
   auto& slots = explore_slots[revision];
   bounds_valid = bounds_valid && revision < (1ULL << 53U) && slot < mmltk::controller::kExploreVisibleItemCapacity && (!slots.contains(slot) || slots.at(slot) == compiled_index) &&
                  (!explore_slot_frames.contains(revision) || explore_slot_frames.at(revision) == frame_revision);
   if (slot < mmltk::controller::kExploreVisibleItemCapacity) slots.insert_or_assign(slot, compiled_index);
   explore_slot_frames.insert_or_assign(revision, frame_revision);
   atlas_identities = true;
  }
 } else if (event == "integration.upscale_growth") {
  const bool complete_growth = record.value("detail", "").ends_with("-four-times") && scalar(record, "c") == scalar(record, "a") * 4U && scalar(record, "d") == scalar(record, "b") * 4U &&
                               (scalar(record, "c") > 1500U || scalar(record, "d") > 1125U);
  upscale_growth = upscale_growth || complete_growth;
  if (complete_growth) upscale_modes.insert(record.value("detail", ""));
 } else if (event == "integration.upscale_presentation") {
  const bool complete_presentation = record.value("detail", "") == "complete-four-times-exported-frame" && scalar(record, "a") == scalar(record, "c") && scalar(record, "b") == scalar(record, "d") &&
                                     scalar(record, "a") > 1500U && scalar(record, "b") > 0U;
  upscale_presentation = upscale_presentation || complete_presentation;
  if (complete_presentation) upscale_presentations.insert(record.value("control", ""));
 } else if (event == "integration.upscale_completed_pixels") {
  if (record.value("detail", "") == "exact-completed-blue" && scalar(record, "a") > 0U && scalar(record, "b") > 0U && scalar(record, "c") > 0U && scalar(record, "d") >= 32U)
   upscale_completed_pixels.insert(record.value("control", ""));
 } else if (event == "integration.upscale_same_method") {
  if (record.value("detail", "") == "same-completed-result" && scalar(record, "a") > 0U && scalar(record, "b") > 0U && scalar(record, "c") > 0U && scalar(record, "d") == 1U)
   upscale_same_method.insert(record.value("control", ""));
 } else if (event == "integration.upscale_later_frame") {
  upscale_later_frame = record.value("detail", "") == "distinct-imported-frame" && scalar(record, "b") > scalar(record, "a") && scalar(record, "c") != 0U && scalar(record, "d") != 0U;
 } else if (event == "integration.viewer_navigation") {
  const auto control = record.value("control", "");
  if (record.value("detail", "") == "automatic-basic-completed-draw" && scalar(record, "a") != scalar(record, "b") && scalar(record, "c") != 0U && scalar(record, "d") != 0U &&
      (control == EXPLORE_NEXT || control == EXPLORE_PREVIOUS))
   viewer_navigation_draws[control == EXPLORE_NEXT ? 0U : 1U] = scalar(record, "d");
 } else if (event == "integration.annotation_ready") {
  annotation_ready = record.value("detail", "") == "receiver-owned" && scalar(record, "a") != 0U;
 } else if (event == "integration.annotation_tool_observed") {
  annotation_tool = record.value("detail", "") == "typed-tool" && record.value("control", "").starts_with("annotation.tool.") && scalar(record, "b") > scalar(record, "a");
 } else if (event == "integration.annotation_pointer_observed") {
  annotation_pointer = record.value("detail", "") == "typed-interaction" && scalar(record, "b") > scalar(record, "a") && scalar(record, "c") != 0U;
 } else if (event == "integration.surface_draw") {
  const std::uint64_t revision = scalar(record, "a");
  if (revision != 0U && record.value("detail", "") == "draw") surface_draws.insert(revision);
  if (revision != 0U && record.value("detail", "") == "redraw") surface_redraws.insert(revision);
 } else if (event == "integration.surface_draw_ordinal") {
  const std::uint64_t revision = scalar(record, "a");
  if (revision != 0U && scalar(record, "b") != 0U && numeric(record, "d") == 1.0) ++surface_redraw_counts[revision];
 } else if (event == "integration.surface_geometry") {
  if (record.value("detail", "") == "shader-viewport" && surface_geometries.size() < kAcceptanceRecordLimit)
   surface_geometries.push_back({
    .presentation_revision = scalar(record, "a"),
    .source_revision = scalar(record, "b"),
    .width = numeric(record, "c"),
    .height = numeric(record, "d"),
   });
  else if (record.value("detail", "") == "shader-viewport")
   bounds_valid = false;
 } else if (event == "integration.surface_container") {
  if (record.value("detail", "") == "rendered-contain-container" && surface_containers.size() < kAcceptanceRecordLimit)
   surface_containers.push_back({
    .presentation_revision = scalar(record, "a"),
    .source_revision = scalar(record, "b"),
    .width = numeric(record, "c"),
    .height = numeric(record, "d"),
    .control = record.value("control", ""),
   });
  else if (record.value("detail", "") == "rendered-contain-container")
   bounds_valid = false;
 } else if (event == "integration.surface_content") {
  const auto key = std::pair{scalar(record, "a"), scalar(record, "b")};
  if (record.value("detail", "") == "exported-native-frame" && (surface_contents.contains(key) || surface_contents.size() < kAcceptanceRecordLimit))
   surface_contents.insert_or_assign(key, std::pair{scalar(record, "c"), scalar(record, "d")});
  else if (record.value("detail", "") == "exported-native-frame")
   bounds_valid = false;
 } else if (event == "integration.surface_scale") {
  const auto revision = scalar(record, "a");
  const auto source = scalar(record, "b");
  const double scale = numeric(record, "c");
  bounds_valid = bounds_valid && revision != 0U && source != 0U && scale > 0.0 && std::abs(scale - numeric(record, "d")) < 0.0001;
  if (revision != 0U && source != 0U && scale > 0.0) surface_scales.insert_or_assign({revision, source}, scale);
 } else if (event == "integration.complete") {
  const std::uint64_t annotation_revision = scalar(record, "a");
  const std::uint64_t exported_receipt = scalar(record, "b");
  const std::uint64_t frame_receipt = scalar(record, "c");
  const std::uint64_t browser_receipt = scalar(record, "d");
  complete = record.value("detail", "") == "typed-mvc-wayland" && annotation_revision != 0U && exported_receipt != 0U && exported_receipt == frame_receipt && frame_receipt == browser_receipt;
  presentation_receipt = exported_receipt;
 } else if (event == "integration.failed" || event == "browser.invalid_webgpu_texture") {
  failed = true;
 } else if (event == "firefox.workspace.admitted") {
  firefox_import = true;
 } else if (event == "firefox.workspace.claim_outcome") {
  firefox_claim = record.value("outcome", "") == "claimed";
 } else if (event == "firefox.workspace.ready" || event == "firefox.workspace.import_ready_emitted" || event == "firefox.workspace.registry_inserted") {
  firefox_ready = true;
 } else if (event == "firefox.workspace.channel_terminal" && record.value("terminal", "") == "protocol_failure") {
  workspace_protocol_failure = true;
 } else if (event.find("panic") != std::string::npos || event == "browser.panic") {
  panic = true;
 }
}
auto BrowserAudit::readiness_blocker() const -> std::string_view {
 static const std::array expected{
  "navigation.train",
  "navigation.validate",
  "navigation.predict",
  "navigation.live",
  "navigation.annotate",
  "navigation.export",
  "navigation.explore",
  TRAIN_CARD,
  DATASET_SOURCE,
  COMPILED_DIRECTORY,
  COMPILE_DIMENSIONS,
  COMPILE_RESOLUTION,
  COMPILE_DATASET,
  COMPILE_PROGRESS,
  DATASET_STATUS,
  TRAIN_MODEL_CARD,
  TRAIN_MODEL_PROGRESS,
  TRAIN_MODEL_SELECTOR,
  TRAIN_MODEL_PRESETS,
  TRAIN_MODEL_DIVIDER,
  TRAIN_MODEL_CUSTOM,
  TRAIN_MODEL_STATUS,
  TRAIN_MODEL_ACTION,
  MATCH_FREE_ASSIGNMENT,
  "workflow.setup",
  "workflow.workspace_and_advanced",
  "workflow.workspace",
  "workflow.advanced",
  "workflow.diagnostics",
  EXPLORE_OPEN,
  EXPLORE_DATASET_PANE,
  EXPLORE_DETAILS_PANE,
  EXPLORE_CARD,
  kExploreGalleryControl,
  EXPLORE_LATER,
  EXPLORE_AUGMENTATION_TOGGLE,
  EXPLORE_AUGMENTATION_REROLL,
  EXPLORE_RESHUFFLE,
  EXPLORE_DETAIL_ORIGINAL,
  EXPLORE_DETAIL_FIT,
  EXPLORE_UPSCALE_BASIC,
  EXPLORE_UPSCALE_FAST,
  EXPLORE_UPSCALE_NEURAL,
  EXPLORE_NEXT,
  EXPLORE_PREVIOUS,
  EXPLORE_ANNOTATE,
  ANNOTATION_SURFACE,
  ANNOTATION_SIDEBAR,
  ANNOTATION_TIMELINE,
  ANNOTATION_OPERATION,
  ANNOTATION_STOP,
  ANNOTATION_BRUSH_RADIUS,
  SETTINGS_MODAL,
  ERROR_MODAL,
  ERROR_COPY,
  ERROR_DISMISS,
  "settings.group.appearance",
  "settings.group.typography",
  "settings.group.environment",
  "settings.show_fps",
  BENCHMARK_OVERRIDE,
  "settings.ui_scale",
  "settings.ui_scale.label",
  "settings.ui_scale.value",
  "settings.font_size",
  "settings.font_size.label",
  "settings.font_size.value",
  "settings.secondary_font_size",
  "settings.secondary_font_size.label",
  "settings.secondary_font_size.value",
  "settings.mono_font_size",
  "settings.mono_font_size.label",
  "settings.mono_font_size.value",
  "settings.text_input_font_size",
  "settings.text_input_font_size.label",
  "settings.text_input_font_size.value",
  SETTINGS_FOOTER,
  SETTINGS_RESET,
  SETTINGS_CLOSE,
 };
 static const std::array pages{"Train", "Validate", "Predict", "Export", "Live", "Annotate"};
 static const std::array regions{"workflow.setup", "workflow.workspace_and_advanced", "workflow.workspace", "workflow.advanced", "workflow.diagnostics"};
 const auto page_bound = [this](const std::string_view page, const std::string_view control) -> const Bounds* {
  const auto found = page_bounds.find(std::string{page} + ":" + std::string{control});
  return found == page_bounds.end() ? nullptr : &found->second;
 };
 const auto immediately_above = [](const Bounds& progress_bounds, const Bounds& action_bounds) {
  const double progress_bottom = progress_bounds.y + progress_bounds.height;
  return progress_bounds.valid() && action_bounds.valid() && progress_bottom <= action_bounds.y + 1.0 && action_bounds.y - progress_bottom <= 5.0;
 };
 const auto page_prefix = [](const std::string_view page) {
  return page == "Train" ? "train" : page == "Validate" ? "validate" : page == "Predict" ? "predict" : page == "Live" ? "live" : page == "Annotate" ? "annotation" : "export";
 };
 const auto primary_action = [&page_prefix](const std::string_view page) { return std::string{page_prefix(page)} + (page == "Annotate" ? ".save" : ".primary"); };
 const bool every_region = std::ranges::all_of(pages, [this, &page_prefix, &primary_action](const std::string_view page) {
  const std::string_view prefix = page_prefix(page);
  return std::ranges::all_of(regions, [this, page](const std::string_view region) { return page_bounds.contains(std::string{page} + ":" + std::string{region}); }) &&
         page_bounds.contains(std::string{page} + ":" + primary_action(page)) && page_bounds.contains(std::string{page} + ":" + prefix + ".status");
 });
 const bool primary_progress_placement = std::ranges::all_of(pages, [&page_bound, &immediately_above, &primary_action](const std::string_view page) {
  const std::string action = primary_action(page);
  const Bounds* const progress_bounds = page_bound(page, action + ".progress");
  const Bounds* const action_bounds = page_bound(page, action);
  return progress_bounds != nullptr && action_bounds != nullptr && immediately_above(*progress_bounds, *action_bounds);
 });
 const bool primary_action_geometry = std::ranges::all_of(pages, [&page_bound, &primary_action](const std::string_view page) {
  const std::string action = primary_action(page);
  const Bounds* const action_bounds = page_bound(page, action);
  const Bounds* const setup_bounds = page_bound(page, "workflow.setup");
  return action_bounds != nullptr && setup_bounds != nullptr && std::abs(action_bounds->height - 48.0) < 0.01 && std::abs(action_bounds->width - (setup_bounds->width - 20.0)) < 1.0;
 });
 const bool primary_card_gaps = [&] {
  const auto gap = [&page_bound, &primary_action](const std::string_view page, const std::string_view card) -> std::optional<double> {
   const Bounds* const above = page_bound(page, card);
   const Bounds* const action = page_bound(page, primary_action(page));
   if (!above || !action) return std::nullopt;
   return action->y - above->y - above->height;
  };
  const auto reference = gap("Export", "export.card.output");
  if (!reference || *reference <= 0.0) return false;
  for (const auto& [page, card] : std::array{std::pair{"Train", "train.card.dataset"}, std::pair{"Validate", "validate.card.inputs"}, std::pair{"Predict", "predict.card.inputs"}}) {
   const auto measured = gap(page, card);
   if (!measured || std::abs(*measured - *reference) > 1.0) return false;
  }
  return true;
 }();
 const bool compile_progress_placement = immediately_above(compile_progress, compile_action);
 const bool model_progress_placement = model_card.valid() && model_progress.valid() && model_card.contains(model_progress);
 const bool model_composition = [&] {
  if (!std::ranges::all_of(model_parts, [](const Bounds& bounds) { return bounds.valid(); })) return false;
  const auto& [selector, presets, divider, custom, status, action] = model_parts;
  return std::ranges::all_of(model_parts, [this](const Bounds& part) { return model_card.contains(part); }) && model_card.contains(model_progress) && selector.contains(presets) &&
         selector.contains(divider) && selector.contains(custom) && std::abs(custom.x - (selector.x + 2.0)) < 1.0 && std::abs(custom.width - (selector.width - 4.0)) < 1.0 &&
         divider.y >= presets.y + presets.height - 1.0 && custom.y >= divider.y + divider.height - 1.0 && status.y >= selector.y + selector.height - 1.0 &&
         model_progress.y >= status.y + status.height - 1.0 && action.y >= model_progress.y + model_progress.height - 1.0;
 }();
 const double page_width = train_setup.width + train_center.width + train_diagnostics.width;
 const bool reference_columns = train_setup.valid() && train_center.valid() && train_diagnostics.valid() && train_setup.x < train_center.x && train_center.x < train_diagnostics.x &&
                                page_width > 0.0 && std::abs(train_setup.width / page_width - 0.19) < 0.002 && std::abs(train_center.width / page_width - 0.62) < 0.002 &&
                                std::abs(train_diagnostics.width / page_width - 0.19) < 0.002;
 const bool vertical_composition = train_workspace.valid() && train_advanced.valid() && train_workspace.y + train_workspace.height <= train_advanced.y + 1.0;
 const bool explore_composition = explore_dataset.valid() && explore_gallery.valid() && explore_details.valid() && explore_dataset.x < explore_gallery.x && explore_gallery.x < explore_details.x &&
                                  std::abs(explore_dataset.width - 280.0) < 1.0 && std::abs(explore_details.width - 300.0) < 1.0;
 const Bounds* const annotation_workspace = page_bound("Annotate", "workflow.workspace");
 const Bounds* const annotation_diagnostics = page_bound("Annotate", "workflow.diagnostics");
 const Bounds* const annotation_setup = page_bound("Annotate", "workflow.setup");
 const Bounds* const annotation_center = page_bound("Annotate", "workflow.workspace_and_advanced");
 const Bounds* const annotation_advanced = page_bound("Annotate", "workflow.advanced");
 const Bounds* const annotation_save = page_bound("Annotate", "annotation.save");
 const bool annotation_composition =
  annotation_workspace != nullptr && annotation_diagnostics != nullptr && annotation_setup != nullptr && annotation_center != nullptr && annotation_advanced != nullptr && annotation_save != nullptr &&
  annotation_sidebar.valid() && annotation_timeline.valid() && annotation_operation.valid() && annotation_stop.valid() && annotation_brush.valid() && annotation_tool_control.valid() &&
  annotation_diagnostics->contains_horizontally(annotation_sidebar) && annotation_diagnostics->contains_horizontally(annotation_operation) &&
  annotation_diagnostics->contains_horizontally(annotation_stop) && annotation_diagnostics->contains_horizontally(annotation_brush) &&
  annotation_diagnostics->contains_horizontally(annotation_tool_control) && annotation_center->contains_horizontally(annotation_timeline) && annotation_setup->contains(*annotation_save) &&
  annotation_advanced->y >= annotation_workspace->y + annotation_workspace->height - 1.0 && std::abs(annotation_setup->width / page_width - 0.19) < 0.002 &&
  std::abs(annotation_center->width / page_width - 0.62) < 0.002 && std::abs(annotation_diagnostics->width / page_width - 0.19) < 0.002 &&
  std::abs(annotation_center->x - annotation_setup->x - annotation_setup->width) < 1.0 && std::abs(annotation_diagnostics->x - annotation_center->x - annotation_center->width) < 1.0;
 const bool settings_composition = settings_modal.valid() && std::abs(settings_modal.width - 520.0) < 0.01 && settings_footer.valid() && settings_appearance.valid() && settings_typography.valid() &&
                                   settings_show_fps.valid() && settings_environment.valid() && settings_appearance.y < settings_typography.y && settings_show_fps.y >= settings_appearance.y - 1.0 &&
                                   settings_show_fps.y + settings_show_fps.height <= settings_appearance.y + settings_appearance.height + 1.0 && settings_typography.y < settings_environment.y &&
                                   settings_environment.y < settings_footer.y && settings_reset.valid() && settings_close.valid() && settings_reset.x < settings_close.x &&
                                   settings_reset.y >= settings_footer.y - 1.0 && settings_close.y >= settings_footer.y - 1.0 && settings_footer.contains_horizontally(settings_reset) &&
                                   settings_footer.contains_horizontally(settings_close);
 const bool settings_numeric_alignment = [&] {
  const Bounds& reference_label = settings_numeric_labels.front();
  const Bounds& reference_value = settings_numeric_values.front();
  for (std::size_t index = 0; index < SETTINGS_NUMERIC_CONTROLS.size(); ++index) {
   const Bounds& control = settings_numeric_controls[index];
   const Bounds& label = settings_numeric_labels[index];
   const Bounds& value = settings_numeric_values[index];
   if (!control.valid() || !label.valid() || !value.valid() || std::abs(label.x - reference_label.x) >= 1.0 || std::abs(label.width - reference_label.width) >= 1.0 ||
       std::abs(value.x - reference_value.x) >= 1.0 || std::abs(value.width - reference_value.width) >= 1.0 || label.x < control.x - 1.0 || value.x + value.width > control.x + control.width + 1.0) {
    return false;
   }
  }
  return true;
 }();
 const auto aligned_grid = [this](const auto& fields) {
  if (!std::ranges::all_of(fields, [](const Bounds& bounds) { return bounds.valid(); })) return false;
  const Bounds& reference = fields.front();
  for (std::size_t index = 0; index < fields.size(); ++index) {
   const Bounds& bounds = fields[index];
   if (std::abs(bounds.y - reference.y) >= 1.0 || std::abs(bounds.width - reference.width) >= 1.0 || bounds.x < advanced_container.x - 1.0 ||
       bounds.x + bounds.width > advanced_container.x + advanced_container.width + 1.0 || bounds.y < advanced_container.y - 1.0 ||
       bounds.y + bounds.height > advanced_container.y + advanced_container.height + 1.0 || (index != 0U && bounds.x < fields[index - 1U].x + fields[index - 1U].width - 1.0)) {
    return false;
   }
  }
  return true;
 };
 const std::array advanced_general{advanced_fixed[0], advanced_fixed[1], advanced_fixed[2], advanced_fixed[3]};
 const std::array advanced_optimizer{advanced_fixed[4], advanced_fixed[5], advanced_fixed[6], advanced_fixed[7]};
 const bool advanced_composition =
  aligned_grid(advanced_general) && aligned_grid(advanced_optimizer) && aligned_grid(advanced_match_free) && aligned_grid(advanced_denoising) &&
  std::abs(advanced_general.front().width - advanced_optimizer.front().width) < 1.0 && std::abs(advanced_general.front().width - advanced_match_free.front().width) < 1.0 &&
  std::abs(advanced_general.front().width - advanced_denoising.front().width) < 1.0 && advanced_optimizer.front().y >= advanced_general.front().y + advanced_general.front().height - 1.0 &&
  advanced_assignment.valid() && advanced_denoising_toggle.valid() && advanced_assignment.x >= advanced_container.x - 1.0 &&
  advanced_assignment.x + advanced_assignment.width <= advanced_container.x + advanced_container.width + 1.0 && advanced_assignment.y >= advanced_container.y - 1.0 &&
  advanced_assignment.y + advanced_assignment.height <= advanced_container.y + advanced_container.height + 1.0 &&
  advanced_assignment.y >= advanced_optimizer.front().y + advanced_optimizer.front().height - 1.0 && advanced_denoising_toggle.x >= advanced_container.x - 1.0 &&
  advanced_denoising_toggle.x + advanced_denoising_toggle.width <= advanced_container.x + advanced_container.width + 1.0 && advanced_denoising_toggle.y >= advanced_container.y - 1.0 &&
  advanced_denoising_toggle.y + advanced_denoising_toggle.height <= advanced_container.y + advanced_container.height + 1.0 &&
  advanced_match_free.front().y >= advanced_denoising_toggle.y + advanced_denoising_toggle.height - 1.0;
 const bool advanced_compact = [&] {
  const auto compact_row = [](const auto& fields) {
   if (fields.size() < 2U) return false;
   const double cell_pitch = fields[1].x - fields[0].x;
   return cell_pitch > 0.0 && std::ranges::all_of(fields, [cell_pitch](const Bounds& bounds) { return bounds.width < cell_pitch * 0.70; });
  };
  return compact_row(advanced_general) && compact_row(advanced_optimizer) && compact_row(advanced_match_free) && compact_row(advanced_denoising);
 }();
 const bool error_composition = error_modal.valid() && error_copy.valid() && error_dismiss.valid() && error_copy.x < error_dismiss.x && error_copy.y >= error_modal.y - 1.0 &&
                                error_dismiss.y >= error_modal.y - 1.0 && error_modal.contains_horizontally(error_copy) && error_modal.contains_horizontally(error_dismiss);
 const bool gallery_shader_fill = explore_gallery.valid() && std::ranges::any_of(surface_geometries, [this](const auto& geometry) {
  const bool gallery_frame = std::ranges::any_of(explore_slot_frames, [&geometry](const auto& frame) { return frame.second == geometry.source_revision; });
  const auto key = std::pair{geometry.presentation_revision, geometry.source_revision};
  if (!gallery_frame || !surface_scales.contains(key)) return false;
  const double scale = surface_scales.at(key);
  return std::abs(geometry.width / scale - explore_gallery.width) < 1.0 && square_atlas_frames.contains(key) && atlas_scaled_frames.contains(key);
 });
 const bool detail_containers = detail_fit;
 // Automatic Basic may supersede the model canvas before the browser's
 // next frame. The exact source transition proves that product;
 // require the Original view to have been physically drawn.
 const bool padded_and_original_detail = detail_source && original_detail_drawn;
 static const std::set<std::string, std::less<>> expected_upscale_modes{"basic-four-times", "fast-four-times", "neural-four-times"};
 static const std::set<std::string, std::less<>> expected_upscale_presentations{EXPLORE_UPSCALE_BASIC, EXPLORE_UPSCALE_FAST, EXPLORE_UPSCALE_NEURAL};
 const bool bounded_exact_grid = exact_grid && exact_grid_revision != 0U && exact_grid_frame_revision != 0U && exact_grid_width <= exact_grid_capacity_width &&
                                 exact_grid_height <= exact_grid_capacity_height && explore_slot_frames.contains(exact_grid_revision) &&
                                 explore_slot_frames.at(exact_grid_revision) == exact_grid_frame_revision;
 const bool repeated_same_revision = std::ranges::any_of(surface_redraw_counts, [](const auto& draws) { return draws.second >= 3U; });
 const bool pointer_render_chain = pointer_frame_revision != 0U && std::ranges::any_of(surface_geometries, [this](const auto& geometry) {
  const auto key = std::pair{geometry.presentation_revision, geometry.source_revision};
  return geometry.source_revision == pointer_frame_revision && surface_scales.contains(key) && surface_draws.contains(geometry.presentation_revision);
 });
 static const std::array expected_drag_stages{"pressed", "moved-1", "moved-2", "released"};
 const bool complete_pointer_drag = std::ranges::all_of(expected_drag_stages, [this](const std::string_view stage) { return ui_scale_pointer_stages.contains(stage); });
 static const std::array expected_primary{
  DATASET_BROWSE,
  COMPILE_DATASET,
  TRAIN_MODEL_CUSTOM,
  TRAIN_MODEL_ACTION,
  EXPLORE_OPEN,
 };
 const bool uniform_primary = !shared_primary_colors.empty() && std::ranges::all_of(shared_primary_colors, [this](const auto& style) { return style.second == shared_primary_colors.begin()->second; });
 static const std::array expected_primary_labels{"Start Training", "Start Validation", "Run Predict", "Run Export", "Start Live", "Save Annotations"};
 return first_failed_check(std::ranges::all_of(expected_primary_labels, [this](const char* label) { return primary_idle_labels.contains(label); }), "primary action rendered labels",
  bootstrap && fluent && uniform_primary && benchmark_purple && rendered_controls.contains(BENCHMARK_OVERRIDE) &&
   std::ranges::all_of(expected_primary, [this](const std::string_view id) { return shared_primary.contains(id) && rendered_controls.contains(id); }),
  "shell and style", every_region, "ordinary workflow regions", primary_progress_placement, "primary progress placement", primary_action_geometry, "primary action geometry", primary_card_gaps,
  "primary card gaps match Export", reference_columns, "workflow column geometry", vertical_composition, "workflow vertical composition", advanced_composition, "Advanced composition",
  advanced_compact, "Advanced compact controls", explore_integer_controls.size() == 5U && explore_integer_precision, "Explore integer editing and spinner suppression", explore_paste_restored,
  "Explore clipboard paste persistence and restoration", spinnerless_integer, "integer spinner suppression", spinnerless_floating, "floating spinner suppression", advanced_integer_persisted,
  "Advanced integer persistence", advanced_floating_persisted, "Advanced floating persistence", compile_progress_placement, "Dataset progress placement", model_progress_placement,
  "Model progress containment", model_composition && model_copy, "Model card composition", benchmark_override.valid() && benchmark_round_trip && benchmark_choices_complete(),
  "benchmark override interaction", perceptual_controls_round_trip, "independent perceptual controls round trip", explore_composition, "Explore composition", annotation_composition,
  "annotation composition", workspace_fps_text && workspace_fps_pixels, "visible workspace FPS",
  settings_composition && settings_numeric_alignment && show_fps_round_trip && ui_scale_drag && ui_scale_released && ui_scale_restored && complete_pointer_drag && error_composition &&
   error_modal_usable,
  "Settings composition", dataset_configured && progress && compile_metrics && compile_tracks.size() == 3U && dataset_complete && progress_ordinal < dataset_complete_ordinal, "Dataset lifecycle", explore_ready, "ready snapshot",
  sweep, "viewport sweep", scrolled, "gallery scroll", detail, "detail selection", bounded_exact_grid, "bounded exact grid", newest_placeholder, "newest placeholder", pointer_inverse,
  "pointer inverse", pointer_dispatched, "pointer dispatch", pointer_selected, "pointer selection", gallery_shader_fill, "gallery shader fill", pointer_render_chain, "pointer render chain",
  atlas_identities, "atlas identities", augmentation_enabled, "augmentation enabled", augmentation_rerolled, "augmentation rerolled", reshuffle_order_only, "reshuffle order only", detail_source,
  "detail source", detail_fit, "detail fit", detail_containers, "detail containers", padded_and_original_detail, "padded and original detail", upscale_growth, "upscale growth", upscale_presentation,
  "upscale presentation", upscale_modes == expected_upscale_modes, "upscale modes", upscale_presentations == expected_upscale_presentations, "upscale presentations",
  upscale_completed_pixels == expected_upscale_presentations, "upscale completed blue pixels", upscale_same_method == expected_upscale_presentations, "upscale exact re-click", upscale_later_frame,
  "upscale later frame",
  std::ranges::all_of(
   viewer_navigation_draws, [this](const auto draw) { return draw != 0U && surface_draws.contains(draw); }),
  "Previous/Next automatic upscale draws", reopened, "dataset reopen", repeated_same_revision, "same-revision redraw", annotation_ready && annotation_tool && annotation_pointer,
  "annotation lifecycle", complete && surface_draws.contains(presentation_receipt) && surface_redraws.contains(presentation_receipt), "presentation completion",
  bounds_valid && !failed_before_termination(), "browser validity", firefox_import && firefox_claim && firefox_ready, "Firefox integration",
  std::ranges::all_of(expected, [this](const std::string_view id) { return controls.contains(id); }), "expected controls");
}
auto BrowserAudit::product_ready() const -> bool { return readiness_blocker().empty(); }
auto BrowserAudit::terminal_evidence_settled() const noexcept -> bool {
 return complete && presentation_receipt != 0U && surface_redraw_counts.contains(presentation_receipt) && surface_redraw_counts.at(presentation_receipt) >= 3U;
}
auto BrowserAudit::rendered_frame_for_slots(const std::map<std::uint64_t, std::uint64_t>& native_slots, const std::uint64_t source_revision, const std::size_t minimum_redraws) const noexcept -> bool {
 const auto rendered = [this, minimum_redraws](const std::uint64_t source) noexcept {
  for (const auto& geometry : surface_geometries) {
   const auto key = std::pair{geometry.presentation_revision, geometry.source_revision};
   if (geometry.source_revision != source || !surface_scales.contains(key) || !surface_draws.contains(geometry.presentation_revision) ||
       (minimum_redraws != 0U && (!surface_redraw_counts.contains(geometry.presentation_revision) || surface_redraw_counts.at(geometry.presentation_revision) < minimum_redraws)))
    continue;
   const double scale = surface_scales.at(key);
   const bool normalized = geometry.width > 0.0 && geometry.height > 0.0 && scale > 0.0 &&
                           (!explore_gallery.valid() || (std::abs(geometry.width / scale - explore_gallery.width) < 1.0 && square_atlas_frames.contains(key) && atlas_scaled_frames.contains(key)));
   if (normalized) return true;
  }
  return false;
 };
 if (source_revision != 0U) return observed_frame_revision_for_slots(native_slots, source_revision) && rendered(source_revision);
 for (const auto& [snapshot_revision, slots] : explore_slots) {
  if (slots == native_slots && explore_slot_frames.contains(snapshot_revision) && rendered(explore_slot_frames.at(snapshot_revision))) return true;
 }
 return false;
}
auto BrowserAudit::observed_frame_revision_for_slots(const std::map<std::uint64_t, std::uint64_t>& native_slots, const std::uint64_t source_revision) const noexcept -> bool {
 return source_revision != 0U && std::ranges::any_of(explore_slots, [this, &native_slots, source_revision](const auto& snapshot) {
  return snapshot.second == native_slots && explore_slot_frames.contains(snapshot.first) && explore_slot_frames.at(snapshot.first) == source_revision;
 });
}
auto BrowserAudit::final_cursor_slots() const noexcept -> const std::map<std::uint64_t, std::uint64_t>* {
 const auto slots = explore_slots.find(final_cursor_revision);
 if (final_cursor_revision == 0U || final_cursor_frame_revision == 0U || final_cursor_generation == 0U || final_cursor_slot_count == 0U ||
     final_cursor_slot_count > mmltk::controller::kExploreVisibleItemCapacity || slots == explore_slots.end() || slots->second.size() != final_cursor_slot_count ||
     !explore_slot_frames.contains(final_cursor_revision) || explore_slot_frames.at(final_cursor_revision) != final_cursor_frame_revision)
  return nullptr;
 for (std::uint64_t slot = 0U; slot != final_cursor_slot_count; ++slot)
  if (!slots->second.contains(slot)) return nullptr;
 return &slots->second;
}
auto BrowserAudit::pointer_slots() const noexcept -> const std::map<std::uint64_t, std::uint64_t>* {
 const auto slots = explore_slots.find(pointer_revision);
 return pointer_selected && slots != explore_slots.end() ? &slots->second : nullptr;
}
auto BrowserAudit::failed_before_termination() const noexcept -> bool { return failed || panic || workspace_protocol_failure; }
auto BrowserAudit::failure_blocker() const noexcept -> std::string_view {
 const std::array checks{
  std::pair{failed, std::string_view{"integration failure"}},
  std::pair{panic, std::string_view{"browser panic"}},
  std::pair{workspace_protocol_failure, std::string_view{"workspace protocol failure"}},
 };
 const auto blocker = std::ranges::find_if(checks, [](const auto& check) { return check.first; });
 return blocker == checks.end() ? std::string_view{} : blocker->second;
}
bool BrowserAudit::benchmark_choices_complete() const {
 if (!benchmark_inactive || !benchmark_baseline || benchmark_choices.size() != 12U || benchmark_visibility.size() != 60U) return false;
 const auto& baseline = *benchmark_baseline;
 if ((baseline[0] != 0.0 && baseline[0] != 1.0) || (baseline[1] != 0.0 && baseline[1] != 1.0) || (baseline[2] != 0.0 && baseline[2] != 1.0 && baseline[2] != 2.0) || baseline[3] <= 0.0) return false;
 constexpr std::array benchmark_controls{
  "train.dataset.benchmark.custom", "train.dataset.benchmark.coconut", "train.dataset.validation.coconut", "train.dataset.validation.stock", "train.dataset.validation.coconut_stock"};
 double previous_revision = baseline[3];
 double previous_dataset = baseline[1];
 double previous_validation = baseline[2];
 bool previous_enabled = baseline[0] == 1.0;
 for (std::size_t index = 0; index != 12U; ++index) {
  const auto found = benchmark_choices.find(index);
  if (found == benchmark_choices.end()) return false;
  const auto& [control, values] = found->second;
  const double dataset = index == 5U ? 0.0 : (index >= 1U && index <= 8U ? 1.0 : baseline[1]);
  const double validation = index == 2U ? 1.0 : (index == 3U ? 2.0 : (index >= 4U && index <= 7U ? 0.0 : baseline[2]));
  const bool enabled = index < 10U || (index == 10U && baseline[0] == 1.0);
  const char* expected_control = BENCHMARK_OVERRIDE;
  if (index == 1U || index == 6U)
   expected_control = benchmark_controls[1];
  else if (index == 2U)
   expected_control = benchmark_controls[3];
  else if (index == 3U)
   expected_control = benchmark_controls[4];
  else if (index == 4U)
   expected_control = benchmark_controls[2];
  else if (index == 5U)
   expected_control = benchmark_controls[0];
  else if (index == 7U)
   expected_control = "train.dataset.browse";
  else if (index == 8U)
   expected_control = benchmark_controls[2U + static_cast<std::size_t>(baseline[2])];
  else if (index == 9U)
   expected_control = benchmark_controls[static_cast<std::size_t>(baseline[1])];
  const bool changed = dataset != previous_dataset || validation != previous_validation || enabled != previous_enabled;
  if (control != expected_control || values[1] != dataset || values[2] != validation || values[3] < previous_revision || (changed && values[3] == previous_revision)) return false;
  if ((index >= 1U && index <= 9U) || changed) {
   const auto clicked = benchmark_clicks.find(index);
   if (clicked == benchmark_clicks.end() || clicked->second != control) return false;
  }
  for (std::size_t choice = 0; choice != benchmark_controls.size(); ++choice) {
   const auto visible = benchmark_visibility.find({index, benchmark_controls[choice]});
   const double expected = enabled && (choice < 2U || dataset == 1.0) ? 1.0 : 0.0;
   if (visible == benchmark_visibility.end() || visible->second[1] != expected || visible->second[2] != expected || visible->second[3] != values[3]) return false;
  }
  previous_revision = values[3];
  previous_dataset = dataset;
  previous_validation = validation;
  previous_enabled = enabled;
 }
 return true;
}
bool BrowserAudit::validation_confidence_complete() const {
 if (validation_confidence_edits.size() != 9U || validation_confidence_pixels.size() != 3U) return false;
 const auto generation = scalar(validation_confidence_edits.front(), "d");
 auto revision = scalar(validation_confidence_edits.front(), "c");
 if (!generation || !revision) return false;
 for (std::size_t index = 0U; index < validation_confidence_edits.size(); ++index) {
  const auto& edit = validation_confidence_edits[index];
  const auto expected = index < 6U ? 0.437 : index == 7U ? 1.0 : 0.0;
  const auto current = scalar(edit, "c");
  if (scalar(edit, "a") != index || std::abs(numeric(edit, "b") - expected) > 0.000001 || scalar(edit, "d") != generation ||
      (index > 0U && index < 6U && current != revision) || (index >= 6U && current <= revision)) return false;
  revision = current;
 }
 const auto& first = validation_confidence_pixels.front();
 if (!scalar(first, "clean") || !scalar(first, "detections")) return false;
 for (std::size_t index = 0U; index < validation_confidence_pixels.size(); ++index) {
  const auto& pixels = validation_confidence_pixels[index];
  if (!pixels.value("matched", false) || pixels.value("control", "") != "validate.samples.atlas" || scalar(pixels, "stage") != index + 6U ||
      numeric(pixels, "threshold") != (index == 1U ? 1.0 : 0.0) || scalar(pixels, "generation") != generation ||
      scalar(pixels, "clean") != scalar(first, "clean") || scalar(pixels, "detections") != scalar(first, "detections") ||
      numeric(pixels, "minimum") != numeric(first, "minimum") || numeric(pixels, "maximum") != numeric(first, "maximum") ||
      !(numeric(pixels, "minimum") >= 0.0 && numeric(pixels, "maximum") < 1.0) ||
      scalar(pixels, "revision") != scalar(validation_confidence_edits[index + 6U], "c") ||
      (index == 1U ? scalar(pixels, "different") < 12U : scalar(pixels, "different") != 0U)) return false;
 }
 return true;
}
bool BrowserAudit::validation_layout_complete() const {
 for (const auto* label : {"Groundtruth", "Detections", "Display confidence", "Preview only"})
  if (!validation_text.contains(label)) return false;
 for (const auto* stage : {"atlas", "narrow"}) {
  const auto find = [&](const char* control) -> std::optional<Bounds> {
   const auto found = validation_layout.find({stage, control});
   if (found == validation_layout.end()) return std::nullopt;
   const auto& b = found->second;
   return Bounds{b[0], b[1], b[2], b[3]};
  };
  const auto atlas = find("validate.samples.atlas"), gt = find("validate.gt.group"), det = find("validate.pred.group");
  if (!atlas || !gt || !det || !atlas->valid() || !gt->valid() || !det->valid() || !atlas->contains_horizontally(*gt) || !atlas->contains_horizontally(*det) ||
      gt->y < atlas->y + atlas->height || det->y < atlas->y + atlas->height) return false;
  const bool stacked = std::abs(det->x - gt->x) < 1.0 && std::abs(det->y - gt->y - gt->height - 20.0) < 1.0;
  const bool horizontal = std::abs(det->y - gt->y) < 1.0 && std::abs(det->x - gt->x - gt->width - 20.0) < 1.0;
  if ((!stacked && !horizontal) || (std::string_view{stage} == "narrow" && !stacked)) return false;
 }
 return true;
}
}  // namespace mmltk::acceptance::wayland
