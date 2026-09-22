#include "audit_facts.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "src/controller/browser/application_stable_identity.h"
#include "src/controller/presentation/workspace_presentation_types.h"
#include "native_audit.h"
namespace mmltk::acceptance::wayland {
constexpr std::uint64_t kUpdateViewportEndpoint = mmltk::controller::browser::application_stable_id("explore", "UpdateViewport");
auto NativeAudit::reject_causal_evidence(const std::string_view reason) noexcept -> void {
 causal_inconsistent = true;
 if (causal_failure.empty()) causal_failure = reason;
}
auto NativeAudit::reconcile_incremental_publication(const std::uint64_t generation) -> void {
 const auto cardinality = placeholder_cardinalities.find(generation);
 const auto placeholder = placeholder_slots.find(generation);
 const auto first_ordinal = first_publication_ordinals.find(generation);
 const auto last_ordinal = last_publication_ordinals.find(generation);
 const auto first_count = first_published_tiles.find(generation);
 const auto latest_count = published_tiles.find(generation);
 const auto first_slots = first_patched_slots.find(generation);
 const auto placeholder_ordinal = placeholder_ordinals.find(generation);
 if (cardinality == placeholder_cardinalities.end() || placeholder == placeholder_slots.end() || placeholder->second.size() != cardinality->second ||
     first_ordinal == first_publication_ordinals.end() || last_ordinal == last_publication_ordinals.end() || first_count == first_published_tiles.end() || latest_count == published_tiles.end() ||
     first_slots == first_patched_slots.end() || placeholder_ordinal == placeholder_ordinals.end())
  return;
 const bool exact_observed_slots = !first_slots->second.empty() && first_slots->second.size() <= first_count->second && std::ranges::includes(placeholder->second, first_slots->second);
 if (partial_generation == 0U && first_count->second != 0U && first_count->second < cardinality->second && exact_observed_slots) {
  partial_generation = generation;
  partial_placeholder_ordinal = placeholder_ordinal->second;
  partial_first_patch_ordinal = first_ordinal->second;
  partial_first_tile_count = first_count->second;
  explore_partial_patch = true;
  acceptance_first_patch_exact = true;
 }
 if (generation == partial_generation && last_ordinal->second > partial_first_patch_ordinal && latest_count->second > partial_first_tile_count) explore_ready_batch = true;
}
auto NativeAudit::reconcile_rendered_probes(const std::uint64_t generation) -> void {
 const auto frames = published_frames.find(generation);
 const auto placeholder = placeholder_slots.find(generation);
 if (frames == published_frames.end() || frames->second.empty() || placeholder == placeholder_slots.end()) return;
 for (const auto& [key, compiled_index] : rendered_probe_slots) {
  if (key.first != generation) continue;
  const auto transition = transition_probe_slots.find(key);
  const auto slot = placeholder->second.find(key.second);
  if (transition == transition_probe_slots.end() || slot == placeholder->second.end()) continue;
  if (transition->second != compiled_index || slot->second != compiled_index) {
   reject_causal_evidence("rendered probe frame identity");
   continue;
  }
  const auto patched_generation = patched_slots.find(generation);
  if (patched_generation != patched_slots.end()) {
   const auto patched = patched_generation->second.find(key.second);
   if (patched != patched_generation->second.end() && patched->second != compiled_index) {
    reject_causal_evidence("rendered probe patch identity");
    continue;
   }
  }
  std::vector<std::uint64_t> frame_revisions;
  const auto rendered_ordinal = rendered_probe_ordinals.find(key);
  const auto transition_ordinal = transition_probe_ordinals.find(key);
  if (rendered_ordinal != rendered_probe_ordinals.end() && transition_ordinal != transition_probe_ordinals.end()) {
   const auto first_probe = std::min(rendered_ordinal->second, transition_ordinal->second);
   const auto last_probe = std::max(rendered_ordinal->second, transition_ordinal->second);
   for (const auto& frame : frames->second)
    if (frame.first > last_probe) frame_revisions.push_back(frame.second);
   if (frame_revisions.empty()) {
    for (auto frame = frames->second.rbegin(); frame != frames->second.rend(); ++frame)
     if (frame->first < first_probe) {
      frame_revisions.push_back(frame->second);
      break;
     }
   }
  } else if (frames->second.size() == 1U) {
   // Direct audit fixtures can provide already-correlated probes.
   frame_revisions.push_back(frames->second.front().second);
  }
  if (frame_revisions.empty()) continue;
  if (!rendered_probe_frames.contains(key) && rendered_probe_frames.size() == kAcceptanceRecordLimit) {
   reject_causal_evidence("rendered probe frame evidence capacity");
   continue;
  }
  rendered_probe_frames.insert_or_assign(key, std::move(frame_revisions));
 }
}
auto NativeAudit::record_frame(const std::uint64_t generation, const std::uint64_t revision, const std::size_t publication_ordinal) -> void {
 if (revision == 0U) {
  reject_causal_evidence("rendered probe frame revision");
  return;
 }
 auto& frames = published_frames[generation];
 if (std::ranges::any_of(frames, [revision](const auto& frame) { return frame.second == revision; })) return;
 if (frames.size() == kAcceptanceRecordLimit) {
  reject_causal_evidence("Explore frame evidence capacity");
  return;
 }
 const auto position = std::ranges::lower_bound(frames, publication_ordinal, {}, &FramePublication::first);
 frames.emplace(position, publication_ordinal, revision);
 reconcile_rendered_probes(generation);
}
auto NativeAudit::record_tile_publication(const std::uint64_t generation, const std::uint64_t count, const std::size_t publication_ordinal) -> void {
 const auto previous = published_tiles.find(generation);
 explore_tile_regressed = explore_tile_regressed || (previous != published_tiles.end() && count < previous->second);
 if (count == 0U || !placeholder_ordinals.contains(generation)) return;
 const auto [_, first] = first_publication_ordinals.try_emplace(generation, publication_ordinal);
 if (first) {
  first_published_tiles.emplace(generation, count);
  first_patched_slots.emplace(generation, patched_slots[generation]);
 }
 last_publication_ordinals.insert_or_assign(generation, publication_ordinal);
 published_tiles.insert_or_assign(generation, count);
 reconcile_incremental_publication(generation);
}
auto NativeAudit::join_gallery_publication(const std::uint64_t generation, const std::map<std::uint64_t, std::uint64_t>& complete_slots) -> void {
 if (complete_slots.empty() || complete_slots.size() > mmltk::controller::kExploreVisibleItemCapacity || complete_slots.rbegin()->first != complete_slots.size() - 1U || generation == 0U) return;
 const auto cardinality = placeholder_cardinalities.find(generation);
 if (cardinality == placeholder_cardinalities.end()) return;
 if (complete_slots.size() != cardinality->second) {
  reject_causal_evidence("joined placeholder cardinality");
  return;
 }
 const auto observed = placeholder_slots.find(generation);
 if (observed == placeholder_slots.end()) return;
 const auto& received = observed->second;
 const bool conflict = std::ranges::any_of(received, [&complete_slots](const auto& slot) {
  const auto found = complete_slots.find(slot.first);
  return found == complete_slots.end() || found->second != slot.second;
 });
 if (conflict) {
  reject_causal_evidence("joined placeholder identity");
  return;
 }
 // The two writers are independent. Missing native slots must arrive
 // from the native cursor; a browser inventory never creates them.
 if (received.size() != complete_slots.size()) return;
 const auto patched = patched_slots.find(generation);
 if (patched != patched_slots.end())
  explore_stale_patch = explore_stale_patch || std::ranges::any_of(patched->second, [&received](const auto& slot) {
   const auto found = received.find(slot.first);
   return found == received.end() || found->second != slot.second;
  });
 // Frame and tile publication are recorded exclusively by their native
 // producers. Presentation and frontend snapshots retain separate facts.
 reconcile_incremental_publication(generation);
 reconcile_rendered_probes(generation);
}
auto NativeAudit::record_probe(
 ProbeSlots& probes, ProbeOrdinals& ordinals, const ProbeSlots::key_type& key, const ProbeSlots::mapped_type compiled_index, const bool valid, const std::string_view failure) -> void {
 if (probes.contains(key) && probes.at(key) != compiled_index) {
  reject_causal_evidence("rendered probe identity changed");
  return;
 }
 if (!valid) {
  reject_causal_evidence(failure);
  return;
 }
 if (!probes.contains(key) && probes.size() == kAcceptanceRecordLimit) {
  reject_causal_evidence("rendered probe evidence capacity");
  return;
 }
 probes.insert_or_assign(key, compiled_index);
 ordinals.try_emplace(key, ordinal);
 reconcile_rendered_probes(key.first);
}
auto NativeAudit::record_card_geometry(const std::uint64_t generation, const std::uint64_t slot, const std::uint64_t compiled_index, const std::uint64_t card_width, const std::uint64_t card_height,
 const std::uint64_t content_x, const std::uint64_t content_y, const std::uint64_t content_width, const std::uint64_t content_height) -> void {
 const auto right_padding = card_width >= content_x && card_width - content_x >= content_width ? card_width - content_x - content_width : std::numeric_limits<std::uint64_t>::max();
 const auto bottom_padding = card_height >= content_y && card_height - content_y >= content_height ? card_height - content_y - content_height : std::numeric_limits<std::uint64_t>::max();
 const bool vertical_padding =
  content_x == 0U && content_width == card_width && content_y != 0U && bottom_padding != 0U && std::max(content_y, bottom_padding) - std::min(content_y, bottom_padding) <= 1U;
 const bool horizontal_padding =
  content_y == 0U && content_height == card_height && content_x != 0U && right_padding != 0U && std::max(content_x, right_padding) - std::min(content_x, right_padding) <= 1U;
 if (card_width == 0U || card_width != card_height || content_width == 0U || content_height == 0U || vertical_padding == horizontal_padding) return;
 const auto key = std::pair{generation, slot};
 const auto orientation = vertical_padding ? PaddingOrientation::Vertical : PaddingOrientation::Horizontal;
 if ((padded_card_slots.contains(key) && padded_card_slots.at(key) != compiled_index) || (padding_orientations.contains(key) && padding_orientations.at(key) != orientation))
  reject_causal_evidence("padded card identity changed");
 if (padded_card_slots.contains(key) || padded_card_slots.size() < kAcceptanceRecordLimit) {
  padded_card_slots.insert_or_assign(key, compiled_index);
  padding_orientations.insert_or_assign(key, orientation);
 } else {
  reject_causal_evidence("padded card evidence capacity");
 }
}
auto NativeAudit::consume_explore_evidence(const char* const event, const std::uint64_t value, const std::uint64_t detail, const std::uint64_t staging_bytes) -> void {
 consume({{"kind", "gui_runtime"}, {"owner", "explore"}, {"event", event}, {"sequence", 2U}, {"value", value}, {"detail", detail}, {"staging_bytes", staging_bytes}});
}
auto NativeAudit::consume(const nlohmann::json& record) -> void {
 if (record.value("kind", "") != "gui_runtime") return;
 ++ordinal;
 const std::string owner = record.value("owner", "");
 const std::string event = record.value("event", "");
 const std::uint64_t sequence = scalar(record, "sequence");
 const std::uint64_t value = scalar(record, "value");
 if (event == "gallery.read.scheduled") {
  admission_seen = true;
  const auto columns = scalar(record, "admission_columns");
  const auto first = scalar(record, "admission_first_row");
  const auto count = scalar(record, "admission_row_count");
  const auto position = scalar(record, "admission_position");
  const auto tier = scalar(record, "admission_tier");
  const bool forward = record.value("admission_forward", true);
  const auto row = columns == 0U ? 0U : position / columns;
  const bool visible = row >= first && row - first < count;
  const bool after = row >= first && row - first >= count;
  const auto expected = visible ? 0U : (after == forward ? 1U : 2U);
  const auto preferred = scalar(record, forward ? "admission_forward_eligible" : "admission_backward_eligible");
  admission_priority_valid = admission_priority_valid && columns != 0U && count != 0U && tier == expected && (visible || (after ? row - first - count < 4U : first - row <= 4U)) &&
                             (tier == 0U || (record.contains("admission_immediate_eligible") && scalar(record, "admission_immediate_eligible") == 0U)) && (tier != 2U || preferred == 0U);
  if (admitted_reads.size() >= kAcceptanceRecordLimit)
   admission_priority_valid = false;
  else
   admitted_reads.emplace(sequence, scalar(record, "detail"));
  if (const auto cached = initial_cache.find(sequence); cached != initial_cache.end()) { cached_first_valid &= cached->second.restored_ordinal != 0U && cached->second.restored_ordinal < ordinal; }
 }
 server_started = server_started || event == "browser.server.started";
 peer_opened = peer_opened || event == "browser.server.peer_opened";
 if (event == "browser.server.peer_opened") ++peer_open_count;
 shutdown_requested = shutdown_requested || event == "shutdown.requested";
 firefox_terminal = firefox_terminal || event == "shutdown.firefox_terminal";
 shutdown_complete = shutdown_complete || event == "shutdown.complete";
 shutdown_incomplete = shutdown_incomplete || event == "shutdown.incomplete";
 worker_failed = worker_failed || event == "worker.failure";
 invalid_message = invalid_message || event == "browser.server.invalid_message";
 peer_replaced = peer_replaced || event == "browser.server.peer_replaced";
 if (event == "browser.server.peer_closed") {
  peer_closed_after_shutdown = peer_closed_after_shutdown || shutdown_requested;
  ++peer_close_count;
 }
 interaction_rejected = interaction_rejected || event == "browser.interaction.rejected";
 if (event == "browser.interaction.accepted" && record.value("participant", "") == "UpdateViewport") {
  const bool new_generation = !accepted_generations.contains(value);
  const bool valid = sequence == kUpdateViewportEndpoint && value != 0U && (!new_generation || accepted_viewports.size() < kAcceptanceGenerationLimit) &&
                     (accepted_viewports.empty() || accepted_viewports.back().generation <= value);
  if (!valid) reject_causal_evidence("viewport acceptance order");
  if (valid && new_generation) {
   accepted_generations.emplace(value, ordinal);
   accepted_viewports.push_back({.endpoint = sequence, .generation = value, .ordinal = ordinal});
  }
 }
 explore_rendered = explore_rendered || (owner == "explore" && event == "render.completed");
 if (owner == "explore" && event == "render.completed" && value != 0U) {
  explore_nproc_changed = explore_nproc_changed || (explore_nproc != 0U && explore_nproc != value);
  explore_nproc = explore_nproc == 0U ? value : explore_nproc;
 }
 if (owner == "explore" && event == "placeholder.published") {
  explore_placeholder = sequence != 0U;
  explore_generation = sequence;
  explore_placeholder_ordinal = ordinal;
  placeholder_ordinals.try_emplace(sequence, ordinal);
  explore_max_pinned = std::max(explore_max_pinned, static_cast<std::size_t>(scalar(record, "staging_bytes")));
 }
 if (owner == "explore" && event == "acceptance.placeholder.slot") {
  if (!placeholder_slots.contains(sequence) && placeholder_slots.size() == kAcceptanceGenerationLimit) {
   reject_causal_evidence("placeholder generation capacity");
   return;
  }
  auto& slots = placeholder_slots[sequence];
  const auto compiled_index = scalar(record, "detail");
  if (value >= mmltk::controller::kExploreVisibleItemCapacity || (slots.contains(value) && slots.at(value) != compiled_index)) reject_causal_evidence("placeholder slot identity");
  if (value < mmltk::controller::kExploreVisibleItemCapacity) slots.insert_or_assign(value, compiled_index);
  if (value < mmltk::controller::kExploreVisibleItemCapacity && scalar(record, "capacity_width") == 1U) initial_cache[sequence].slots.emplace(value);
 }
 if (owner == "explore" && event == "acceptance.placeholder.complete") {
  const auto cardinality = static_cast<std::size_t>(value);
  const auto digest = scalar(record, "detail");
  if (!placeholder_slots.contains(sequence) && placeholder_slots.size() == kAcceptanceGenerationLimit) {
   reject_causal_evidence("placeholder generation capacity");
   return;
  }
  const auto& slots = placeholder_slots[sequence];
  const bool invalid_slot = std::ranges::any_of(slots, [cardinality](const auto& slot) { return slot.first >= cardinality; });
  if (cardinality > mmltk::controller::kExploreVisibleItemCapacity || slots.size() > cardinality || invalid_slot ||
      (placeholder_cardinalities.contains(sequence) && placeholder_cardinalities.at(sequence) != cardinality) || (placeholder_digests.contains(sequence) && placeholder_digests.at(sequence) != digest))
   reject_causal_evidence("placeholder cardinality");
  if (cardinality <= mmltk::controller::kExploreVisibleItemCapacity) {
   placeholder_cardinalities.insert_or_assign(sequence, cardinality);
   placeholder_digests.insert_or_assign(sequence, digest);
   if (scalar(record, "admission_columns") != 0U) {
    auto& cached = initial_cache[sequence];
    cached.restored_ordinal = ordinal;
    cached.forward = record.value("admission_forward", true);
    cached_first_valid &= cached.slots.size() == scalar(record, "capacity_width");
   }
   // Gallery emits its inventory even when retained pixels make
   // Explore's PlaceholderPublished/DiagnoseFrame conditional.
   // This is the inventory's own entered boundary, not a
   // reconstruction of either omitted product event.
   if (sequence >= explore_generation) {
    explore_placeholder = sequence != 0U;
    explore_generation = sequence;
    explore_placeholder_ordinal = ordinal;
    placeholder_ordinals.try_emplace(sequence, ordinal);
   }
   reconcile_incremental_publication(sequence);
  }
 }
 if (owner == "explore" && event == "acceptance.slot.patched") {
  const auto compiled_index = scalar(record, "detail");
  if (!patched_slots.contains(sequence) && patched_slots.size() == kAcceptanceGenerationLimit) {
   reject_causal_evidence("patched generation capacity");
   return;
  }
  auto& slots = patched_slots[sequence];
  if (value >= mmltk::controller::kExploreVisibleItemCapacity || (slots.contains(value) && slots.at(value) != compiled_index)) reject_causal_evidence("patched slot identity");
  if (value < mmltk::controller::kExploreVisibleItemCapacity) slots.insert_or_assign(value, compiled_index);
  last_patch_ordinals.insert_or_assign(sequence, ordinal);
  const auto placeholder = placeholder_slots[sequence].find(value);
  explore_stale_patch = explore_stale_patch || (placeholder != placeholder_slots[sequence].end() && placeholder->second != compiled_index);
  reconcile_rendered_probes(sequence);
 }
 if (owner == "explore" && event == "acceptance.completion.held") {
  const auto compiled_index = scalar(record, "detail");
  const auto capacity = scalar(record, "staging_bytes");
  if (acceptance_held_read && (held_generation != sequence || held_slot != value || held_compiled_index != compiled_index || held_capacity != capacity))
   reject_causal_evidence("held read identity changed");
  acceptance_held_read = true;
  acceptance_held_completed = true;
  held_generation = sequence;
  held_slot = value;
  held_compiled_index = compiled_index;
  held_capacity = capacity;
  held_ordinal = ordinal;
 }
 if (owner == "explore" && event == "acceptance.completion.released" && sequence == held_generation && value == held_slot && scalar(record, "detail") == held_compiled_index) {
  acceptance_held_released = true;
  held_release_ordinal = ordinal;
 }
 if (owner == "explore" && event == "tile.batch.published") {
  record_tile_publication(sequence, value, ordinal);
  explore_max_pinned = std::max(explore_max_pinned, static_cast<std::size_t>(scalar(record, "staging_bytes")));
 }
 explore_stale_discard = explore_stale_discard || (owner == "explore" && event == "thumbnail.stale.discarded" && sequence != 0U && value != 0U);
 if (owner == "explore" && event == "acceptance.stale.read.discarded" && sequence == held_generation && value == held_slot && scalar(record, "detail") == held_compiled_index &&
     scalar(record, "staging_bytes") == held_capacity && held_discard_ordinal == 0U) {
  held_discard_ordinal = ordinal;
  acceptance_held_stale = true;
 }
 if (owner == "explore" && event == "thumbnail.stale.discarded") explore_stale_count += value;
 if (owner == "explore" && event == "explore.augmentation.batch.prepared") {
  const bool usable_batch = value != 0U && scalar(record, "capacity_width") != 0U && scalar(record, "capacity_height") != 0U;
  if (usable_batch && (augmentation_seeds.contains(sequence) || augmentation_seeds.size() < kAcceptanceGenerationLimit))
   augmentation_seeds.insert_or_assign(sequence, scalar(record, "detail"));
  else if (usable_batch)
   reject_causal_evidence("augmentation seed capacity");
 }
 if (owner == "explore" && event == "explore.image.pixel_checksum" && (scalar(record, "staging_bytes") & 7U) == 7U) {
  const auto seed = scalar(record, "capacity_width");
  const bool has_pixels = scalar(record, "detail") != 0U;
  if (has_pixels && (seed == 0U || seed == 1U)) {
   if (augmentation_pixel_seeds.contains(sequence) && augmentation_pixel_seeds.at(sequence) != seed) reject_causal_evidence("augmentation pixel seed changed");
   augmentation_pixel_seeds.insert_or_assign(sequence, seed);
  }
 }
 overlay_descriptors = overlay_descriptors || (owner == "explore" && event == "explore.overlay.descriptors.prepared" && value != 0U && scalar(record, "detail") != 0U);
 if (owner == "explore" && event == "explore.overlay.transformed_bounds") {
  const auto detail = scalar(record, "detail");
  const auto minimum_x = scalar(record, "capacity_width");
  const auto minimum_y = scalar(record, "capacity_height");
  const auto maximum_x = scalar(record, "staging_bytes") >> 32U;
  const auto maximum_y = scalar(record, "staging_bytes") & 0xffffffffU;
  if ((detail >> 32U) != 0U && (detail & 0xffffffffU) != 0U && maximum_x > minimum_x && maximum_y > minimum_y && maximum_x <= 65'535U && maximum_y <= 65'535U) {
   const auto key = std::pair{sequence, value};
   if (transformed_overlay_slots.contains(key) || transformed_overlay_slots.size() < kAcceptanceRecordLimit)
    transformed_overlay_slots.insert(key);
   else
    reject_causal_evidence("transformed overlay evidence capacity");
  }
 }
 if (owner == "explore" && event == "explore.semantic.nonzero_pixels" && scalar(record, "detail") != 0U && scalar(record, "capacity_width") != 0U && scalar(record, "capacity_height") != 0U) {
  const auto key = std::pair{sequence, value};
  if (semantic_overlay_slots.contains(key) || semantic_overlay_slots.size() < kAcceptanceRecordLimit)
   semantic_overlay_slots.insert(key);
  else
   reject_causal_evidence("semantic overlay evidence capacity");
 }
 if (owner == "explore" && event == "explore.card.geometry_probe") {
  const auto packed = scalar(record, "staging_bytes");
  record_card_geometry(sequence, value, scalar(record, "detail"), scalar(record, "capacity_width"), scalar(record, "capacity_height"), (packed >> 48U) & 0xffffU, (packed >> 32U) & 0xffffU,
   (packed >> 16U) & 0xffffU, packed & 0xffffU);
 }
 if (owner == "explore" && event == "explore.overlay.selection_probe") {
  const auto packed = scalar(record, "staging_bytes");
  const auto slot = packed >> 32U;
  const auto selected_class = (packed >> 16U) & 0xffffU;
  const auto hidden_class = packed & 0xffffU;
  const auto key = std::pair{sequence, slot};
  const auto compiled_index = scalar(record, "detail");
  if (value != 0U && scalar(record, "capacity_height") != 0U && selected_class != 0U) {
   if (selected_overlay_slots.contains(key) || selected_overlay_slots.size() < kAcceptanceRecordLimit)
    selected_overlay_slots.insert_or_assign(key, OverlayDescriptorIdentity{selected_class, compiled_index});
   else
    reject_causal_evidence("selected overlay evidence capacity");
  }
  if (scalar(record, "capacity_width") != 0U && hidden_class != 0U) {
   if (hidden_overlay_slots.contains(key) || hidden_overlay_slots.size() < kAcceptanceRecordLimit)
    hidden_overlay_slots.insert_or_assign(key, OverlayDescriptorIdentity{hidden_class, compiled_index});
   else
    reject_causal_evidence("hidden overlay evidence capacity");
  }
 }
 if (owner == "explore" && event == "explore.card.rendered_probe") {
  const auto key = std::pair{sequence, value};
  const auto packed = scalar(record, "staging_bytes");
  const auto padding_pixels = packed >> 32U;
  const auto compiled_index = scalar(record, "detail");
  record_card_geometry(sequence, value, compiled_index, scalar(record, "source_width"), scalar(record, "source_height"), scalar(record, "content_x"), scalar(record, "content_y"),
   scalar(record, "content_width"), scalar(record, "content_height"));
  const bool complete_probe = scalar(record, "capacity_width") == 31U && scalar(record, "capacity_height") != 0U && padded_card_slots.contains(key) && padded_card_slots.at(key) == compiled_index &&
                              padding_pixels != 0U && ((packed >> 16U) & 0xffffU) != 0U && (packed & 0xffffU) != 0U;
  record_probe(rendered_probe_slots, rendered_probe_ordinals, key, compiled_index, complete_probe, "rendered card probe incomplete");
 }
 if (owner == "explore" && event == "explore.card.transition_probe") {
  const auto key = std::pair{sequence, value};
  const auto compiled_index = scalar(record, "detail");
  const auto expected = scalar(record, "staging_bytes");
  const bool complete_transition = expected != 0U && scalar(record, "capacity_width") >= expected / 2U && scalar(record, "capacity_width") <= expected && scalar(record, "capacity_height") == expected;
  record_probe(transition_probe_slots, transition_probe_ordinals, key, compiled_index, complete_transition, "rendered transition probe incomplete");
 }
 if (owner == "explore" && event == "explore.frame.published") { record_frame(sequence, value, ordinal); }
 donor_descriptors = donor_descriptors || (owner == "explore" && event == "explore.donor.descriptors.prepared" && value != 0U && scalar(record, "detail") != 0U &&
                                           scalar(record, "capacity_width") > value && scalar(record, "capacity_height") > scalar(record, "detail"));
 const bool document_opened = owner == "annotation" && event == "document.opened";
 annotation_copied = annotation_copied || (owner == "annotation" && event == "copy.completed");
 annotation_opened = annotation_opened || document_opened;
 annotation_edited = annotation_edited || (owner == "annotation" && event == "document.edited");
 if (owner == "presentation" && event == "timeline.ready" && value != 0U) {
  presentation_ready = true;
  presentation_timeline = std::max(presentation_timeline, value);
 }
 if (owner == "presentation" && event == "presentation.frame.edge" &&
     scalar(record, "source_session") == mmltk::controller::presentation_source_session(mmltk::controller::PresentationSourceKind::Explore) && scalar(record, "source_instance") != 0U &&
     scalar(record, "source_observation_revision") != 0U && scalar(record, "source_revision") != 0U && scalar(record, "frame_revision") == scalar(record, "source_revision")) {
  const auto frame = std::pair{scalar(record, "source_observation_revision"), scalar(record, "source_revision")};
  if (presented_explore_frames.contains(frame) || presented_explore_frames.size() < kAcceptanceRecordLimit)
   presented_explore_frames.try_emplace(frame, ordinal);
  else
   reject_causal_evidence("presented Explore frame evidence capacity");
 }
 if (owner == "firefox_process" && event == "child.spawned" && sequence <= static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) firefox_pid = static_cast<pid_t>(sequence);
}
auto NativeAudit::final_generations_for(const std::map<std::uint64_t, std::uint64_t>& rendered_slots, const std::uint64_t generation, const std::uint64_t frame_revision) const noexcept
 -> std::optional<FinalCursorGenerations> {
 if (rendered_slots.empty() || generation == 0U || frame_revision == 0U) return std::nullopt;
 const auto placeholder = placeholder_slots.find(generation);
 if (placeholder == placeholder_slots.end() || !placeholder_cardinalities.contains(generation) || placeholder_cardinalities.at(generation) != placeholder->second.size() ||
     placeholder->second != rendered_slots)
  return std::nullopt;
 const auto native_frame = published_frames.find(generation);
 if (native_frame == published_frames.end() || std::ranges::none_of(native_frame->second, [frame_revision](const auto& publication) { return publication.second == frame_revision; }))
  return std::nullopt;
 return FinalCursorGenerations{.material = generation, .cursor = generation};
}
auto NativeAudit::generation_for(const std::map<std::uint64_t, std::uint64_t>& rendered_slots) const noexcept -> std::optional<std::uint64_t> {
 for (auto accepted = accepted_viewports.rbegin(); accepted != accepted_viewports.rend(); ++accepted) {
  const auto placeholder = placeholder_slots.find(accepted->generation);
  if (placeholder != placeholder_slots.end() && placeholder_cardinalities.contains(accepted->generation) && placeholder_cardinalities.at(accepted->generation) == placeholder->second.size() &&
      placeholder->second == rendered_slots)
   return accepted->generation;
 }
 return std::nullopt;
}
auto NativeAudit::held_stale_read_discarded() const noexcept -> bool { return acceptance_held_stale; }
auto NativeAudit::stale_thumbnail_discarded() const noexcept -> bool { return explore_stale_discard || held_stale_read_discarded(); }
auto NativeAudit::superseding_placeholder_observed() const noexcept -> bool {
 return acceptance_held_read && std::ranges::any_of(placeholder_slots, [this](const auto& placeholder) {
  return placeholder.first > held_generation && placeholder_cardinalities.contains(placeholder.first) && placeholder_cardinalities.at(placeholder.first) == placeholder.second.size();
 });
}
auto NativeAudit::RecordHeldControlObservation(const ExploreAcceptanceGate::ControlObservation observation) noexcept -> void {
 using Event = ExploreAcceptanceGate::ControlEvent;
 if (observation.event == Event::InitialWait) return;
 const bool valid_identity = observation.generation != 0U && observation.slot < mmltk::controller::kExploreVisibleItemCapacity &&
                             observation.compiled_index <= std::numeric_limits<std::uint32_t>::max() && observation.staging_bytes != 0U;
 if (!valid_identity) {
  reject_causal_evidence("held control identity");
  return;
 }
 if (acceptance_held_read &&
     (held_generation != observation.generation || held_slot != observation.slot || held_compiled_index != observation.compiled_index || held_capacity != observation.staging_bytes)) {
  reject_causal_evidence("held control identity changed");
  return;
 }
 acceptance_held_read = true;
 acceptance_held_completed = true;
 held_generation = observation.generation;
 held_slot = observation.slot;
 held_compiled_index = observation.compiled_index;
 held_capacity = observation.staging_bytes;
 if (observation.event == Event::HeldProceed)
  acceptance_held_released = true;
 else if (observation.event == Event::HeldStale)
  acceptance_held_stale = true;
 else if (observation.event != Event::HeldWait)
  reject_causal_evidence("held control event");
}
auto NativeAudit::causal_stale_blocker(const FinalCursorGenerations final) const noexcept -> std::string_view {
 if (!acceptance_held_read) return "held read";
 if (!acceptance_held_completed) return "held read completion";
 if (!held_stale_read_discarded()) return "released held read discard";
 if (final.material <= held_generation || final.cursor <= held_generation) return "final generation order";
 if (!placeholder_slots.contains(final.material)) return "final material placeholder";
 if (!placeholder_cardinalities.contains(final.material) || placeholder_cardinalities.at(final.material) != placeholder_slots.at(final.material).size())
  return "final material placeholder completeness";
 if (!superseding_placeholder_observed()) return "superseding placeholder";
 return {};
}
auto NativeAudit::causal_stale_chain(const FinalCursorGenerations final) const noexcept -> bool { return causal_stale_blocker(final).empty(); }
auto NativeAudit::exact_partial_slot_identity() const noexcept -> bool {
 const auto placeholder = placeholder_slots.find(partial_generation);
 const auto patched = patched_slots.find(partial_generation);
 // A newer viewport may supersede unfinished slots or an unpublished batch.
 return placeholder != placeholder_slots.end() && patched != patched_slots.end() && !patched->second.empty() && std::ranges::includes(placeholder->second, patched->second);
}
auto NativeAudit::augmentation_generation_for(const std::uint64_t seed, const std::uint64_t frame_revision) const noexcept -> std::optional<std::uint64_t> {
 if (frame_revision == 0U) return std::nullopt;
 const auto generation = std::ranges::find_if(augmentation_seeds, [this, seed, frame_revision](const auto& candidate) {
  const auto frames = published_frames.find(candidate.first);
  return candidate.second == seed && frames != published_frames.end() && std::ranges::any_of(frames->second, [frame_revision](const auto& frame) { return frame.second == frame_revision; });
 });
 return generation == augmentation_seeds.end() ? std::nullopt : std::optional{generation->first};
}
auto NativeAudit::augmentation_pixels_observed(const std::uint64_t seed) const noexcept -> bool {
 return std::ranges::any_of(augmentation_seeds, [this, seed](const auto& generation) {
  const auto pixels = augmentation_pixel_seeds.find(generation.first);
  const auto frames = published_frames.find(generation.first);
  return generation.second == seed && pixels != augmentation_pixel_seeds.end() && pixels->second == seed && frames != published_frames.end() && !frames->second.empty();
 });
}
auto NativeAudit::aligned_rendered_probe(const std::pair<std::uint64_t, std::uint64_t> key) const noexcept -> bool {
 const auto rendered = rendered_probe_slots.find(key);
 const auto selected = selected_overlay_slots.find(key);
 const auto placeholder = placeholder_slots.find(key.first);
 return rendered != rendered_probe_slots.end() && padded_card_slots.contains(key) && padding_orientations.contains(key) && padded_card_slots.at(key) == rendered->second &&
        transition_probe_slots.contains(key) && transition_probe_slots.at(key) == rendered->second && rendered_probe_frames.contains(key) &&
        std::ranges::any_of(rendered_probe_frames.at(key), [](const auto revision) { return revision != 0U; }) && placeholder != placeholder_slots.end() && placeholder->second.contains(key.second) &&
        placeholder->second.at(key.second) == rendered->second && selected != selected_overlay_slots.end() && selected->second.second == rendered->second && transformed_overlay_slots.contains(key) &&
        semantic_overlay_slots.contains(key);
}
auto NativeAudit::filtered_overlay_identity() const noexcept -> bool {
 return std::ranges::any_of(selected_overlay_slots, [this](const auto& selected) {
  return std::ranges::any_of(hidden_overlay_slots, [&selected](const auto& hidden) { return hidden.first.first != selected.first.first && hidden.second.first == selected.second.first; });
 });
}
auto NativeAudit::aligned_padding_orientation(const PaddingOrientation orientation) const noexcept -> bool {
 return std::ranges::any_of(rendered_probe_slots,
  [this, orientation](const auto& probe) { return padding_orientations.contains(probe.first) && padding_orientations.at(probe.first) == orientation && aligned_rendered_probe(probe.first); });
}
auto NativeAudit::aligned_overlay_pixels() const noexcept -> bool {
 return filtered_overlay_identity() && aligned_padding_orientation(PaddingOrientation::Vertical) && aligned_padding_orientation(PaddingOrientation::Horizontal);
}
auto NativeAudit::overlay_readiness_blocker(const bool require_pixel_probes) const noexcept -> std::string_view {
 if (!overlay_descriptors) return "overlay descriptors";
 if (!donor_descriptors) return "donor descriptors";
 if (!require_pixel_probes) return {};
 if (!filtered_overlay_identity()) return "filtered overlay identity";
 if (!aligned_padding_orientation(PaddingOrientation::Vertical)) return "vertical rendered overlay";
 if (!aligned_padding_orientation(PaddingOrientation::Horizontal)) return "horizontal rendered overlay";
 return {};
}
auto NativeAudit::readiness_blocker(const FinalCursorGenerations final, const bool seeded_augmentation_ready, const bool require_overlay_pixel_probes) const noexcept -> std::string_view {
 const auto overlay_blocker = overlay_readiness_blocker(require_overlay_pixel_probes);
 const std::array checks{
  std::pair{server_started && peer_opened && firefox_pid > 0 && active_peer(), std::string_view{"browser peer"}},
  std::pair{explore_rendered && explore_placeholder, std::string_view{"Explore publication"}},
  std::pair{annotation_copied && annotation_opened && annotation_edited, std::string_view{"Annotation lifecycle"}},
  std::pair{presentation_ready, std::string_view{"Presentation timeline"}},
  std::pair{explore_partial_patch && acceptance_first_patch_exact && explore_ready_batch, std::string_view{"incremental Explore publication"}},
  std::pair{stale_thumbnail_discarded(), std::string_view{"stale thumbnail discard"}},
  std::pair{causal_stale_chain(final), causal_stale_blocker(final).empty() ? std::string_view{"causal stale-read chain"} : causal_stale_blocker(final)},
  std::pair{exact_partial_slot_identity(), std::string_view{"partial-slot identity"}},
  std::pair{overlay_blocker.empty(), overlay_blocker},
  std::pair{seeded_augmentation_ready, std::string_view{"seeded augmentation pixel publication"}},
  std::pair{!worker_failed && !invalid_message && !peer_replaced && !interaction_rejected, std::string_view{"runtime validity"}},
  std::pair{!explore_tile_regressed && !explore_nproc_changed && !explore_stale_patch && !causal_inconsistent, std::string_view{"Explore causal validity"}},
 };
 const auto blocker = std::ranges::find_if(checks, [](const auto& check) { return !check.first; });
 return blocker == checks.end() ? std::string_view{} : blocker->second;
}
auto NativeAudit::product_completed(const FinalCursorGenerations final, const bool seeded_augmentation_ready, const bool require_overlay_pixel_probes) const noexcept -> bool {
 return server_started && peer_opened && firefox_pid > 0 && explore_rendered && annotation_copied && annotation_opened && annotation_edited && presentation_ready && explore_placeholder &&
        explore_partial_patch && acceptance_first_patch_exact && explore_ready_batch && stale_thumbnail_discarded() && causal_stale_chain(final) && exact_partial_slot_identity() &&
        overlay_descriptors && donor_descriptors && (!require_overlay_pixel_probes || aligned_overlay_pixels()) && seeded_augmentation_ready && !worker_failed && !explore_tile_regressed &&
        !explore_nproc_changed && !explore_stale_patch && !invalid_message && !peer_replaced && !interaction_rejected && !causal_inconsistent;
}
auto NativeAudit::product_ready(const FinalCursorGenerations final, const bool seeded_augmentation_ready, const bool require_overlay_pixel_probes) const noexcept -> bool {
 return product_completed(final, seeded_augmentation_ready, require_overlay_pixel_probes) && active_peer();
}
auto NativeAudit::failed_before_termination() const noexcept -> bool { return worker_failed || invalid_message || peer_replaced || interaction_rejected || causal_inconsistent; }
auto NativeAudit::failure_blocker() const noexcept -> std::string_view {
 const std::array checks{
  std::pair{worker_failed, std::string_view{"worker failure"}},
  std::pair{invalid_message, std::string_view{"invalid browser message"}},
  std::pair{peer_replaced, std::string_view{"browser peer replacement"}},
  std::pair{interaction_rejected, std::string_view{"browser interaction rejected"}},
  std::pair{causal_inconsistent, causal_failure.empty() ? std::string_view{"causal evidence inconsistency"} : causal_failure},
 };
 const auto blocker = std::ranges::find_if(checks, [](const auto& check) { return check.first; });
 return blocker == checks.end() ? std::string_view{} : blocker->second;
}
auto NativeAudit::active_peer() const noexcept -> bool { return peer_open_count == peer_close_count + 1U; }
}  // namespace mmltk::acceptance::wayland
