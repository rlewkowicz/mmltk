#include "audit_facts.h"
#include <nlohmann/json.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cctype>
#include "src/controller/contracts/diagnostic_context.h"
#include "src/controller/contracts/workspace_input.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <limits>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "surface_audit.h"
#include "pixel_audit.h"
#include "native_audit.h"
#include "browser_audit.h"
namespace mmltk::acceptance::wayland {
TEST_CASE("Validate to Explore requires real gallery pixel evidence", "[workspace][audit][pixel]") {
 const auto defect = GENERATE("none", "control", "source", "presentation", "sampled", "visible", "placeholder", "background", "missing_tile", "unbounded");
 BrowserAudit audit;
 nlohmann::json record{{"event", "integration.workflow.pixels"}, {"detail", "validate-to-explore"}, {"control", "explore.gallery.workspace"},
  {"a", 7U}, {"b", 9U}, {"c", 64U}, {"d", 48U}, {"ready_tile", true}, {"matched", true}, {"compiled_index", 0U}, {"sample_width", 8U}, {"sample_height", 8U}};
 if (std::string_view{defect} == "control") record["control"] = "validate.detail.image";
 if (std::string_view{defect} == "source") record["a"] = 0U;
 if (std::string_view{defect} == "presentation") record["b"] = 0U;
 if (std::string_view{defect} == "sampled") record["c"] = 0U;
 if (std::string_view{defect} == "visible") record["d"] = 11U;
 if (std::string_view{defect} == "placeholder") record["ready_tile"] = false;
 if (std::string_view{defect} == "background") { record["d"] = 0U; record["matched"] = false; }
 if (std::string_view{defect} == "missing_tile") record.erase("compiled_index");
 if (std::string_view{defect} == "unbounded") { record["sample_width"] = 100U; record["c"] = 800U; record["d"] = 800U; }
 audit.consume(record);
 CHECK(audit.validate_to_explore_pixels == (std::string_view{defect} == "none"));
}

TEST_CASE("display confidence audit requires exact edits and reversible paired pixels", "[workspace][audit][validation]") {
 BrowserAudit audit;
 for (unsigned stage = 0U; stage < 9U; ++stage) {
  const double value = stage < 6U ? 0.437 : stage == 7U ? 1.0 : 0.0;
  const unsigned revision = stage < 6U ? 2U : stage - 3U;
  audit.consume({{"event", "integration.validation_confidence_edit"}, {"a", stage}, {"b", value}, {"c", revision}, {"d", 1U}});
  if (stage >= 6U) audit.consume({{"event", "integration.validation_confidence_pixels"}, {"control", "validate.samples.atlas"},
   {"stage", stage}, {"threshold", value}, {"detections", 300U}, {"minimum", 0.01}, {"maximum", 0.25}, {"clean", 8U},
   {"generation", 1U}, {"revision", revision}, {"different", stage == 7U ? 50U : 0U}, {"matched", true}});
 }
 REQUIRE(audit.validation_confidence_complete());
 SECTION("rounded value") { audit.validation_confidence_edits[0]["b"] = 0.44; }
 SECTION("arrow increment") { audit.validation_confidence_edits[3]["b"] = 1.0; }
 SECTION("extra persistence") { audit.validation_confidence_edits[4]["c"] = 3U; }
 SECTION("inference restarted") { audit.validation_confidence_pixels[1]["generation"] = 2U; }
 SECTION("clean regenerated") { audit.validation_confidence_pixels[1]["clean"] = 9U; }
 SECTION("no removed pixels") { audit.validation_confidence_pixels[1]["different"] = 0U; }
 SECTION("lower threshold did not restore") { audit.validation_confidence_pixels[2]["different"] = 20U; }
 SECTION("missing proof") { audit.validation_confidence_pixels.pop_back(); }
 CHECK_FALSE(audit.validation_confidence_complete());
}

TEST_CASE("validation controls require rendered names right-preview placement and narrow stacking", "[workspace][audit][validation]") {
 BrowserAudit audit;
 for (const char* label : {"Groundtruth", "Detections", "Display confidence", "Preview only"})
  audit.consume({{"event", "integration.validation_text"}, {"control", label}});
 for (const char* stage : {"atlas", "narrow"}) {
  audit.validation_layout[{stage, "validate.samples.atlas"}] = {500, 100, 300, 400};
  audit.validation_layout[{stage, "validate.gt.group"}] = {500, 510, 250, 47};
  audit.validation_layout[{stage, "validate.pred.group"}] = {500, 577, 250, 47};
 }
 REQUIRE(audit.validation_layout_complete());
 SECTION("old label") { audit.validation_text.erase("Groundtruth"); }
 SECTION("left preview") { audit.validation_layout[{"atlas", "validate.gt.group"}][0] = 100; }
 SECTION("missing spacing") { audit.validation_layout[{"atlas", "validate.pred.group"}][1] = 557; }
 SECTION("unstacked narrow") { audit.validation_layout[{"narrow", "validate.pred.group"}] = {770, 510, 250, 47}; }
 CHECK_FALSE(audit.validation_layout_complete());
}

TEST_CASE("benchmark radio audit requires every real choice, current visibility and settled restoration", "[workspace][audit][benchmark]") {
 const bool baseline_enabled = GENERATE(false, true);
 const unsigned baseline_dataset = GENERATE(0U, 1U);
 const unsigned baseline_validation = GENERATE(0U, 1U, 2U);
 constexpr const char* benchmark_override = "train.dataset.benchmark_override";
 BrowserAudit audit;
 const auto record = [&](const char* event, const std::string& control, const char* detail, std::array<double, 4> values) {
  audit.consume({{"event", event}, {"control", control}, {"detail", detail}, {"a", static_cast<std::uint64_t>(values[0])}, {"b", values[1]}, {"c", values[2]}, {"d", values[3]}});
 };
 record("integration.benchmark_baseline", benchmark_override, "native-settled", {baseline_enabled ? 1.0 : 0.0, double(baseline_dataset), double(baseline_validation), 10.0});
 constexpr std::array controls{
  "train.dataset.benchmark.custom", "train.dataset.benchmark.coconut", "train.dataset.validation.coconut", "train.dataset.validation.stock", "train.dataset.validation.coconut_stock"};
 struct Step {
  const char* control;
  unsigned dataset;
  unsigned validation;
  bool enabled;
 };
 const std::array sequence{Step{benchmark_override, baseline_dataset, baseline_validation, true}, Step{controls[1], 1U, baseline_validation, true}, Step{controls[3], 1U, 1U, true},
  Step{controls[4], 1U, 2U, true}, Step{controls[2], 1U, 0U, true}, Step{controls[0], 0U, 0U, true}, Step{controls[1], 1U, 0U, true}, Step{"train.dataset.browse", 1U, 0U, true},
  Step{controls[2U + baseline_validation], 1U, baseline_validation, true}, Step{controls[baseline_dataset], baseline_dataset, baseline_validation, true},
  Step{benchmark_override, baseline_dataset, baseline_validation, baseline_enabled}, Step{benchmark_override, baseline_dataset, baseline_validation, false}};
 for (std::size_t index = 0; index != sequence.size(); ++index) {
  const auto& step = sequence[index];
  record("integration.benchmark_click", step.control, "real-click", {double(index), 1.0, 0.0, 0.0});
  record("integration.benchmark_choice", step.control, "native-settled", {double(index), double(step.dataset), double(step.validation), 11.0 + double(index)});
  for (std::size_t choice = 0; choice != controls.size(); ++choice) {
   const double present = step.enabled && (choice < 2U || step.dataset == 1U) ? 1.0 : 0.0;
   record("integration.benchmark_visibility", controls[choice], "current-tree", {double(index), present, present, 11.0 + double(index)});
  }
 }
 record("integration.benchmark_inactive", "train.dataset.browse", "unchanged", {7.0, 1.0, 18.0, 0.0});
 REQUIRE(audit.benchmark_choices_complete());
 if (baseline_enabled) {
  auto unchanged = audit;
  unchanged.benchmark_choices.at(0U).second[3] = 10.0;
  for (const char* control : controls) unchanged.benchmark_visibility.at({0U, control})[3] = 10.0;
  CHECK(unchanged.benchmark_choices_complete());
 }
 SECTION("missing inactive evidence") { audit.benchmark_inactive = false; }
 SECTION("missing option") { audit.benchmark_choices.erase(3U); }
 SECTION("missing click") { audit.benchmark_clicks.erase(4U); }
 SECTION("missing hidden evidence") { audit.benchmark_visibility.erase({11U, controls[0]}); }
 SECTION("cumulative rendered control cannot prove hidden") { audit.benchmark_visibility.at({11U, controls[0]})[2] = 1.0; }
 SECTION("unsettled choice") { audit.benchmark_choices.at(2U).second[3] = 10.0; }
 SECTION("wrong retained validation") { audit.benchmark_choices.at(10U).second[2] = 4.0; }
 SECTION("missing original state") { audit.benchmark_baseline.reset(); }
 SECTION("local compile handoff missing") { audit.benchmark_choices.erase(11U); }
 CHECK_FALSE(audit.benchmark_choices_complete());
}
TEST_CASE("browser adapter evidence requires the compositor display device", "[workspace][audit][device]") {
 BrowserAudit audit;
 audit.consume({{"event", "firefox.adapter.selected"}, {"display_pci_bus_id", "0000:09:00.0"}, {"adapter_pci_bus_id", "0000:09:00.0"}});
 CHECK_FALSE(audit.failed_before_termination());
 const auto selected = GENERATE("", "0000:02:00.0");
 audit.consume({{"event", "firefox.adapter.selected"}, {"display_pci_bus_id", "0000:09:00.0"}, {"adapter_pci_bus_id", selected}});
 CHECK(audit.failed_before_termination());
}
TEST_CASE("pixel evidence joins exact physical samples and includes alpha", "[workspace][audit][pixel]") {
 const nlohmann::json native{{"event", "presentation.pixel"}, {"surface_high", 1U}, {"surface_low", 2U}, {"presentation_revision", 7U}, {"source_session", 1U}, {"source_instance", 3U},
  {"source_revision", 9U}, {"clean_revision", 8U}, {"source_observation_revision", 11U}, {"source_width", 384U}, {"source_height", 384U}, {"content_width", 384U}, {"content_height", 384U},
  {"capacity_width", 894U}, {"capacity_height", 1080U}, {"allocation_generation", 2U}, {"transfer_sequence", 4U}, {"timeline_ready", 7U}, {"workspace_source_high", 3U}, {"workspace_source_low", 4U},
  {"workspace_allocation", 8U}, {"sample_rgba", 0xff705030U}};
 const std::string surface = SurfaceAudit::native_identity(native);
 const auto pixel = [&](const char* event, const char* boundary) {
  nlohmann::json record{{"event", event}, {"boundary", boundary}, {"surface", surface}, {"source", "00000000000000030000000000000004"}, {"presentation_revision", 7U}, {"content_session", 1U},
   {"content_sequence", 9U}, {"frame_revision", 9U}, {"content_width", 384U}, {"content_height", 384U}, {"width", 894U}, {"height", 1080U}, {"transfer_sequence", 4U}, {"timeline_ready", 7U},
   {"timeline_release", 8U}, {"layer", 0U}, {"slot", 1U}, {"sample_rgba", 0xff705030U}};
  if (std::string_view{event}.starts_with("iced.surface.")) {
   record.erase("transfer_sequence");
   record.erase("timeline_ready");
   record.erase("timeline_release");
  }
  return record;
 };
 auto imported = pixel("firefox.workspace.pixel", "import");
 auto mailbox = pixel("firefox.workspace.pixel", "mailbox");
 auto owned = pixel("iced.surface.pixel", "");
 const auto fill = [&](PixelBoundaryAudit& audit, nlohmann::json changed, bool missing = false, bool all_black = false, std::string_view missing_owner = {}, std::uint64_t publication = 7U,
                    bool direct = false) {
  constexpr std::array<unsigned, 5> coordinates{0U, 191U, 383U, 191U, 383U};
  for (std::size_t index = 0U; index < 25U; ++index) {
   for (auto record : {changed, mailbox, imported, native}) {
    if (direct && record.value("event", "") == "firefox.workspace.pixel") continue;
    record["direct_sampling"] = direct;
    if (!missing_owner.empty() && (record.value("boundary", "") == missing_owner || record.value("event", "") == missing_owner)) continue;
    if (missing && index == 24U && record.value("event", "") == "iced.surface.pixel") continue;
    record["presentation_revision"] = publication;
    record["sample_index"] = index;
    if (!record.contains("sample_x")) record["sample_x"] = coordinates[index % 5U];
    record["sample_y"] = coordinates[index / 5U];
    if (all_black) record["sample_rgba"] = 0xff000000U;
    audit.consume(record);
   }
  }
  auto edge = native;
  edge["event"] = "presentation.frame.edge";
  edge["presentation_revision"] = publication;
  edge["direct_sampling"] = direct;
  if (missing_owner != "presentation.frame.edge") audit.consume(edge);
  auto forwarded = imported;
  forwarded["event"] = "firefox.workspace.frame_forwarded";
  forwarded["presentation_revision"] = publication;
  forwarded["direct_sampling"] = direct;
  if (missing_owner != "firefox.workspace.frame_forwarded") audit.consume(forwarded);
 };
 PixelBoundaryAudit valid;
 fill(valid, owned);
 CHECK(valid.failure.empty());
 CHECK(std::ranges::all_of(valid.joined, [](auto count) { return count != 0U; }));
 CHECK(valid.retained_logical_content);
 for (const bool direct : {false, true}) {
  PixelBoundaryAudit unforwarded;
  fill(unforwarded, owned, false, false, "firefox.workspace.frame_forwarded", 7U, direct);
  CHECK(unforwarded.failure.empty());
  CHECK_FALSE(unforwarded.raw_complete());
  CHECK_FALSE(unforwarded.copy_probe_omission_proven());
 }
 for (const std::string_view fault : {"none", "alpha", "missing", "copy", "source", "mode"}) {
  INFO("direct pixel fault: " << fault);
  auto changed = owned;
  if (fault == "alpha") changed["sample_rgba"] = 0x00705030U;
  PixelBoundaryAudit direct;
  fill(direct, changed, fault == "missing", false, {}, 7U, true);
  if (fault == "copy") direct.consume(imported);
  if (fault == "source" || fault == "mode") {
   auto forwarded = imported;
   forwarded["event"] = "firefox.workspace.frame_forwarded";
   forwarded["direct_sampling"] = fault != "mode";
   if (fault == "source") forwarded["source"] = "00000000000000030000000000000005";
   direct.consume(forwarded);
  }
  CHECK(direct.raw_complete() == (fault == "none"));
  CHECK(direct.copy_probe_omission_proven() == (fault == "none"));
  CHECK(std::ranges::all_of(direct.joined, [](const auto count) { return count == 0U; }));
  if (fault == "none") {
   CHECK(direct.direct_joined == 1U);
   const auto& receipt = direct.samples.at({surface, 7U});
   CHECK(receipt.receivers[0].identity.empty());
   CHECK(receipt.receivers[1].identity.empty());
  }
 }
 for (const auto field : {"sample_rgba", "content_session", "frame_revision", "width", "height", "layer", "slot", "sample_x"}) {
  auto changed = owned;
  changed[field] = field == std::string_view{"sample_rgba"} ? 0x00705030U : 8U;
  PixelBoundaryAudit audit;
  fill(audit, changed);
  CHECK_FALSE(audit.failure.empty());
 }
 for (const bool direct : {false, true}) {
  for (const auto source : {"", "00000000000000030000000000000005"}) {
   auto changed = owned;
   changed["source"] = source;
   PixelBoundaryAudit audit;
   fill(audit, changed, false, false, {}, 7U, direct);
   CHECK_FALSE(audit.failure.empty());
   CHECK_FALSE(audit.raw_complete());
  }
  for (const auto arena : {"", "00000000000000010000000000000003"}) {
   auto changed = owned;
   changed["surface"] = arena;
   PixelBoundaryAudit audit;
   fill(audit, changed, false, false, {}, 7U, direct);
   CHECK_FALSE(audit.raw_complete());
   CHECK_FALSE(audit.retained_logical_content);
  }
 }
 PixelBoundaryAudit partial;
 fill(partial, owned, true);
 CHECK(partial.joined.back() == 0U);
 CHECK_FALSE(partial.retained_logical_content);
 for (const auto field : {"source_instance", "clean_revision", "source_observation_revision", "content_x", "allocation_generation", "capacity_width", "timeline_ready"}) {
  PixelBoundaryAudit audit;
  fill(audit, owned);
  auto edge = native;
  edge["event"] = "presentation.frame.edge";
  edge[field] = 99U;
  audit.consume(edge);
  CHECK_FALSE(audit.raw_complete());
 }
 auto canvas = pixel("iced.surface.canvas_pixel", "");
 canvas["control"] = "explore.detail.workspace";
 canvas["sample_index"] = 0U;
 canvas["sample_x"] = 0U;
 canvas["sample_y"] = 0U;
 canvas["sample_rgba"] = 0xff000000U;
 PixelBoundaryAudit black_viewer;
 fill(black_viewer, owned);
 black_viewer.consume(canvas);
 CHECK(black_viewer.viewer_canvas_joins == 1U);
 CHECK_FALSE(black_viewer.failure.empty());
 canvas["sample_rgba"] = 0xff060503U;
 PixelBoundaryAudit dimmed_viewer;
 fill(dimmed_viewer, owned);
 dimmed_viewer.consume(canvas);
 CHECK(dimmed_viewer.failure.empty());
 PixelBoundaryAudit all_black;
 canvas["sample_rgba"] = 0xff000000U;
 fill(all_black, owned, false, true);
 all_black.consume(canvas);
 all_black.consume({{"event", "integration.viewer_complete"}, {"detail", "square"}, {"a", 7U}, {"b", 9U}});
 CHECK(all_black.failure.empty());
 CHECK(all_black.raw_complete());
 CHECK_FALSE(all_black.viewer_nonblack_complete());
 canvas["sample_rgba"] = 0xff705030U;
 const nlohmann::json selected{{"event", "integration.viewer_complete"}, {"detail", "square"}, {"a", 7U}, {"b", 9U}};
 PixelBoundaryAudit colored_viewer;
 fill(colored_viewer, owned);
 colored_viewer.consume(canvas);
 colored_viewer.consume(selected);
 CHECK(colored_viewer.viewer_nonblack_complete());
 canvas["control"] = "workflow.visual.workspace";
 canvas["sample_rgba"] = 0xff000000U;
 colored_viewer.consume(canvas);
 CHECK(colored_viewer.failure.empty());
 canvas["control"] = "explore.detail.workspace";
 canvas["sample_rgba"] = 0xff705030U;
 for (const bool missing_sample : {false, true}) {
  PixelBoundaryAudit dropped_edge;
  fill(dropped_edge, owned, missing_sample, false, "presentation.frame.edge");
  dropped_edge.consume(canvas);
  dropped_edge.consume(selected);
  CHECK_FALSE(dropped_edge.viewer_nonblack_complete());
  CHECK_FALSE(dropped_edge.raw_complete());
  CHECK(dropped_edge.samples.at({surface, 7U}).native.at({"00000000000000030000000000000004", 4U}).publication_fact.empty());
  auto contradictory_edge = native;
  contradictory_edge["event"] = "presentation.frame.edge";
  contradictory_edge["source_instance"] = 4U;
  dropped_edge.consume(contradictory_edge);
  CHECK_FALSE(dropped_edge.failure.empty());
  CHECK_FALSE(dropped_edge.viewer_nonblack_complete());
 }
 for (const auto* boundary : {"Allocation", "Reset", "Begin", "End"}) {
  const bool allocation = std::string_view{boundary} == "Allocation";
  std::string target{boundary};
  target.front() = static_cast<char>(std::tolower(static_cast<unsigned char>(target.front())));
  auto audit = colored_viewer;
  auto failed = pixel("firefox.workspace.probe_failed", boundary);
  failed["presentation_revision"] = 6U;
  failed["transfer_sequence"] = 3U;
  if (allocation) failed["source"] = "00000000000000030000000000000005";
  audit.consume(failed);
  CHECK_FALSE(audit.probe_failure_complete(target));
  auto ordinary = failed;
  ordinary["event"] = "firefox.workspace.frame_forwarded";
  ordinary["pixel_probe"] = false;
  audit.consume(ordinary);
  CHECK(audit.probe_failure_complete(target));
  CHECK(audit.probe_failure_evidence(target).complete());
  auto reused_counter = audit;
  reused_counter.samples.try_emplace(PixelBoundaryAudit::Key{surface, 10U});
  CHECK(reused_counter.probe_failure_evidence(target).complete());
  auto unrelated = audit.samples.at({surface, 7U});
  unrelated.forwarded["workspace_source"] = "00000000000000030000000000000006";
  for (auto& receiver : unrelated.receivers) receiver.identity["transfer_sequence"] = scalar(failed, "transfer_sequence");
  reused_counter.samples.emplace(PixelBoundaryAudit::Key{surface, 8U}, std::move(unrelated));
  CHECK(reused_counter.probe_failure_evidence(target).complete());
  auto before_viewer = audit;
  before_viewer.successful_viewer.reset();
  CHECK(before_viewer.probe_failure_evidence(target).complete());
  CHECK_FALSE(before_viewer.probe_failure_complete(target));
  if (!allocation) {
   auto unrelated_content = audit;
   auto& publication = unrelated_content.samples.at({surface, 7U});
   const auto other_sequence = scalar(publication.receivers[0].identity, "content_sequence") + 1U;
   for (auto& [_, receipt_boundary] : publication.native) {
    receipt_boundary.identity["content_sequence"] = other_sequence;
    receipt_boundary.publication_fact["content_sequence"] = other_sequence;
   }
   for (auto& receipt_boundary : publication.receivers) receipt_boundary.identity["content_sequence"] = other_sequence;
   publication.forwarded["content_sequence"] = other_sequence;
   CHECK_FALSE(unrelated_content.probe_failure_complete(target));
   CHECK_FALSE(unrelated_content.probe_failure_evidence(target).complete());
   CHECK(audit.probe_failure_complete(target));
  }
  auto wrong_slot = audit;
  wrong_slot.probe_failures.front()["slot"] = 0U;
  if (!allocation) CHECK_FALSE(wrong_slot.probe_failure_complete(target));
  auto stale_receipt = audit;
  auto successful = failed;
  successful["event"] = "firefox.workspace.pixel";
  successful["boundary"] = "mailbox";
  successful["sample_index"] = 0U;
  successful["sample_x"] = 0U;
  successful["sample_y"] = 0U;
  stale_receipt.consume(successful);
  CHECK_FALSE(stale_receipt.probe_failure_complete(target));
  CHECK_FALSE(stale_receipt.probe_failure_evidence(target).forwarded);
  if (!allocation) {
   auto no_recovery = audit;
   no_recovery.samples.at({surface, 7U}).counted.fill(false);
   CHECK_FALSE(no_recovery.probe_failure_complete(target));
   CHECK(no_recovery.probe_failure_evidence(target).forwarded);
   CHECK_FALSE(no_recovery.probe_failure_evidence(target).recovered);
  }
 }
 for (const auto missing_owner : {"import", "mailbox", "iced.surface.pixel"}) {
  PixelBoundaryAudit audit;
  fill(audit, owned, false, false, {}, 8U);
  fill(audit, owned, false, false, missing_owner);
  audit.consume(canvas);
  audit.consume(selected);
  CHECK(audit.raw_complete());
  CHECK_FALSE(audit.viewer_nonblack_complete());
 }
 PixelBoundaryAudit wrong_transfer;
 fill(wrong_transfer, owned);
 auto reoffer = imported;
 reoffer["transfer_sequence"] = 5U;
 reoffer["timeline_ready"] = 9U;
 reoffer["timeline_release"] = 10U;
 reoffer["sample_index"] = 0U;
 reoffer["sample_x"] = 0U;
 reoffer["sample_y"] = 0U;
 wrong_transfer.consume(reoffer);
 wrong_transfer.consume(canvas);
 wrong_transfer.consume(selected);
 CHECK_FALSE(wrong_transfer.viewer_nonblack_complete());
}
TEST_CASE("composition evidence requires every card and kind in each column publication", "[workspace][audit][pixel]") {
 const auto fill = [](PixelBoundaryAudit& audit, std::string_view defect) {
  for (const auto columns : {4U, 10U}) {
   for (const auto card : {3U, 8U}) {
    for (unsigned kind = 0U; kind < 4U; ++kind) {
     if (defect == "missing" && columns == 10U && card == 8U && kind == 3U) continue;
     nlohmann::json record{{"event", "integration.atlas_composition"}, {"surface", "surface"}, {"columns", columns}, {"presentation_revision", columns}, {"card", card}, {"kind", kind},
      {"content_width", 100U}, {"content_height", 100U}, {"image_x", 0.0}, {"image_y", 0.0}, {"image_width", 100.0}, {"image_height", 100.0}, {"sample_x", 10.5}, {"sample_y", 10.5},
      {"canvas_x", 10.5}, {"canvas_y", 10.5}, {"expected", {120, 80, 40, 255}}, {"observed", {120, 80, 40, 255}}};
     if (defect == "mask-as-box" && kind == 2U) record["observed"] = {74, 80, 86, 255};
     if (defect == "mixed" && kind == 3U) record["presentation_revision"] = 99U;
     audit.consume(record);
    }
   }
   audit.consume({{"event", "integration.atlas_composition_complete"}, {"surface", "surface"}, {"columns", columns}, {"presentation_revision", columns}, {"cards", {3U, 8U}}, {"emitted", 8U},
    {"content_width", 100U}, {"content_height", 100U}});
  }
 };
 PixelBoundaryAudit valid;
 fill(valid, "");
 CHECK(valid.composition_complete());
 for (const auto defect : {"missing", "mask-as-box", "mixed"}) {
  PixelBoundaryAudit audit;
  fill(audit, defect);
  CHECK_FALSE(audit.composition_complete());
 }
}
TEST_CASE("viewer continuity requires mapped routes foreground persistence and the same restored product", "[workspace][audit][pixel]") {
 const auto fill = [](PixelBoundaryAudit& audit, std::string_view defect, bool persistence) {
  const auto event = [&](const char* name, const char* control = "", const char* detail = "", unsigned a = 0U, unsigned b = 0U, unsigned c = 0U, unsigned d = 0U) {
   audit.consume({{"event", name}, {"control", control}, {"detail", detail}, {"a", a}, {"b", b}, {"c", c}, {"d", d}});
  };
  for (unsigned kernel = 0U; kernel < 3U; ++kernel) event("integration.upscale_cached", "", "", 0U, 0U, kernel);
  event("integration.viewer_settings_preserved");
  event("integration.viewer_departure_started", "", "", 20U, persistence ? 1U : 0U, 30U);
  audit.consume({{"event", "upscale.stop.requested"}, {"observation_revision", 20U}});
  if (defect == "duplicate-stop") audit.consume({{"event", "upscale.stop.requested"}, {"observation_revision", 20U}});
  for (const bool train : {true, false}) {
   const auto* route = train ? "navigation.train" : "navigation.explore";
   if (defect != "direct") event("integration.navigation_message", route, train ? "Explore" : "Train");
   if (defect != "missing-outcome") event("integration.navigation_outcome", route, train ? "Train" : "Explore");
   if (persistence && defect != "missing-persistence") event("integration.route_state", route, "settings.reply", 1U);
   event("integration.viewer_route_confirmed", route, defect == "foreground" ? "Annotation" : train ? "None" : "Upscale", train ? 31U : 32U, persistence ? 1U : 0U, 1U);
   if (train) event("integration.viewer_abandoned", "", "", 21U);
  }
  if (defect != "missing-basic") event("integration.viewer_basic_reentry", "", "automatic-completed-draw", 41U, 50U, 3U, 40U);
  event("integration.viewer_reconnected", "", "matching-completed-draw", defect == "different-product" ? 42U : 41U, 51U, 3U, 40U);
  if (defect != "missing-gallery") event("integration.explore_reopened", "", "usable-after-reopen", 60U, 61U, 10U, 128U);
 };
 for (const bool persistence : {false, true}) {
  PixelBoundaryAudit valid;
  fill(valid, "", persistence);
  CHECK(valid.continuity_complete());
 }
 for (const auto defect : {"direct", "missing-outcome", "foreground", "missing-persistence", "duplicate-stop", "missing-basic", "different-product", "missing-gallery"}) {
  PixelBoundaryAudit audit;
  fill(audit, defect, true);
  CHECK_FALSE(audit.continuity_complete());
 }
}
void add_rendered_probe_audit_fixture(NativeAudit& audit, const NativeAudit::PaddingOrientation orientation, const std::uint64_t generation, const std::uint64_t slot,
 const std::uint64_t compiled_index, const std::uint64_t frame_revision) {
 const auto key = std::pair{generation, slot};
 audit.padded_card_slots.emplace(key, compiled_index);
 audit.padding_orientations.emplace(key, orientation);
 audit.rendered_probe_slots.emplace(key, compiled_index);
 audit.transition_probe_slots.emplace(key, compiled_index);
 audit.rendered_probe_frames.emplace(key, std::vector{frame_revision});
 audit.selected_overlay_slots.emplace(key, NativeAudit::OverlayDescriptorIdentity{1U, compiled_index});
 audit.hidden_overlay_slots.emplace(std::pair{generation - 1U, slot + 1U}, NativeAudit::OverlayDescriptorIdentity{1U, compiled_index + 100U});
 audit.transformed_overlay_slots.emplace(key);
 audit.semantic_overlay_slots.emplace(key);
 audit.placeholder_slots[generation].emplace(slot, compiled_index);
 audit.patched_slots[generation].emplace(slot, compiled_index);
}
[[nodiscard]] NativeAudit rendered_probe_audit_fixture(const bool vertical = true, const bool horizontal = true) {
 NativeAudit audit;
 if (vertical) add_rendered_probe_audit_fixture(audit, NativeAudit::PaddingOrientation::Vertical, 7U, 2U, 11U, 50U);
 if (horizontal) add_rendered_probe_audit_fixture(audit, NativeAudit::PaddingOrientation::Horizontal, 8U, 3U, 12U, 51U);
 return audit;
}
void rendered_probe_audit_rejects_mismatched_identity() {
 NativeAudit self_contained;
 self_contained.consume(
  {{"kind", "gui_runtime"}, {"owner", "explore"}, {"event", "explore.card.rendered_probe"}, {"sequence", 7U}, {"value", 2U}, {"detail", 11U}, {"capacity_width", 31U}, {"capacity_height", 3916U},
   {"staging_bytes", (89ULL << 32U) | (1ULL << 16U) | 1ULL}, {"source_width", 89U}, {"source_height", 89U}, {"content_x", 0U}, {"content_y", 22U}, {"content_width", 89U}, {"content_height", 45U}});
 CHECK_FALSE(self_contained.causal_inconsistent);
 CHECK(self_contained.rendered_probe_slots.at({7U, 2U}) == 11U);
 CHECK(self_contained.padding_orientations.at({7U, 2U}) == NativeAudit::PaddingOrientation::Vertical);
 const auto publish_frame = [](NativeAudit& audit, const std::uint64_t generation, const std::uint64_t revision) {
  audit.consume({{"kind", "gui_runtime"}, {"owner", "explore"}, {"event", "explore.frame.published"}, {"sequence", generation}, {"value", revision}});
 };
 NativeAudit incomplete_pair;
 incomplete_pair.placeholder_slots[7U].emplace(2U, 11U);
 incomplete_pair.rendered_probe_slots.emplace(std::pair{7U, 2U}, 11U);
 publish_frame(incomplete_pair, 7U, 50U);
 CHECK_FALSE(incomplete_pair.causal_inconsistent);
 CHECK_FALSE(incomplete_pair.rendered_probe_frames.contains({7U, 2U}));
 incomplete_pair.transition_probe_slots.emplace(std::pair{7U, 2U}, 11U);
 incomplete_pair.reconcile_rendered_probes(7U);
 CHECK_FALSE(incomplete_pair.causal_inconsistent);
 CHECK(incomplete_pair.rendered_probe_frames.at({7U, 2U}) == std::vector{std::uint64_t{50U}});
 NativeAudit missing_patch;
 missing_patch.placeholder_slots[7U].emplace(2U, 11U);
 missing_patch.rendered_probe_slots.emplace(std::pair{7U, 2U}, 11U);
 missing_patch.transition_probe_slots.emplace(std::pair{7U, 2U}, 11U);
 publish_frame(missing_patch, 7U, 50U);
 CHECK_FALSE(missing_patch.causal_inconsistent);
 CHECK(missing_patch.rendered_probe_frames.at({7U, 2U}) == std::vector{std::uint64_t{50U}});
 NativeAudit coalesced_publications;
 const auto coalesced_key = std::pair{7U, 2U};
 coalesced_publications.placeholder_slots[7U].emplace(2U, 11U);
 coalesced_publications.rendered_probe_slots.emplace(coalesced_key, 11U);
 coalesced_publications.transition_probe_slots.emplace(coalesced_key, 11U);
 coalesced_publications.rendered_probe_ordinals.emplace(coalesced_key, 2U);
 coalesced_publications.transition_probe_ordinals.emplace(coalesced_key, 3U);
 coalesced_publications.published_frames[7U] = {{1U, 49U}, {4U, 50U}, {5U, 51U}};
 coalesced_publications.reconcile_rendered_probes(7U);
 CHECK(coalesced_publications.rendered_probe_frames.at(coalesced_key) == std::vector{std::uint64_t{50U}, std::uint64_t{51U}});
 NativeAudit conflicting_pair;
 conflicting_pair.placeholder_slots[7U].emplace(2U, 11U);
 conflicting_pair.patched_slots[7U].emplace(2U, 12U);
 conflicting_pair.rendered_probe_slots.emplace(std::pair{7U, 2U}, 11U);
 conflicting_pair.transition_probe_slots.emplace(std::pair{7U, 2U}, 11U);
 publish_frame(conflicting_pair, 7U, 50U);
 CHECK(conflicting_pair.causal_inconsistent);
 CHECK(conflicting_pair.causal_failure == "rendered probe patch identity");
 CHECK(rendered_probe_audit_fixture().aligned_overlay_pixels());
 const auto vertical_only = rendered_probe_audit_fixture(true, false);
 CHECK(vertical_only.aligned_padding_orientation(NativeAudit::PaddingOrientation::Vertical));
 CHECK_FALSE(vertical_only.aligned_padding_orientation(NativeAudit::PaddingOrientation::Horizontal));
 CHECK_FALSE(vertical_only.aligned_overlay_pixels());
 const auto horizontal_only = rendered_probe_audit_fixture(false, true);
 CHECK(horizontal_only.aligned_padding_orientation(NativeAudit::PaddingOrientation::Horizontal));
 CHECK_FALSE(horizontal_only.aligned_padding_orientation(NativeAudit::PaddingOrientation::Vertical));
 CHECK_FALSE(horizontal_only.aligned_overlay_pixels());
 auto stale_generation = rendered_probe_audit_fixture();
 stale_generation.transition_probe_slots.erase({7U, 2U});
 stale_generation.transition_probe_slots.emplace(std::pair{8U, 2U}, 11U);
 CHECK_FALSE(stale_generation.aligned_overlay_pixels());
 auto mismatched_slot = rendered_probe_audit_fixture();
 mismatched_slot.placeholder_slots.at(7U).clear();
 mismatched_slot.placeholder_slots.at(7U).emplace(3U, 11U);
 CHECK_FALSE(mismatched_slot.aligned_overlay_pixels());
 auto mismatched_compiled_index = rendered_probe_audit_fixture();
 mismatched_compiled_index.rendered_probe_slots.at({7U, 2U}) = 12U;
 CHECK_FALSE(mismatched_compiled_index.aligned_overlay_pixels());
 auto mismatched_orientation = rendered_probe_audit_fixture();
 mismatched_orientation.padding_orientations.at({7U, 2U}) = NativeAudit::PaddingOrientation::Horizontal;
 CHECK_FALSE(mismatched_orientation.aligned_overlay_pixels());
 auto mismatched_frame = rendered_probe_audit_fixture();
 mismatched_frame.rendered_probe_frames.at({7U, 2U}) = {0U};
 CHECK_FALSE(mismatched_frame.aligned_overlay_pixels());
 BrowserAudit browser;
 browser.surface_geometries.push_back({.presentation_revision = 70U, .source_revision = 50U, .width = 640.0, .height = 320.0});
 browser.surface_geometries.push_back({.presentation_revision = 71U, .source_revision = 51U, .width = 640.0, .height = 320.0});
 browser.surface_scales.emplace(std::pair{70U, 50U}, 1.0);
 browser.surface_scales.emplace(std::pair{71U, 51U}, 1.0);
 browser.surface_draws.emplace(70U);
 browser.surface_draws.emplace(71U);
 browser.explore_slots[9U].emplace(2U, 11U);
 browser.explore_slot_frames.emplace(9U, 50U);
 browser.explore_slots[10U].emplace(3U, 12U);
 browser.explore_slot_frames.emplace(10U, 51U);
 CHECK(browser.observed_frame_revision_for_slots({{2U, 11U}}, 50U));
 CHECK(browser.observed_frame_revision_for_slots({{3U, 12U}}, 51U));
 CHECK_FALSE(browser.observed_frame_revision_for_slots({{2U, 11U}}, 51U));
 CHECK_FALSE(browser.observed_frame_revision_for_slots({{3U, 11U}}, 50U));
 CHECK_FALSE(browser.observed_frame_revision_for_slots({{2U, 12U}}, 50U));
 browser.surface_draws.erase(70U);
 CHECK(browser.observed_frame_revision_for_slots({{2U, 11U}}, 50U));
 CHECK_FALSE(browser.rendered_frame_for_slots({{2U, 11U}}, 0U, 0U));
 CHECK(browser.rendered_frame_for_slots({{3U, 12U}}, 0U, 0U));
}
TEST_CASE("native probe joins retain strict ordinal ranges at maximum generation", "[workspace][audit]") {
 const auto generation = std::numeric_limits<std::uint64_t>::max();
 const auto key = std::pair{generation, std::uint64_t{2U}};
 NativeAudit audit;
 audit.placeholder_slots[generation].emplace(2U, 11U);
 audit.rendered_probe_slots[key] = 11U;
 audit.transition_probe_slots[key] = 11U;
 audit.rendered_probe_ordinals[key] = 3U;
 audit.transition_probe_ordinals[key] = 5U;
 audit.rendered_probe_slots[{generation - 1U, 2U}] = 99U;
 audit.reconcile_rendered_probes(generation);
 CHECK(audit.rendered_probe_frames.empty());
 audit.published_frames[generation] = {{3U, 30U}, {4U, 40U}, {5U, 50U}};
 audit.reconcile_rendered_probes(generation);
 CHECK(audit.rendered_probe_frames.empty());
 audit.published_frames[generation].insert(audit.published_frames[generation].begin(), {{1U, 10U}, {2U, 20U}});
 audit.reconcile_rendered_probes(generation);
 CHECK(audit.rendered_probe_frames.at(key) == std::vector<std::uint64_t>{20U});
 audit.published_frames[generation].insert(audit.published_frames[generation].end(), {{6U, 60U}, {7U, 70U}});
 audit.reconcile_rendered_probes(generation);
 CHECK(audit.rendered_probe_frames.at(key) == std::vector<std::uint64_t>{60U, 70U});
 CHECK(audit.rendered_probe_frames.size() == 1U);
 audit.patched_slots[generation][2U] = 12U;
 audit.reconcile_rendered_probes(generation);
 CHECK(audit.causal_failure == "rendered probe patch identity");
 CHECK(audit.rendered_probe_frames.at(key) == std::vector<std::uint64_t>{60U, 70U});
}
TEST_CASE("rendered slot queries share fixed and wildcard geometry admission", "[workspace][audit]") {
 BrowserAudit audit;
 const std::map<std::uint64_t, std::uint64_t> slots{{2U, 11U}};
 for (std::uint64_t snapshot = 1U; snapshot <= 3U; ++snapshot) {
  audit.explore_slots[snapshot] = slots;
  audit.explore_slot_frames[snapshot] = 50U;
 }
 audit.surface_geometries.push_back({.presentation_revision = 70U, .source_revision = 50U, .width = 640.0, .height = 320.0});
 const auto key = std::pair{70U, 50U};
 audit.surface_draws.insert(70U);
 audit.surface_redraw_counts[70U] = 2U;
 for (const double scale : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), 1.0}) {
  audit.surface_scales[key] = scale;
  for (const std::uint64_t source : {0U, 50U}) {
   CHECK(audit.rendered_frame_for_slots(slots, source, 2U) == (scale > 0.0));
   CHECK_FALSE(audit.rendered_frame_for_slots(slots, source, 3U));
  }
 }
 audit.explore_slots[4U] = slots;
 audit.explore_slot_frames[4U] = 0U;
 audit.surface_geometries.push_back({.presentation_revision = 71U, .source_revision = 0U, .width = 640.0, .height = 320.0});
 audit.surface_scales[{71U, 0U}] = 1.0;
 audit.surface_draws.insert(71U);
 audit.surface_draws.erase(70U);
 CHECK(audit.rendered_frame_for_slots(slots, 0U, 0U));
 CHECK_FALSE(audit.rendered_frame_for_slots(slots, 50U, 0U));
 audit.surface_draws.erase(71U);
 audit.surface_draws.insert(70U);
 CHECK_FALSE(audit.rendered_frame_for_slots(slots, 51U, 0U));
 CHECK_FALSE(audit.rendered_frame_for_slots({{2U, 12U}}, 50U, 0U));
 audit.surface_geometries.front().width = std::numeric_limits<double>::quiet_NaN();
 CHECK_FALSE(audit.rendered_frame_for_slots(slots, 50U, 0U));
 audit.surface_geometries.front().width = 640.0;
 audit.explore_gallery = {.x = 0.0, .y = 0.0, .width = 640.0, .height = 320.0};
 CHECK_FALSE(audit.rendered_frame_for_slots(slots, 50U, 0U));
 audit.square_atlas_frames.insert(key);
 audit.atlas_scaled_frames.insert(key);
 for (const double width : {639.0, 639.5, 640.0, 641.0}) {
  audit.surface_geometries.front().width = width;
  for (const std::uint64_t source : {0U, 50U}) CHECK(audit.rendered_frame_for_slots(slots, source, 0U) == (width > 639.0 && width < 641.0));
 }
}
TEST_CASE("browser compile metrics accept zero drops and require consistent progress", "[workspace][audit]") {
 const auto dropped = GENERATE(0U, 3U);
 const auto completed = GENERATE(0U, 4U);
 const nlohmann::json metrics{{"event", "integration.compile_metrics"}, {"control", "train.compile_dataset.progress"}, {"detail", "elapsed-eta-throughput-dropped"}, {"a", 2U},
  {"b", completed == 0U ? 0U : 8U}, {"c", completed == 0U ? 0U : 2U}, {"d", dropped}};
 BrowserAudit audit;
 audit.consume(metrics);
 CHECK_FALSE(audit.compile_metrics);
 audit.consume({{"event", "integration.compile_progress"}, {"control", "train.compile_dataset.progress"}, {"detail", completed == 0U ? "planning" : "compiling"}, {"a", 1U}, {"b", completed},
  {"c", 20U}, {"d", dropped}});
 REQUIRE(audit.progress);
 audit.consume(metrics);
 CHECK(audit.compile_metrics);
 for (const auto* field : {"b", "c", "d"}) {
  auto inconsistent = metrics;
  inconsistent[field] = metrics.at(field).get<unsigned>() + 1U;
  audit.consume(inconsistent);
  CHECK_FALSE(audit.compile_metrics);
 }
}
TEST_CASE("browser audit exposes distinct integration phases for progress deadlines", "[workspace][audit]") {
 BrowserAudit audit;
 audit.consume({{"event", "integration.phase_progress"}, {"control", "startup"}, {"detail", "AwaitBootstrap"}});
 CHECK(audit.phase_progress_revision == 1U);
 CHECK(audit.phase_progress_class == "startup");
 CHECK(audit.phase_progress_name == "AwaitBootstrap");
 audit.consume({{"event", "integration.phase_progress"}, {"control", "startup"}, {"detail", "AwaitBootstrap"}});
 CHECK(audit.phase_progress_revision == 1U);
 audit.consume({{"event", "integration.phase_progress"}, {"control", "work"}, {"detail", "AwaitCompileCompletion"}});
 CHECK(audit.phase_progress_revision == 2U);
 CHECK(audit.phase_progress_class == "work");
 CHECK(audit.phase_progress_name == "AwaitCompileCompletion");
 audit.consume({{"event", "integration.explore_reopen_wait"}, {"a", 41U}});
 CHECK(audit.work_progress_revision == 1U);
 audit.consume({{"event", "integration.explore_reopen_wait"}, {"a", 41U}});
 CHECK(audit.work_progress_revision == 1U);
 audit.consume({{"event", "integration.explore_reopen_wait"}, {"a", 42U}});
 CHECK(audit.work_progress_revision == 2U);
 audit.consume({{"event", "integration.explore_reopen_draw"}, {"d", 9U}});
 CHECK(audit.work_progress_revision == 3U);
 audit.consume({{"event", "integration.explore_reopen_draw"}, {"d", 9U}});
 CHECK(audit.work_progress_revision == 3U);
 audit.consume({{"event", "integration.phase_progress"}, {"control", "unbounded"}, {"detail", "Unknown"}});
 CHECK_FALSE(audit.bounds_valid);
}
TEST_CASE("workflow deadlines advance only for new native progress in the waiting step", "[workspace][audit]") {
 BrowserAudit audit;
 audit.consume({{"event", "integration.phase_progress"}, {"control", "work"}, {"detail", "Workflows(Trained)"}});
 nlohmann::json progress{{"event", "integration.workflow.operation_progress"}, {"control", "train.primary"}, {"detail", "Training"}, {"a", 1U}, {"b", 2U}};
 audit.consume(progress);
 CHECK(audit.work_progress_revision == 0U);
 progress["detail"] = "Trained";
 audit.consume(progress);
 CHECK(audit.work_progress_revision == 1U);
 audit.consume(progress);
 CHECK(audit.work_progress_revision == 1U);
 progress["b"] = 1U;
 audit.consume(progress);
 CHECK(audit.work_progress_revision == 1U);
 progress["b"] = 3U;
 audit.consume(progress);
 CHECK(audit.work_progress_revision == 2U);
 progress["control"] = "unrelated";
 progress["b"] = 4U;
 audit.consume(progress);
 CHECK(audit.work_progress_revision == 2U);
}
// Configured inventory: primary + DPI + two destructive terminals + four
// startup-latched faults + two quiet paths + model workflows = 11 H2D lifetimes. Optional GDR
// adds one focused lifetime. The former matrix used 23 per transport (46
// with GDR), recompiling/relaunching ordinary coverage for each case.
TEST_CASE("rendered_probe_audit_rejects_mismatched_identity", "[workspace_wayland_integration][rendered_probe_audit]") { rendered_probe_audit_rejects_mismatched_identity(); }
[[nodiscard]] nlohmann::json native_surface_record(const char* event, const std::uint64_t low = 12U) {
 const std::string_view name{event};
 const bool copying = name.starts_with("presentation.source_borrow.") || name == "presentation.source.read_submitted";
 const bool source_control = name.starts_with("presentation.source.") && !copying;
 return {{"kind", "gui_runtime"}, {"event", event}, {"sequence", 7U}, {"surface_high", 11U}, {"surface_low", source_control ? low + 1000U : low}, {"selection_generation", 19U},
  {"frame_revision", 23U}, {"capacity_width", 64U}, {"capacity_height", 32U}, {"condition", 2U}, {"outcome", name == "presentation.source.read_submitted" ? 0U : 1U}, {"value", copying ? 0U : 1U},
  {"source_revision", 23U}, {"source_session", 1U}, {"source_width", 64U}, {"source_height", 32U}, {"workspace_source_high", 11U}, {"workspace_source_low", low + 1000U},
  {"workspace_allocation", low + 2000U}, {"workspace_bytes", 8192U}, {"workspace_pitch", 256U}, {"workspace_width", 64U}, {"workspace_height", 32U}, {"direct_sampling", false},
  {"allocation_generation", 7U}, {"presentation_revision", copying ? 0U : 1U}, {"transfer_sequence", copying ? 0U : 1U}, {"metadata_bytes", 128U}, {"metadata_fingerprint", "819de48387b34f29"},
  {"timeline_ready", copying ? 0U : 1U}, {"trace_id", 31U}, {"span_id", 31U},
  {"span_outcome", static_cast<std::uint64_t>(std::string_view{event}.ends_with(".completed") ? mmltk::controller::contracts::DiagnosticSpanOutcome::Success
                                                                                              : mmltk::controller::contracts::DiagnosticSpanOutcome::Unspecified)}};
}
[[nodiscard]] nlohmann::json browser_surface_record(const char* event, const std::uint64_t low = 12U) {
 const std::string_view name{event};
 static std::uint64_t next_draw = 0U;
 static std::unordered_map<std::uint64_t, std::uint64_t> current_draw;
 if (name == "iced.surface.sample_draw_selected") current_draw[low] = ++next_draw;
 return {{"event", event}, {"draw_identity", current_draw[low]},
  {"surface", SurfaceAudit::native_identity(native_surface_record("", name.starts_with("firefox.workspace.source.") ? low + 1000U : low))},
  {"source", SurfaceAudit::native_identity(native_surface_record("", low + 1000U))}, {"arena", SurfaceAudit::native_identity(native_surface_record("", low))}, {"workspace_allocation", low + 2000U},
  {"direct_sampling", false}, {"layer", 0U}, {"slot", 0U}, {"content_session", 1U}, {"content_sequence", 23U}, {"content_width", 64U}, {"content_height", 32U}, {"transfer_sequence", 1U},
  {"metadata_bytes", 128U}, {"metadata_fingerprint", "819de48387b34f29"}, {"timeline_ready", 1U}, {"timeline_release", 2U},
  {"requested_surface", SurfaceAudit::native_identity(native_surface_record("", low))}, {"width", 64U}, {"height", 32U}, {"frame_revision", 23U}, {"presentation_revision", 1U},
  {"outcome", "claimed"}};
}
[[nodiscard]] nlohmann::json release_only_record(const char* event) {
 auto record = browser_surface_record(event);
 record.erase("layer");
 record.erase("slot");
 return record;
}
constexpr std::array native_surface_events{"presentation.arena.advertised", "presentation.admission.enqueued", "presentation.admission.written", "presentation.import.outcome",
 "presentation.source.admission.enqueued", "presentation.source.admission.written", "presentation.source.ready", "presentation.source_borrow.started", "presentation.source_borrow.completed",
 "presentation.source.read_submitted", "presentation.ready_sync.started", "presentation.ready_sync.completed", "presentation.frame.edge", "presentation.release_wait.started",
 "presentation.release_wait.completed", "presentation.active.withdrawal", "presentation.retirement"};
constexpr std::array browser_surface_events{"firefox.workspace.admitted", "iced.surface.texture_create", "firefox.workspace.claim_outcome", "firefox.workspace.registry_inserted",
 "firefox.workspace.import_ready_emitted", "firefox.workspace.ready", "firefox.workspace.source.admitted", "firefox.workspace.source.claim_outcome", "firefox.workspace.source.ready",
 "firefox.workspace.frame_forwarded", "firefox.workspace.frame_dispatched", "firefox.workspace.copy_completed", "iced.surface.sample_acquired", "iced.surface.sample_draw_selected",
 "iced.surface.draw_encoded", "iced.surface.draw_submitted", "iced.frame.draw_settled", "firefox.workspace.withdrawal", "iced.frame.sample_released", "iced.surface.texture_destroyed",
 "firefox.workspace.retired"};
constexpr std::size_t browser_live_stages = 17U;
void record_source_transfer(SurfaceAudit& audit, const bool acquired = true) {
 for (std::size_t stage = 0U; stage < (acquired ? 15U : 13U); ++stage) audit.native(native_surface_record(native_surface_events[stage]));
 for (std::size_t stage = 0U; stage < 9U; ++stage) audit.browser(browser_surface_record(browser_surface_events[stage]));
}
void record_unsettled_source_read(SurfaceAudit& audit, const bool direct, const bool dispatched) {
 for (std::size_t stage = 0U; stage < 13U; ++stage) {
  auto record = native_surface_record(native_surface_events[stage]);
  record["direct_sampling"] = direct;
  audit.native(record);
 }
 for (std::size_t stage = 0U; stage < browser_live_stages; ++stage) {
  const std::string_view event{browser_surface_events[stage]};
  if (event == "firefox.workspace.copy_completed" || (!dispatched && event == "firefox.workspace.frame_dispatched")) continue;
  auto record = browser_surface_record(browser_surface_events[stage]);
  record["direct_sampling"] = direct;
  audit.browser(record);
 }
}
void record_receiver_withdrawal(SurfaceAudit& audit) {
 for (std::size_t index = 0U; index < 6U; ++index) audit.browser(browser_surface_record(browser_surface_events[index]));
 audit.browser(browser_surface_record("iced.surface.pending_discarded"));
 audit.browser(browser_surface_record("firefox.workspace.withdrawal"));
 audit.browser(browser_surface_record("iced.surface.import_dropped"));
 audit.browser(browser_surface_record("iced.surface.texture_destroyed"));
 audit.browser(browser_surface_record("firefox.workspace.retired"));
}
TEST_CASE("unsampled arena retirement joins native and Firefox physical ownership", "[workspace][audit]") {
 const std::string fault =
  GENERATE("complete", "native-admission", "browser-admission", "dimensions", "native-withdrawal", "browser-withdrawal", "native-retirement", "browser-retirement", "failed-retirement");
 CAPTURE(fault);
 SurfaceAudit audit;
 for (std::size_t stage = 0U; stage < 4U; ++stage)
  if (fault != "native-admission" || stage != 2U) audit.native(native_surface_record(native_surface_events[stage]));
 for (std::size_t stage = 0U; stage < 6U; ++stage) {
  if (stage == 1U || (fault == "browser-admission" && stage == 4U)) continue;
  auto record = browser_surface_record(browser_surface_events[stage]);
  if (fault == "dimensions") record["width"] = 65U;
  audit.browser(record);
 }
 if (fault != "native-withdrawal") audit.native(native_surface_record("presentation.candidate.withdrawal"));
 if (fault != "browser-withdrawal") audit.browser(browser_surface_record("firefox.workspace.drop_received"));
 if (fault != "native-retirement") {
  auto retirement = native_surface_record("presentation.retirement");
  if (fault == "failed-retirement") retirement["outcome"] = 0U;
  audit.native(retirement);
 }
 if (fault != "browser-retirement") audit.browser(browser_surface_record("firefox.workspace.retired"));
 if (fault == "complete") {
  CHECK(audit.joined_failure().empty());
  CHECK(audit.evidence_settled());
  audit.SettleScenario();
  CHECK(audit.surfaces.empty());
  SurfaceAudit unclaimed;
  for (std::size_t stage = 0U; stage < 3U; ++stage) unclaimed.native(native_surface_record(native_surface_events[stage]));
  unclaimed.browser(browser_surface_record("firefox.workspace.admitted"));
  CHECK_FALSE(unclaimed.joined_failure().empty());
  unclaimed.native({{"event", "shutdown.requested"}});
  CHECK_FALSE(unclaimed.joined_failure().empty());
  unclaimed.native(native_surface_record("presentation.retirement"));
  CHECK_FALSE(unclaimed.joined_failure().empty());
  unclaimed.browser({{"event", "firefox.workspace.channel_terminal"}, {"terminal", "orderly_bridge_close"}});
  CHECK_FALSE(unclaimed.joined_failure().empty());
  unclaimed.native({{"event", "shutdown.firefox_terminal"}});
  REQUIRE(unclaimed.joined_failure().empty());
  const auto id = SurfaceAudit::native_identity(native_surface_record(""));
  auto missing_retirement = unclaimed;
  missing_retirement.surfaces.at(id).native_retired = false;
  CHECK_FALSE(missing_retirement.joined_failure().empty());
  auto mismatched = unclaimed;
  mismatched.surfaces.at(id).browser_width += 1U;
  CHECK_FALSE(mismatched.joined_failure().empty());
  auto claimed = unclaimed;
  claimed.browser(browser_surface_record("firefox.workspace.claim_outcome"));
  CHECK_FALSE(claimed.joined_failure().empty());
  auto textured = unclaimed;
  textured.browser(browser_surface_record("iced.surface.texture_create"));
  CHECK_FALSE(textured.joined_failure().empty());
 } else {
  CHECK_FALSE(audit.joined_failure().empty());
 }
}
TEST_CASE("Image metadata stays paired with its acquired pixels through retained draws", "[workspace][audit]") {
 SurfaceAudit audit;
 record_source_transfer(audit);
 for (std::size_t stage = 9U; stage < browser_live_stages; ++stage) audit.browser(browser_surface_record(browser_surface_events[stage]));
 REQUIRE(audit.evidence_settled());
 audit.SettleScenario();
 for (const auto* event : {"iced.surface.sample_draw_selected", "iced.surface.draw_encoded", "iced.surface.draw_submitted", "iced.frame.draw_settled"}) audit.browser(browser_surface_record(event));
 REQUIRE(audit.evidence_settled());
 const auto& surface = audit.surfaces.at(SurfaceAudit::native_identity(native_surface_record("")));
 REQUIRE(surface.custody.at(1U).metadata.has_value());
 CHECK(surface.custody.at(1U).metadata == surface.transfers.at({1U, 1U}).metadata);
 CHECK(surface.custody.at(1U).settled == 2U);
}
TEST_CASE("exact draw submission survives non-FIFO encoder settlement", "[workspace][audit]") {
 SurfaceAudit audit;
 for (const auto* event : native_surface_events) audit.native(native_surface_record(event));
 for (std::size_t stage = 0U; stage < browser_live_stages - 1U; ++stage) audit.browser(browser_surface_record(browser_surface_events[stage]));
 const auto submitted_identity = audit.draws.front().identity;
 audit.SettleScenario();
 REQUIRE(audit.draws.size() == 1U);
 // The older encoder entered the queue. A later encoder of the same
 // publication is abandoned before the older work-done callback arrives.
 for (const auto* event : {"iced.surface.sample_draw_selected", "iced.surface.draw_encoded", "iced.frame.draw_abandoned"}) audit.browser(browser_surface_record(event));
 auto settled = browser_surface_record("iced.frame.draw_settled");
 settled["draw_identity"] = submitted_identity;
 audit.browser(settled);
 for (std::size_t stage = browser_live_stages; stage < browser_surface_events.size(); ++stage) audit.browser(browser_surface_record(browser_surface_events[stage]));
 REQUIRE(audit.joined_failure().empty());
 REQUIRE(audit.draws.size() == 2U);
 CHECK(audit.draws[0].submitted > audit.draws[0].encoded);
 CHECK(audit.draws[0].settled > audit.draws[0].submitted);
 CHECK_FALSE(audit.draws[0].abandoned);
 CHECK(audit.draws[1].submitted == 0U);
 CHECK(audit.draws[1].abandoned);
}
TEST_CASE("draw submission requires exact encoded identity and live custody", "[workspace][audit]") {
 const std::string fault = GENERATE("complete", "width", "height", "requested", "publication", "before-encoding", "after-settlement", "duplicate", "draw-identity");
 CAPTURE(fault);
 SurfaceAudit audit;
 record_source_transfer(audit);
 for (std::size_t stage = 9U; stage < browser_live_stages - 2U; ++stage)
  if (fault != "before-encoding" || std::string_view{browser_surface_events[stage]} != "iced.surface.draw_encoded") audit.browser(browser_surface_record(browser_surface_events[stage]));
 if (fault == "after-settlement") audit.browser(browser_surface_record("iced.frame.draw_abandoned"));
 auto submitted = browser_surface_record("iced.surface.draw_submitted");
 if (fault == "width" || fault == "height") submitted.erase(fault);
 if (fault == "requested") submitted["requested_surface"] = "";
 if (fault == "publication") submitted["presentation_revision"] = 2U;
 if (fault == "draw-identity") submitted.erase("draw_identity");
 audit.browser(submitted);
 if (fault == "duplicate") audit.browser(submitted);
 CHECK(audit.failure.empty() == (fault == "complete"));
}
TEST_CASE("Image metadata acceptance validates payload and physical transfer identity", "[workspace][audit]") {
 const std::string fault = GENERATE("bytes", "fingerprint", "empty", "malformed", "source", "transfer");
 CAPTURE(fault);
 SurfaceAudit audit;
 record_source_transfer(audit);
 for (std::size_t stage = 9U; stage < browser_live_stages; ++stage) {
  auto record = browser_surface_record(browser_surface_events[stage]);
  if (std::string_view{browser_surface_events[stage]} == "iced.surface.sample_acquired") {
   if (fault == "bytes") record["metadata_bytes"] = 129U;
   if (fault == "fingerprint") record["metadata_fingerprint"] = "819de48387b34f28";
   if (fault == "empty") record["metadata_bytes"] = 0U;
   if (fault == "malformed") record["metadata_fingerprint"] = "not-hex";
   if (fault == "source") record["source"] = SurfaceAudit::native_identity(native_surface_record("", 13U));
   if (fault == "transfer") record["transfer_sequence"] = 2U;
  }
  audit.browser(record);
 }
 CHECK_FALSE(audit.evidence_settled());
}
TEST_CASE("scenario settlement retains physical reuse and waits for independent native delivery", "[workspace][audit]") {
 SurfaceAudit audit;
 for (std::size_t stage = 0U; stage < 12U; ++stage) audit.native(native_surface_record(native_surface_events[stage]));
 for (std::size_t stage = 0U; stage < browser_live_stages; ++stage) audit.browser(browser_surface_record(browser_surface_events[stage]));
 REQUIRE(audit.failure.empty());
 CHECK_FALSE(audit.evidence_settled());
 for (std::size_t stage = 12U; stage < 15U; ++stage) audit.native(native_surface_record(native_surface_events[stage]));
 REQUIRE(audit.evidence_settled());
 const auto id = SurfaceAudit::native_identity(native_surface_record(""));
 const auto publications = audit.surfaces.at(id).publications;
 audit.SettleScenario();
 REQUIRE(audit.surfaces.contains(id));
 CHECK(audit.surfaces.at(id).publications == publications);
 CHECK(audit.draws.empty());
 audit.browser(browser_surface_record("iced.surface.sample_draw_selected"));
 audit.browser(browser_surface_record("iced.surface.draw_encoded"));
 audit.browser(browser_surface_record("iced.surface.draw_submitted"));
 audit.browser(browser_surface_record("iced.frame.draw_settled"));
 CHECK(audit.evidence_settled());
 CHECK(audit.draws.size() == 1U);
 for (std::size_t stage = 15U; stage < native_surface_events.size(); ++stage) audit.native(native_surface_record(native_surface_events[stage]));
 CHECK_FALSE(audit.evidence_settled());
 for (std::size_t stage = browser_live_stages; stage < browser_surface_events.size(); ++stage) audit.browser(browser_surface_record(browser_surface_events[stage]));
 REQUIRE(audit.evidence_settled());
 audit.SettleScenario();
 CHECK(audit.surfaces.empty());
 CHECK(audit.failure.empty());
}
TEST_CASE("terminal reads require exact positive completion or installed custody evidence", "[workspace][audit]") {
 const bool retained = GENERATE(false, true);
 const bool direct = GENERATE(false, true);
 const std::string fault = GENERATE("none", "missing", "duplicate", "before_terminal", "allocation", "outcome", "dispatch");
 const std::string contradiction = retained ? GENERATE("none", "release_before", "release_after", "retirement_before", "retirement_after") : "none";
 CAPTURE(retained, direct, fault, contradiction);
 SurfaceAudit audit;
 const auto native = [&](const char* event) {
  auto record = native_surface_record(event);
  record["direct_sampling"] = direct;
  audit.native(record);
 };
 record_unsettled_source_read(audit, direct, fault != "dispatch");
 if (fault != "before_terminal") {
  native("shutdown.requested");
  native("shutdown.firefox_terminal");
 }
 const auto contradict = [&](const std::string_view order) {
  if (contradiction == std::string{"release_"} + std::string{order}) {
   native("presentation.release_wait.started");
   native("presentation.release_wait.completed");
  } else if (contradiction == std::string{"retirement_"} + std::string{order}) {
   native("presentation.source.withdrawal");
   native("presentation.source.retirement");
  }
 };
 contradict("before");
 if (fault != "missing") {
  auto record = native_surface_record(retained ? "presentation.terminal_read.retained" : "presentation.terminal_read.completed");
  record["direct_sampling"] = direct;
  if (fault == "allocation") record["workspace_allocation"] = 999999U;
  if (fault == "outcome") record["outcome"] = 0U;
  audit.native(record);
  if (fault == "duplicate") audit.native(record);
 }
 contradict("after");
 if (!retained) native("presentation.retirement");
 INFO(audit.joined_failure());
 CHECK(audit.joined_failure().empty() == (fault == "none" && contradiction == "none"));
}
TEST_CASE("terminal source retirement proves native completion whose receiver notification was lost", "[workspace][audit]") {
 const bool direct = GENERATE(false, true);
 const std::string fault = GENERATE("none", "release", "retirement", "shutdown", "browser_exit", "allocation", "mode", "dispatch", "transfer");
 CAPTURE(direct, fault);
 SurfaceAudit audit;
 const auto native = [&](const char* event) {
  auto record = native_surface_record(event);
  record["direct_sampling"] = direct;
  if (std::string_view{event} == "presentation.source.retirement") {
   if (fault == "allocation") record["workspace_allocation"] = 999999U;
   if (fault == "mode") record["direct_sampling"] = !direct;
  }
  if (fault == "transfer" && std::string_view{event} == "presentation.release_wait.completed") record["transfer_sequence"] = 999999U;
  audit.native(record);
 };
 record_unsettled_source_read(audit, direct, fault != "dispatch");
 native("presentation.release_wait.started");
 if (fault != "release") native("presentation.release_wait.completed");
 if (fault != "shutdown") native("shutdown.requested");
 if (fault != "browser_exit") native("shutdown.firefox_terminal");
 native("presentation.source.withdrawal");
 if (fault != "retirement") native("presentation.source.retirement");
 native("presentation.retirement");
 INFO(audit.joined_failure());
 CHECK(audit.joined_failure().empty() == (fault == "none"));
}
TEST_CASE("source release settlement outlives the sample arena but not its physical source", "[workspace][audit]") {
 const bool release_started = GENERATE(false, true);
 const bool source_retired = GENERATE(false, true);
 SurfaceAudit audit;
 for (std::size_t stage = 0U; stage < 13U; ++stage) audit.native(native_surface_record(native_surface_events[stage]));
 for (const char* event : browser_surface_events) audit.browser(browser_surface_record(event));
 if (release_started) audit.native(native_surface_record("presentation.release_wait.started"));
 audit.native(native_surface_record("presentation.active.withdrawal"));
 audit.native(native_surface_record("presentation.retirement"));
 REQUIRE(audit.failure.empty());
 CHECK_FALSE(audit.evidence_settled());
 if (source_retired) {
  audit.native(native_surface_record("presentation.source.withdrawal"));
  audit.native(native_surface_record("presentation.source.retirement"));
 }
 if (!release_started) audit.native(native_surface_record("presentation.release_wait.started"));
 audit.native(native_surface_record("presentation.release_wait.completed"));
 CHECK(audit.failure.empty() == !source_retired);
 CHECK(audit.evidence_settled() == !source_retired);
}
TEST_CASE("receiver process exit terminates page readers without fabricating draw completion", "[workspace][audit]") {
 const std::string boundary = GENERATE("shutdown", "bridge", "exit", "premature-texture-destruction");
 SurfaceAudit audit;
 for (const char* event : native_surface_events) audit.native(native_surface_record(event));
 for (std::size_t stage = 0U; stage < browser_live_stages - 1U; ++stage) audit.browser(browser_surface_record(browser_surface_events[stage]));
 REQUIRE(audit.failure.empty());
 REQUIRE_FALSE(audit.evidence_settled());
 audit.native({{"event", "shutdown.requested"}});
 if (boundary == "bridge") audit.browser({{"event", "firefox.workspace.channel_terminal"}, {"terminal", "orderly_bridge_close"}});
 if (boundary == "premature-texture-destruction") audit.browser(browser_surface_record("iced.surface.texture_destroyed"));
 if (boundary == "exit" || boundary == "premature-texture-destruction") audit.native({{"event", "shutdown.firefox_terminal"}});
 CHECK(audit.joined_failure().empty() == (boundary == "exit"));
 const auto& custody = audit.surfaces.begin()->second.custody.begin()->second;
 CHECK(custody.settled == 0U);
 CHECK(custody.abandoned == 0U);
}
TEST_CASE("physical lifecycle custody rejects capacity overflow instead of dropping provenance", "[workspace][audit]") {
 SurfaceAudit audit;
 for (std::size_t identity = 1U; identity <= kAcceptanceGenerationLimit; ++identity) audit.native(native_surface_record("presentation.arena.advertised", identity));
 REQUIRE(audit.failure.empty());
 audit.native(native_surface_record("presentation.arena.advertised", kAcceptanceGenerationLimit + 1U));
 CHECK_FALSE(audit.failure.empty());
 CHECK(audit.surfaces.size() == kAcceptanceGenerationLimit);
}
TEST_CASE("surface join rejects missing native provenance", "[workspace][audit]") {
 for (const char* field : {"sequence", "selection_generation", "frame_revision", "capacity_width", "capacity_height", "condition", "outcome"}) {
  SurfaceAudit audit;
  auto record = native_surface_record("presentation.arena.advertised");
  record.erase(field);
  audit.native(record);
  CHECK_FALSE(audit.joined_failure().empty());
 }
 SurfaceAudit audit;
 audit.browser(nlohmann::json{{"event", "iced.surface.texture_create"}, {"surface", "not-a-capability"}});
 CHECK_FALSE(audit.joined_failure().empty());
}
TEST_CASE("surface join records failed span endings without accepting a completed physical transition", "[workspace][audit]") {
 using mmltk::controller::contracts::DiagnosticSpanOutcome;
 for (const auto outcome : {DiagnosticSpanOutcome::Unspecified, DiagnosticSpanOutcome::ScopeExit, DiagnosticSpanOutcome::Cancelled, DiagnosticSpanOutcome::Exception}) {
  SurfaceAudit audit;
  for (std::size_t index = 0U; index < 8U; ++index) audit.native(native_surface_record(native_surface_events[index]));
  auto ended = native_surface_record("presentation.source_borrow.completed");
  ended["span_outcome"] = static_cast<std::uint64_t>(outcome);
  audit.native(ended);
  const auto& surface = audit.surfaces.at(SurfaceAudit::native_identity(ended));
  CHECK(surface.source_steps.at(31U) == 1U);
  CHECK(surface.ended_spans.at(31U) == static_cast<std::uint64_t>(outcome));
  audit.native(native_surface_record("presentation.source.read_submitted"));
  CHECK_FALSE(audit.joined_failure().empty());
 }
}
TEST_CASE("surface join rejects omitted admission and reordered observed copy stages", "[workspace][audit]") {
 for (std::size_t omitted = 0U; omitted < native_surface_events.size(); ++omitted) {
  if (std::string_view{native_surface_events[omitted]} == "presentation.active.withdrawal") continue;  // Receiver-initiated withdrawal is independently authoritative.
  SurfaceAudit audit;
  for (std::size_t index = 0U; index < native_surface_events.size(); ++index)
   if (index != omitted) audit.native(native_surface_record(native_surface_events[index]));
  for (const char* event : browser_surface_events) audit.browser(browser_surface_record(event));
  CHECK_FALSE(audit.joined_failure().empty());
 }
 for (std::size_t omitted = 0U; omitted < browser_surface_events.size(); ++omitted) {
  SurfaceAudit audit;
  for (const char* event : native_surface_events) audit.native(native_surface_record(event));
  for (std::size_t index = 0U; index < browser_surface_events.size(); ++index)
   if (index != omitted) audit.browser(browser_surface_record(browser_surface_events[index]));
  if (std::string_view{browser_surface_events[omitted]} == "iced.surface.draw_submitted") {
   // Physical closure remains independent of proof that this exact
   // selected/requested draw entered a submitted encoder.
   CHECK(audit.joined_failure().empty());
   REQUIRE(audit.draws.size() == 1U);
   CHECK(audit.draws.front().submitted == 0U);
  } else {
   CHECK_FALSE(audit.joined_failure().empty());
  }
 }
 SurfaceAudit reordered;
 reordered.native(native_surface_record("presentation.admission.written"));
 reordered.native(native_surface_record("presentation.arena.advertised"));
 CHECK_FALSE(reordered.joined_failure().empty());
 SurfaceAudit reordered_copy;
 for (std::size_t index = 0U; index < 4U; ++index) reordered_copy.native(native_surface_record(native_surface_events[index]));
 reordered_copy.native(native_surface_record("presentation.source.read_submitted"));
 reordered_copy.native(native_surface_record("presentation.source_borrow.started"));
 CHECK_FALSE(reordered_copy.joined_failure().empty());
 SurfaceAudit dropped_ready_start;
 for (std::size_t index = 0U; index < native_surface_events.size(); ++index)
  if (index != 10U) dropped_ready_start.native(native_surface_record(native_surface_events[index]));
 for (const char* event : browser_surface_events) dropped_ready_start.browser(browser_surface_record(event));
 CHECK_FALSE(dropped_ready_start.joined_failure().empty());
 SurfaceAudit reordered_ready;
 for (std::size_t index = 0U; index < 10U; ++index) reordered_ready.native(native_surface_record(native_surface_events[index]));
 reordered_ready.native(native_surface_record("presentation.ready_sync.completed"));
 reordered_ready.native(native_surface_record("presentation.ready_sync.started"));
 CHECK_FALSE(reordered_ready.joined_failure().empty());
}
TEST_CASE("surface join accepts receiver-confirmed withdrawal and explicit rejected-import retirement", "[workspace][audit]") {
 SurfaceAudit audit;
 for (std::size_t index = 0U; index < 4U; ++index) audit.native(native_surface_record(native_surface_events[index]));
 audit.native(native_surface_record("presentation.retirement"));
 record_receiver_withdrawal(audit);
 CHECK(audit.joined_failure().empty());
 SurfaceAudit dropped_candidate_retirement;
 for (std::size_t index = 0U; index < 4U; ++index) dropped_candidate_retirement.native(native_surface_record(native_surface_events[index]));
 dropped_candidate_retirement.native(native_surface_record("presentation.candidate.withdrawal"));
 auto replacement = native_surface_record("presentation.replacement", 13U);
 replacement["sequence"] = 8U;
 replacement["allocation_generation"] = 8U;
 dropped_candidate_retirement.native(replacement);
 record_receiver_withdrawal(dropped_candidate_retirement);
 CHECK_FALSE(dropped_candidate_retirement.joined_failure().empty());
 for (const bool released : {false, true}) {
  CAPTURE(released);
  SurfaceAudit used_pending;
  for (const char* event : native_surface_events) used_pending.native(native_surface_record(event));
  for (const char* event : browser_surface_events) {
   used_pending.browser(browser_surface_record(event));
   if (std::string_view{event} == "iced.surface.sample_acquired") break;
  }
  used_pending.browser(browser_surface_record("firefox.workspace.withdrawal"));
  used_pending.browser(browser_surface_record("iced.surface.pending_discarded"));
  if (released) used_pending.browser(browser_surface_record("iced.frame.sample_released"));
  used_pending.browser(browser_surface_record("iced.surface.texture_destroyed"));
  used_pending.browser(browser_surface_record("firefox.workspace.retired"));
  CHECK(used_pending.joined_failure().empty() == released);
 }
 for (const bool observed_native_outcome : {false, true}) {
  SurfaceAudit rejected;
  for (std::size_t index = 0U; index < 3U; ++index) rejected.native(native_surface_record(native_surface_events[index]));
  if (observed_native_outcome) {
   auto outcome = native_surface_record("presentation.import.outcome");
   outcome["value"] = 0U;
   outcome["outcome"] = 4U;
   rejected.native(outcome);
  }
  rejected.native(native_surface_record("presentation.retirement"));
  rejected.browser(browser_surface_record("firefox.workspace.admitted"));
  rejected.browser(browser_surface_record("iced.surface.texture_create"));
  rejected.browser(browser_surface_record("firefox.workspace.claim_outcome"));
  rejected.browser(browser_surface_record("firefox.workspace.import_failed"));
  rejected.browser(browser_surface_record("iced.surface.pending_discarded"));
  rejected.browser(browser_surface_record("iced.surface.texture_destroyed"));
  CHECK(rejected.joined_failure().empty() == observed_native_outcome);
 }
}
TEST_CASE("incremental Explore audit requires independent lifecycle and exact inventory evidence", "[workspace][audit]") {
 NativeAudit annotation;
 annotation.consume({{"kind", "gui_runtime"}, {"owner", "annotation"}, {"event", "document.opened"}});
 CHECK_FALSE(annotation.annotation_copied);
 CHECK(annotation.annotation_opened);
 annotation.consume({{"kind", "gui_runtime"}, {"owner", "annotation"}, {"event", "copy.completed"}});
 CHECK(annotation.annotation_copied);
 NativeAudit closure;
 closure.consume({{"kind", "gui_runtime"}, {"event", "browser.server.peer_opened"}});
 closure.consume({{"kind", "gui_runtime"}, {"event", "shutdown.complete"}});
 CHECK(closure.peer_open_count == 1U);
 CHECK(closure.peer_close_count == 0U);
 closure.consume({{"kind", "gui_runtime"}, {"event", "browser.server.peer_closed"}});
 CHECK(closure.peer_close_count == closure.peer_open_count);
 NativeAudit audit;
 audit.consume_explore_evidence("acceptance.placeholder.slot", 0U, 10U, 4096U);
 audit.consume_explore_evidence("acceptance.placeholder.slot", 1U, 11U, 4096U);
 audit.consume_explore_evidence("acceptance.placeholder.complete", 2U, 19U, 4096U);
 audit.consume_explore_evidence("acceptance.slot.patched", 0U, 10U, 4096U);
 audit.consume_explore_evidence("tile.batch.published", 1U, 0U, 4096U);
 CHECK(audit.explore_placeholder);
 CHECK(audit.partial_generation == 2U);
 CHECK(audit.acceptance_first_patch_exact);
 NativeAudit joined_after_publication;
 joined_after_publication.consume_explore_evidence("acceptance.placeholder.complete", 2U, 19U, 4096U);
 joined_after_publication.consume_explore_evidence("acceptance.slot.patched", 0U, 10U, 4096U);
 joined_after_publication.consume_explore_evidence("tile.batch.published", 1U, 0U, 4096U);
 CHECK_FALSE(joined_after_publication.acceptance_first_patch_exact);
 joined_after_publication.join_gallery_publication(2U, {{0U, 10U}, {1U, 11U}});
 CHECK_FALSE(joined_after_publication.acceptance_first_patch_exact);
 joined_after_publication.consume_explore_evidence("acceptance.placeholder.slot", 0U, 10U, 4096U);
 joined_after_publication.consume_explore_evidence("acceptance.placeholder.slot", 1U, 11U, 4096U);
 joined_after_publication.join_gallery_publication(2U, {{0U, 10U}, {1U, 11U}});
 CHECK(joined_after_publication.partial_generation == 2U);
 CHECK(joined_after_publication.acceptance_first_patch_exact);
 const std::map<std::uint64_t, std::uint64_t> rendered_slots{{0U, 10U}, {1U, 11U}};
 for (const auto missing : {"none", "summary", "slot", "patch", "count", "frame", "identity", "conflict"}) {
  INFO("missing native inventory evidence: " << missing);
  NativeAudit material;
  const std::string_view omitted{missing};
  material.consume_explore_evidence("placeholder.published", 2U);
  material.consume_explore_evidence("acceptance.placeholder.slot", 0U, 10U);
  if (omitted != "slot") material.consume_explore_evidence("acceptance.placeholder.slot", 1U, omitted == "conflict" ? 12U : 11U);
  if (omitted != "summary") material.consume_explore_evidence("acceptance.placeholder.complete", 2U);
  material.consume_explore_evidence("acceptance.slot.patched", 0U, 10U);
  if (omitted != "patch") material.consume_explore_evidence("acceptance.slot.patched", 1U, omitted == "identity" ? 12U : 11U);
  material.consume_explore_evidence("tile.batch.published", omitted == "count" ? 1U : 2U);
  if (omitted != "frame") material.consume_explore_evidence("explore.frame.published", 7U);
  material.join_gallery_publication(3U, rendered_slots);
  material.join_gallery_publication(2U, rendered_slots);
  CHECK_FALSE(material.final_generations_for(rendered_slots, 3U, 7U).has_value());
  CHECK_FALSE(material.final_generations_for(rendered_slots, 2U, 8U).has_value());
  material.join_gallery_publication(2U, rendered_slots);
  const auto final = material.final_generations_for(rendered_slots, 2U, 7U);
  CHECK(final.has_value() == (omitted != "summary" && omitted != "slot" && omitted != "frame" && omitted != "conflict"));
  const bool complete_material =
   final && material.patched_slots.at(2U) == rendered_slots && material.published_tiles.at(2U) == rendered_slots.size() && !material.explore_stale_patch && !material.causal_inconsistent;
  CHECK(complete_material == (omitted == "none"));
  if (final) {
   CHECK(final->material == 2U);
   CHECK(final->cursor == 2U);
   CHECK(material.placeholder_slots.at(2U) == rendered_slots);
   CHECK(material.placeholder_cardinalities.at(2U) == rendered_slots.size());
  }
  if (omitted == "conflict") CHECK(material.causal_inconsistent);
 }
 NativeAudit initial;
 initial.consume_explore_evidence("placeholder.published", 0U);
 initial.consume_explore_evidence("acceptance.placeholder.slot", 0U, 10U);
 initial.consume_explore_evidence("acceptance.slot.patched", 0U, 10U);
 initial.consume_explore_evidence("explore.frame.published", 7U);
 initial.consume_explore_evidence("tile.batch.published", 1U);
 initial.join_gallery_publication(2U, rendered_slots);
 CHECK_FALSE(initial.acceptance_first_patch_exact);
 CHECK_FALSE(initial.placeholder_cardinalities.contains(2U));
 initial.consume_explore_evidence("acceptance.placeholder.slot", 1U, 11U);
 initial.join_gallery_publication(2U, rendered_slots);
 CHECK_FALSE(initial.acceptance_first_patch_exact);
 initial.join_gallery_publication(2U, rendered_slots);
 CHECK_FALSE(initial.acceptance_first_patch_exact);
 initial.consume_explore_evidence("acceptance.placeholder.complete", 2U);
 CHECK(initial.acceptance_first_patch_exact);
 CHECK(initial.partial_generation == 2U);
 CHECK(initial.partial_first_tile_count == 1U);
 CHECK_FALSE(initial.causal_inconsistent);
 CHECK_FALSE(initial.final_generations_for(rendered_slots, 2U, 8U).has_value());
 initial.consume({{"kind", "gui_runtime"}, {"owner", "presentation"}, {"event", "presentation.frame.edge"}, {"source_session", 1U}, {"source_instance", 1U}, {"source_observation_revision", 10U},
  {"source_revision", 8U}, {"frame_revision", 8U}});
 initial.join_gallery_publication(2U, rendered_slots);
 CHECK_FALSE(initial.final_generations_for(rendered_slots, 2U, 8U).has_value());
 initial.join_gallery_publication(2U, rendered_slots);
 CHECK_FALSE(initial.final_generations_for(rendered_slots, 2U, 8U).has_value());
 initial.consume_explore_evidence("explore.frame.published", 8U);
 initial.join_gallery_publication(2U, rendered_slots);
 CHECK(initial.final_generations_for(rendered_slots, 2U, 8U).has_value());
 initial.augmentation_seeds.emplace(2U, 0U);
 CHECK(initial.augmentation_generation_for(0U, 8U) == 2U);
 CHECK_FALSE(initial.augmentation_generation_for(1U, 8U).has_value());
 for (const std::string_view missing : {"none", "bitmap", "wrong-ready", "extra-ready", "patch", "identity", "frame", "observation", "physical", "late-patch"}) {
  INFO("missing partial publication evidence: " << missing);
  NativeAudit partial;
  partial.consume_explore_evidence("placeholder.published", 0U);
  for (std::uint64_t slot = 0U; slot < 3U; ++slot) partial.consume_explore_evidence("acceptance.placeholder.slot", slot, slot + 10U);
  partial.consume_explore_evidence("acceptance.placeholder.complete", 3U);
  if (missing != "patch" && missing != "late-patch") partial.consume_explore_evidence("acceptance.slot.patched", 1U, missing == "identity" ? 12U : 11U);
  partial.consume_explore_evidence("explore.frame.published", 8U);
  if (missing != "physical")
   partial.consume({{"kind", "gui_runtime"}, {"owner", "presentation"}, {"event", "presentation.frame.edge"}, {"source_session", 1U}, {"source_instance", 1U}, {"source_observation_revision", 11U},
    {"source_revision", 8U}, {"frame_revision", 8U}});
  if (missing == "late-patch") partial.consume_explore_evidence("acceptance.slot.patched", 1U, 11U);
  BrowserAudit browser;
  nlohmann::json snapshot{
   {"gallery_generation", 2U}, {"visible_indices", {10U, 11U, 12U}}, {"source_revision", missing == "frame" ? 9U : 8U}, {"source_observation_revision", missing == "observation" ? 12U : 11U}};
  if (missing != "bitmap") snapshot["ready_slots"] = std::vector<bool>{missing == "wrong-ready", missing != "wrong-ready", missing == "extra-ready"};
  browser.consume_gallery_generation(snapshot);
  REQUIRE(browser.bounds_valid);
  const auto& evidence = browser.gallery_generations.at(2U);
  partial.join_gallery_publication(2U, evidence.slots);
  // Even a complete receiver bitmap and Presentation receipt cannot
  // substitute for the native batch publication.
  CHECK_FALSE(partial.acceptance_first_patch_exact);
  CHECK_FALSE(partial.first_publication_ordinals.contains(2U));
  if (missing == "none") {
   partial.consume_explore_evidence("tile.batch.published", 1U);
   CHECK(partial.acceptance_first_patch_exact);
   CHECK(partial.partial_generation == 2U);
   CHECK(partial.partial_first_tile_count == 1U);
   CHECK(partial.first_patched_slots.at(2U) == std::map<std::uint64_t, std::uint64_t>{{1U, 11U}});
   partial.consume_explore_evidence("acceptance.slot.patched", 2U, 12U);
   partial.consume_explore_evidence("tile.batch.published", 2U);
   CHECK(partial.explore_ready_batch);
   CHECK_FALSE(partial.explore_tile_regressed);
  } else {
   CHECK_FALSE(partial.first_publication_ordinals.contains(2U));
  }
  snapshot["ready_slots"] = std::vector<bool>{true};
  BrowserAudit malformed;
  malformed.consume_gallery_generation(snapshot);
  CHECK_FALSE(malformed.bounds_valid);
  CHECK(malformed.gallery_generations.empty());
 }
}
TEST_CASE("surface join rejects duplicate claims and texture construction", "[workspace][audit]") {
 for (const char* repeated : {"firefox.workspace.claim_outcome", "iced.surface.texture_create"}) {
  SurfaceAudit audit;
  for (const char* event : browser_surface_events) {
   audit.browser(browser_surface_record(event));
   if (std::string_view{event} == repeated) audit.browser(browser_surface_record(event));
  }
  CHECK_FALSE(audit.failure.empty());
 }
}
TEST_CASE("surface join rejects otherwise complete traces for different physical identities", "[workspace][audit]") {
 SurfaceAudit audit;
 for (const char* event : native_surface_events) audit.native(native_surface_record(event, 12U));
 for (const char* event : browser_surface_events) audit.browser(browser_surface_record(event, 13U));
 CHECK_FALSE(audit.joined_failure().empty());
 CHECK_FALSE(audit.pending_supersession_completed());
}
enum class MissingHandoffEvidence {
 PendingDiscard,
 Reconstruction,
 CaptureBeforeDraw,
 DuplicateReconstruction,
 DifferentCompleted,
 Fallback,
 FallbackPublication,
 FallbackRequest,
 ReplacementPublication,
 AcquisitionAfterReconstruction,
 OlderCompleted,
 AbandonedFallback,
 AbandonedReplacement,
 FallbackSubmission,
 ReplacementSubmission,
 DuplicateFallbackSubmission,
 DuplicateReplacementSubmission
};
enum class HandoffSubmission { Submit, Omit, Duplicate, Abandon, Pending };
[[nodiscard]] SurfaceAudit pending_handoff(
 const std::optional<MissingHandoffEvidence> missing = std::nullopt, const bool pending_texture = true, const bool incumbent_after_admission = false, const bool retain_latest = false) {
 SurfaceAudit audit;
 const auto native = [&](const char* event, const std::uint64_t surface, const std::uint64_t publication = 1U) {
  auto record = native_surface_record(event, surface);
  record["sequence"] = surface;
  record["allocation_generation"] = surface;
  if (publication != 1U) {
   record["trace_id"] = 31U + publication;
   record["span_id"] = 31U + publication;
   if (scalar(record, "presentation_revision") != 0U) {
    record["value"] = publication;
    record["presentation_revision"] = publication;
    record["transfer_sequence"] = publication;
    record["timeline_ready"] = publication * 2U - 1U;
   }
  }
  audit.native(record);
 };
 const auto browser = [&](const char* event, const std::uint64_t surface, const std::uint64_t requested = 0U, const std::uint64_t publication = 1U,
                       const HandoffSubmission submission = HandoffSubmission::Submit) {
  auto record = browser_surface_record(event, surface);
  if (requested != 0U) record["requested_surface"] = SurfaceAudit::native_identity(native_surface_record("", requested));
  record["presentation_revision"] = publication;
  record["transfer_sequence"] = publication;
  record["timeline_ready"] = publication * 2U - 1U;
  record["timeline_release"] = publication * 2U;
  record["slot"] = (publication - 1U) % 2U;
  audit.browser(record);
  if (std::string_view{event} == "iced.surface.sample_draw_selected") {
   record["event"] = "iced.surface.draw_encoded";
   audit.browser(record);
   if (submission == HandoffSubmission::Submit || submission == HandoffSubmission::Duplicate || submission == HandoffSubmission::Pending) {
    record["event"] = "iced.surface.draw_submitted";
    audit.browser(record);
    if (submission == HandoffSubmission::Duplicate) audit.browser(record);
   }
   if (submission != HandoffSubmission::Pending) {
    record["event"] = submission == HandoffSubmission::Abandon ? "iced.frame.draw_abandoned" : "iced.frame.draw_settled";
    audit.browser(record);
   }
  }
 };
 constexpr std::uint64_t d = 11U, a = 12U, b = 13U, c = 14U;
 const bool newer_incumbent = incumbent_after_admission || missing == MissingHandoffEvidence::FallbackPublication || missing == MissingHandoffEvidence::AcquisitionAfterReconstruction ||
                              missing == MissingHandoffEvidence::OlderCompleted;
 const std::uint64_t retained_publication = newer_incumbent ? 2U : 1U;
 if (missing == MissingHandoffEvidence::DifferentCompleted)
  for (const char* event : native_surface_events) native(event, d);
 for (std::size_t stage = 0U; stage < 15U; ++stage) native(native_surface_events[stage], a);
 for (const auto surface : {b, c})
  for (std::size_t stage = 0U; stage < 4U; ++stage) native(native_surface_events[stage], surface);
 native("presentation.candidate.withdrawal", b);
 native("presentation.retirement", b);
 for (std::size_t stage = 4U; stage < 15U; ++stage) native(native_surface_events[stage], c);
 if (newer_incumbent)
  for (std::size_t stage = 7U; stage < 15U; ++stage) native(native_surface_events[stage], a, 2U);
 if (missing == MissingHandoffEvidence::ReplacementPublication)
  for (std::size_t stage = 7U; stage < 15U; ++stage) native(native_surface_events[stage], c, 2U);
 for (const auto surface : {a, c}) {
  if (retain_latest && surface == c) continue;
  native("presentation.active.withdrawal", surface);
  native("presentation.retirement", surface);
 }
 if (missing == MissingHandoffEvidence::DifferentCompleted)
  for (std::size_t stage = 0U; stage < 14U; ++stage) browser(browser_surface_events[stage], d);
 for (std::size_t stage = 0U; stage < 14U; ++stage) browser(browser_surface_events[stage], a);
 for (std::size_t stage = 0U; stage < 6U; ++stage)
  if (pending_texture || stage != 1U) browser(browser_surface_events[stage], b);
 if (newer_incumbent)
  for (std::size_t stage = 9U; stage < 13U; ++stage)
   if (stage != 12U || missing != MissingHandoffEvidence::AcquisitionAfterReconstruction) browser(browser_surface_events[stage], a, 0U, 2U);
 if (missing != MissingHandoffEvidence::Reconstruction)
  browser("iced.surface.renderer_reconstructed", missing == MissingHandoffEvidence::DifferentCompleted ? d : a, b, missing == MissingHandoffEvidence::OlderCompleted ? 1U : retained_publication);
 if (missing == MissingHandoffEvidence::DuplicateReconstruction) browser("iced.surface.renderer_reconstructed", a, b);
 if (missing == MissingHandoffEvidence::AcquisitionAfterReconstruction) browser("iced.surface.sample_acquired", a, 0U, 2U);
 if (missing != MissingHandoffEvidence::Fallback)
  browser("iced.surface.sample_draw_selected", a, pending_texture || missing == MissingHandoffEvidence::FallbackRequest ? b : a,
   missing == MissingHandoffEvidence::FallbackPublication ? 1U : retained_publication,
   missing == MissingHandoffEvidence::AbandonedFallback              ? HandoffSubmission::Abandon
   : missing == MissingHandoffEvidence::FallbackSubmission           ? HandoffSubmission::Omit
    : missing == MissingHandoffEvidence::DuplicateFallbackSubmission ? HandoffSubmission::Duplicate
                                                                     : HandoffSubmission::Submit);
 if (missing == MissingHandoffEvidence::AbandonedFallback && pending_texture) browser("iced.surface.sample_draw_selected", a, a, retained_publication);
 if (pending_texture || missing != MissingHandoffEvidence::PendingDiscard) browser("firefox.workspace.drop_received", b);
 for (std::size_t stage = 0U; stage < 2U; ++stage) browser(browser_surface_events[stage], c);
 if (pending_texture) {
  if (missing != MissingHandoffEvidence::PendingDiscard) browser("iced.surface.pending_discarded", b);
  browser("iced.surface.import_dropped", b);
  browser("iced.surface.texture_destroyed", b);
 }
 browser("firefox.workspace.retired", b);
 browser("iced.surface.sample_draw_selected", a, c, retained_publication);
 for (std::size_t stage = 2U; stage < 6U; ++stage) browser(browser_surface_events[stage], c);
 for (std::size_t stage = 6U; stage < 12U; ++stage) browser(browser_surface_events[stage], c);
 if (missing == MissingHandoffEvidence::CaptureBeforeDraw) {
  browser("iced.surface.sample_draw_selected", c);
  browser("iced.surface.sample_acquired", c);
 } else {
  browser("iced.surface.sample_acquired", c);
  browser("iced.surface.sample_draw_selected", a, c, retained_publication);
  if (missing == MissingHandoffEvidence::ReplacementPublication)
   for (std::size_t stage = 9U; stage < 13U; ++stage) browser(browser_surface_events[stage], c, 0U, 2U);
  browser("iced.surface.sample_draw_selected", c, 0U, 1U,
   missing == MissingHandoffEvidence::AbandonedReplacement              ? HandoffSubmission::Abandon
   : missing == MissingHandoffEvidence::ReplacementSubmission           ? HandoffSubmission::Omit
    : missing == MissingHandoffEvidence::DuplicateReplacementSubmission ? HandoffSubmission::Duplicate
                                                                        : HandoffSubmission::Submit);
  if (missing == MissingHandoffEvidence::AbandonedReplacement) browser("iced.surface.sample_draw_selected", c, a);
 }
 if (newer_incumbent) browser("iced.frame.sample_released", a, 0U, 2U);
 if (missing == MissingHandoffEvidence::ReplacementPublication) browser("iced.frame.sample_released", c, 0U, 2U);
 for (const auto surface : {a, c}) {
  if (retain_latest && surface == c) {
   browser("iced.surface.sample_draw_selected", c, c, 1U, HandoffSubmission::Pending);
   continue;
  }
  for (std::size_t stage = browser_live_stages; stage < browser_surface_events.size(); ++stage) browser(browser_surface_events[stage], surface);
 }
 if (missing == MissingHandoffEvidence::DifferentCompleted)
  for (std::size_t stage = browser_live_stages; stage < browser_surface_events.size(); ++stage) browser(browser_surface_events[stage], d);
 return audit;
}
TEST_CASE("rapid surface join requires a complete pending-candidate handoff", "[workspace][audit]") {
 const bool pending_texture = GENERATE(false, true);
 CAPTURE(pending_texture);
 const auto complete = pending_handoff(std::nullopt, pending_texture);
 REQUIRE(complete.joined_failure().empty());
 CHECK(complete.pending_supersession_completed());
 const auto progressed = pending_handoff(std::nullopt, pending_texture, true);
 INFO("post-admission incumbent completion: " << progressed.joined_failure());
 REQUIRE(progressed.joined_failure().empty());
 CHECK(progressed.pending_supersession_completed());
 const auto drawing = pending_handoff(std::nullopt, pending_texture, true, true);
 REQUIRE(drawing.evidence_settled());
 CHECK(drawing.pending_supersession_completed());
 for (const auto missing : {MissingHandoffEvidence::PendingDiscard, MissingHandoffEvidence::Reconstruction, MissingHandoffEvidence::CaptureBeforeDraw, MissingHandoffEvidence::DuplicateReconstruction,
       MissingHandoffEvidence::DifferentCompleted, MissingHandoffEvidence::Fallback, MissingHandoffEvidence::FallbackPublication, MissingHandoffEvidence::FallbackRequest,
       MissingHandoffEvidence::ReplacementPublication, MissingHandoffEvidence::AcquisitionAfterReconstruction, MissingHandoffEvidence::OlderCompleted, MissingHandoffEvidence::AbandonedFallback,
       MissingHandoffEvidence::AbandonedReplacement, MissingHandoffEvidence::FallbackSubmission, MissingHandoffEvidence::ReplacementSubmission, MissingHandoffEvidence::DuplicateFallbackSubmission,
       MissingHandoffEvidence::DuplicateReplacementSubmission}) {
  if (pending_texture && missing == MissingHandoffEvidence::FallbackRequest) continue;
  const auto audit = pending_handoff(missing, pending_texture);
  INFO("surface join: " << audit.joined_failure());
  if (missing != MissingHandoffEvidence::CaptureBeforeDraw && missing != MissingHandoffEvidence::DuplicateReconstruction && missing != MissingHandoffEvidence::AcquisitionAfterReconstruction &&
      missing != MissingHandoffEvidence::DuplicateFallbackSubmission && missing != MissingHandoffEvidence::DuplicateReplacementSubmission &&
      (pending_texture || missing != MissingHandoffEvidence::PendingDiscard))
   CHECK(audit.joined_failure().empty());
  else
   CHECK_FALSE(audit.joined_failure().empty());
  CHECK_FALSE(audit.pending_supersession_completed());
 }
}
[[nodiscard]] nlohmann::json atlas_draw_record(const std::uint64_t revision, const std::uint64_t first_row, const std::uint64_t rows, const double top) {
 nlohmann::json record{
  {"event", "iced.surface.draw_encoded"},
  {"control", kExploreGalleryControl},
  {"source_revision", revision},
  {"frame_revision", revision},
  {"presentation_revision", revision + 100U},
  {"content_session", 1U},
  {"source_kind", 1U},
  {"source_instance", 1U},
  {"dataset_identity", 11U},
  {"gallery_generation", revision},
  {"surface", "000000000000000b000000000000000c"},
  {"width", 512U},
  {"height", 1024U},
  {"layer", 0U},
  {"slot", 0U},
  {"content_width", 400U},
  {"content_height", 800U},
  {"row_capacity", 8U},
  {"row_origin", first_row % 8U},
  {"card_extent", 100U},
  {"augmentation_enabled", false},
  {"augmentation_seed", 0U},
  {"ready_slots", std::vector<bool>{true, true}},
  {"columns", 4U},
  {"rows", rows},
  {"first_row", first_row},
  {"matching_count", 300U},
  {"visible_indices", std::vector<std::uint64_t>{first_row * 4U, first_row * 4U + 1U}},
  {"bounds", std::array{30.0, top, 600.0, static_cast<double>(rows) * 150.0}},
  {"clip", std::array{30.0, 0.0, 600.0, 500.0}},
 };
 record["image"] = record["bounds"];
 return record;
}
TEST_CASE("Explore clipboard evidence requires an exact seed and authoritative restoration", "[workspace][audit]") {
 for (const std::string_view defect : {"none", "alternate", "target-mismatch", "baseline-collision", "typed-only", "rounded", "control", "persistence", "baseline", "revision"}) {
  INFO(defect);
  BrowserAudit audit;
  const auto entry = [](const char* event, const char* value, const std::uint64_t before, const std::uint64_t after) {
   return nlohmann::json{{"event", event}, {"control", "seed-field"}, {"detail", value}, {"a", before}, {"b", after}, {"c", 1U}, {"d", 1U}};
  };
  const auto* target = defect == "alternate" ? "9007199254740995" : "9007199254740993";
  const auto* baseline = defect == "alternate" || defect == "baseline-collision" ? "9007199254740993" : "73";
  audit.consume(entry("integration.explore_integer", target, 1U, 2U));
  audit.consume(entry("integration.explore_integer_paste_baseline", baseline, 3U, 0U));
  auto pasted = entry("integration.explore_integer_paste", target, 3U, 4U);
  if (defect == "target-mismatch") pasted["detail"] = "9007199254740995";
  if (defect == "rounded") pasted["detail"] = "9007199254740992";
  if (defect == "control") pasted["control"] = "another-field";
  if (defect == "persistence") pasted["d"] = 0U;
  if (defect != "typed-only") audit.consume(pasted);
  auto restored = entry("integration.explore_integer_paste_restored", baseline, 4U, 5U);
  if (defect == "baseline") restored["detail"] = "74";
  if (defect == "revision") restored["b"] = 4U;
  audit.consume(restored);
  CHECK(audit.explore_integer_precision);
  CHECK(audit.explore_paste_restored == (defect == "none" || defect == "alternate"));
 }
}
TEST_CASE("initial atlas completion requires nonblack canvas pixels for the exact publication", "[workspace][audit]") {
 const nlohmann::json completed{{"event", "integration.initial_atlas_complete"}, {"detail", "no-input-canvas-pixels"}, {"a", 17U}, {"b", 3U}, {"c", 23U}, {"d", 29U}};
 for (const auto* defect : {"missing", "black", "empty", "source", "publication", "none"}) {
  INFO(defect);
  BrowserAudit audit;
  nlohmann::json pixels{{"event", "integration.atlas_canvas_pixels"}, {"a", 23U}, {"b", 29U}, {"c", 4U}, {"d", 4U}};
  const std::string_view kind{defect};
  if (kind == "black") pixels["d"] = 0U;
  if (kind == "empty") {
   pixels["c"] = 0U;
   pixels["d"] = 0U;
  }
  if (kind == "source") pixels["a"] = 22U;
  if (kind == "publication") pixels["b"] = 28U;
  if (kind != "missing") audit.consume(pixels);
  audit.consume(completed);
  CHECK(audit.initial_atlas_complete == (kind == "none"));
 }
}
TEST_CASE("annotation canvas palette evidence preserves class hue contrast and solid control colors", "[workspace][audit]") {
 struct Sample final {
  std::array<double, 3U> expected;
  std::array<double, 3U> observed;
  double scale;
  bool valid;
  const char* event = "integration.annotation_pixel";
 };
 constexpr std::array samples{
  Sample{{255, 255, 0}, {255, 255, 0}, 1.0, true},
  Sample{{255, 255, 0}, {228, 229, 8}, 1148.0 / 2048.0, true},
  Sample{{255, 255, 0}, {247, 248, 26}, 1148.0 / 2048.0, true},
  Sample{{255, 255, 0}, {156, 171, 53}, 910.0 / 2048.0, true},
  Sample{{255, 80.52631258964539, 0}, {131, 80, 67}, 910.0 / 2048.0, true},
  Sample{{255, 80.52631258964539, 0}, {90, 80, 89}, 910.0 / 2048.0, false},
  Sample{{255, 80.52631258964539, 0}, {131, 67, 80}, 910.0 / 2048.0, false},
  Sample{{183.60000729560852, 82.62000024318695, 98.56424868106842}, {110, 81, 106}, 910.0 / 2048.0, true},
  Sample{{183.60000729560852, 82.62000024318695, 98.56424868106842}, {48, 80, 112}, 910.0 / 2048.0, false},
  Sample{{48, 80, 112}, {48, 80, 112}, 910.0 / 2048.0, true},
  Sample{{255, 255, 0}, {48, 80, 112}, 910.0 / 2048.0, false},
  Sample{{0, 255, 0}, {44, 218, 10}, 1148.0 / 2048.0, true},
  Sample{{0, 255, 0}, {55, 218, 10}, 1148.0 / 2048.0, false},
  Sample{{255, 255, 0}, {228, 229, 8}, 1.0, false},
  Sample{{255, 255, 0}, {255, 0, 0}, .5, false},
  Sample{{255, 255, 0}, {200, 200, 200}, .5, false},
  Sample{{255, 255, 0}, {200, 200, 80}, .5, false},
  Sample{{255, 255, 0}, {90, 90, 0}, .5, false},
  Sample{{255, 255, 255}, {228, 229, 228}, .5, false},
  Sample{{255, 255, 0}, {500, 500, 0}, .5, false},
  Sample{{255, 255, 0}, {255, 255, 0}, 0.0, false},
  Sample{{255, 255, 0}, {228, 229, 8}, .5, false, "integration.annotation_swatch"},
  Sample{{32, 32, 32}, {32, 32, 32}, .5, true, "integration.annotation_capability"},
 };
 for (const auto& sample : samples) {
  BrowserAudit audit;
  audit.consume({{"event", sample.event}, {"a", 17U}, {"b", 23U}, {"expected", sample.expected}, {"observed", sample.observed}, {"source_to_screen", sample.scale}, {"matched", true}});
  CHECK(audit.annotation_pixels_valid == sample.valid);
 }
}
TEST_CASE("Paired empty atlases replace displayed content through an actual acquired draw", "[workspace][audit]") {
 const std::string fault = GENERATE("none", "source", "acquisition", "unsubmitted", "visible_cells", "negative_count");
 CAPTURE(fault);
 BrowserAudit audit;
 const auto prior = atlas_draw_record(23U, 0U, 4U, 0.0);
 for (const auto* event : {"iced.gallery.source", "iced.surface.sample_acquired", "iced.surface.draw_encoded"}) {
  auto record = prior;
  record["event"] = event;
  audit.consume(record);
 }
 REQUIRE(audit.atlas_draws.valid);
 REQUIRE(audit.owned_atlas_current);
 REQUIRE_FALSE(audit.atlas_draws.empty_seen);
 auto empty = atlas_draw_record(24U, 0U, 4U, 0.0);
 empty["matching_count"] = 0U;
 empty["visible_indices"] = nlohmann::json::array();
 empty["ready_slots"] = nlohmann::json::array();
 if (fault == "visible_cells") {
  empty["visible_indices"] = std::vector<std::uint64_t>{0U};
  empty["ready_slots"] = std::vector<bool>{true};
 }
 if (fault == "negative_count") empty["matching_count"] = -1;
 for (const auto* event : {"iced.gallery.source", "iced.surface.sample_acquired", "iced.surface.draw_encoded"}) {
  const std::string_view name{event};
  if ((fault == "source" && name == "iced.gallery.source") || (fault == "acquisition" && name == "iced.surface.sample_acquired")) continue;
  auto record = empty;
  record["event"] = fault == "unsubmitted" && name == "iced.surface.draw_encoded" ? "iced.surface.sample_draw_selected" : event;
  audit.consume(record);
  CHECK(audit.owned_atlas_current);
  if (name != "iced.surface.draw_encoded") CHECK_FALSE(audit.atlas_draws.empty_seen);
 }
 CHECK(audit.atlas_draws.empty_seen == (fault == "none"));
 if (fault == "none") {
  REQUIRE(audit.atlas_draws.valid);
  REQUIRE(audit.atlas_draws.last_draw.has_value());
  CHECK(scalar(*audit.atlas_draws.last_draw, "source_revision") == 24U);
  CHECK(scalar(*audit.atlas_draws.last_draw, "matching_count") == 0U);
 }
}
TEST_CASE("atlas visibility requires an encoded intersecting draw with exact source rows", "[workspace][audit]") {
 const auto draw = atlas_draw_record(23U, 2U, 5U, -37.25);
 auto source = draw;
 source["event"] = "iced.gallery.source";
 auto capture = draw;
 capture["event"] = "iced.surface.sample_acquired";
 const nlohmann::json missing{{"event", "iced.surface.sample_draw_missing"}, {"control", kExploreGalleryControl}};
 BrowserAudit observed;
 observed.consume(missing);
 CHECK_FALSE(observed.owned_atlas_seen);
 CHECK_FALSE(observed.owned_atlas_interrupted);
 observed.consume(source);
 observed.consume(capture);
 observed.consume(draw);
 const auto& complete = observed.atlas_draws;
 CHECK(complete.valid);
 CHECK(complete.seen);
 CHECK(complete.drawn_rows.contains(2U));
 for (const auto* control : {"", kExploreGalleryControl, "explore.detail.workspace"}) {
  auto browser = observed;
  REQUIRE(browser.owned_atlas_seen);
  browser.consume({{"event", "iced.surface.sample_draw_missing"}, {"control", control}});
  CHECK(browser.owned_atlas_interrupted == (std::string_view{control} != "explore.detail.workspace"));
  browser.consume(missing);
  CHECK(browser.owned_atlas_interrupted);
  browser.consume(draw);
  CHECK(browser.owned_atlas_interrupted);
 }
 for (const auto* control : {"explore.detail.workspace", "workflow.visual.workspace", "unknown.workspace"}) {
  auto browser = observed;
  auto replacement = draw;
  replacement["control"] = control;
  browser.consume(replacement);
  const bool unknown = std::string_view{control} == "unknown.workspace";
  CHECK(browser.owned_atlas_current == unknown);
  browser.consume(missing);
  CHECK(browser.owned_atlas_interrupted == unknown);
  browser.consume(draw);
  CHECK(browser.owned_atlas_current);
  CHECK(browser.owned_atlas_seen);
  browser.consume(missing);
  CHECK(browser.owned_atlas_interrupted);
  browser.consume(replacement);
  CHECK(browser.owned_atlas_interrupted);
 }
 for (const auto* event : {"iced.gallery.source", "iced.surface.renderer_reconstructed", "iced.surface.sample_acquired", "iced.surface.sample_draw_selected"}) {
  auto browser = observed;
  auto pending = atlas_draw_record(24U, 3U, 5U, -37.25);
  pending["event"] = event;
  pending["control"] = "explore.detail.workspace";
  pending["surface"] = "000000000000000b000000000000000d";
  browser.consume(pending);
  CHECK(browser.owned_atlas_current);
  browser.consume(missing);
  CHECK(browser.owned_atlas_interrupted);
 }
 for (const auto* defect : {"offscreen", "grid", "row", "indices", "source", "source-instance", "session", "pending", "capture", "selection"}) {
  INFO(defect);
  AtlasDrawAudit audit;
  audit.consume(source);
  if (std::string_view{defect} != "capture") audit.consume(capture);
  auto invalid = draw;
  const std::string_view kind{defect};
  if (kind == "offscreen") invalid["image"][1] = 700.0;
  if (kind == "grid") invalid["rows"] = 4U;
  if (kind == "row") invalid["first_row"] = 10U;
  if (kind == "indices") invalid["visible_indices"][0] = 99U;
  if (kind == "source") invalid["source_revision"] = 24U;
  if (kind == "source-instance") invalid["source_instance"] = 2U;
  if (kind == "session") invalid["content_session"] = 3U;
  if (kind == "pending") invalid["presentation_revision"] = 2U;
  if (kind == "selection") invalid["event"] = "iced.surface.sample_draw_selected";
  audit.consume(invalid);
  CHECK_FALSE(audit.seen);
  if (kind != "selection") CHECK_FALSE(audit.valid);
 }
}
TEST_CASE("atlas stages require fresh complete draw identity and reject ambiguous sources", "[workspace][audit]") {
 AtlasDrawAudit complete;
 constexpr std::array names{"fractional", "row1", "row2", "row10", "row9", "end", "restored"};
 constexpr std::array<std::uint64_t, 7U> first_rows{0U, 1U, 2U, 10U, 9U, 71U, 0U};
 for (std::size_t index = 0U; index != names.size(); ++index) {
  const auto rows = index % 2U == 0U ? 5U : 4U;
  const double top = index == 5U ? -100.0 : (index == 0U || index == 2U ? -37.25 : 0.0);
  const auto draw = atlas_draw_record(23U + index, first_rows[index], rows, top);
  auto source = draw;
  source["event"] = "iced.gallery.source";
  auto capture = draw;
  capture["event"] = "iced.surface.sample_acquired";
  complete.consume(source);
  complete.consume(capture);
  complete.consume(draw);
  auto stage = draw;
  stage["event"] = "iced.surface.scroll_stage";
  stage["control"] = names[index];
  for (const auto* defect : {"source", "surface", "width", "height", "presentation", "row", "clip", "missing", "clipped"}) {
   CAPTURE(index, defect);
   auto invalid_audit = complete;
   auto invalid_stage = stage;
   const std::string_view kind{defect};
   if (kind == "source") invalid_stage["source_instance"] = 2U;
   if (kind == "surface") invalid_stage["surface"] = "000000000000000b000000000000000d";
   if (kind == "width") invalid_stage["width"] = 1024U;
   if (kind == "height") invalid_stage["height"] = scalar(stage, "height") + 1U;
   if (kind == "presentation") invalid_stage["presentation_revision"] = 1U;
   if (kind == "row") invalid_stage["first_row"] = 99U;
   if (kind == "clip") invalid_stage["clip"][1] = 20.0;
   if (kind == "missing") invalid_audit.last_draw.reset();
   if (kind == "clipped") {
    auto clipped = draw;
    clipped["event"] = "iced.surface.sample_draw_clipped";
    invalid_audit.consume(clipped);
   }
   invalid_audit.consume(invalid_stage);
   CHECK_FALSE(invalid_audit.valid);
  }
  complete.consume(stage);
  CHECK(complete.valid);
  CHECK(complete.stages.contains(names[index]));
  auto duplicate = complete;
  source["ready_slots"] = std::vector<bool>{true, true};
  source["gallery_generation"] = scalar(source, "gallery_generation") + 1U;
  source["source_observation_revision"] = scalar(source, "source_observation_revision") + 1U;
  duplicate.consume(source);
  CHECK(duplicate.valid);
  auto observed_draw = draw;
  observed_draw["gallery_generation"] = source["gallery_generation"];
  observed_draw["source_observation_revision"] = source["source_observation_revision"];
  duplicate.consume(observed_draw);
  CHECK(duplicate.valid);
  for (const auto* field : {"dataset_identity", "first_row", "content_width"}) {
   auto conflicting = complete;
   auto conflicting_source = source;
   conflicting_source[field] = scalar(source, field) + 1U;
   conflicting.consume(conflicting_source);
   CHECK_FALSE(conflicting.valid);
  }
  auto ambiguous = complete;
  source["source_instance"] = 2U;
  ambiguous.consume(source);
  CHECK_FALSE(ambiguous.valid);
 }
 CHECK(complete.grid_round_trip);
 CHECK(complete.stages.size() == names.size());
}
TEST_CASE("atlas grid transitions belong only to consecutive stages on one allocation", "[workspace][audit]") {
 constexpr std::array names{"fractional", "row1", "row2", "row10", "row9", "end", "restored"};
 const auto publish = [](AtlasDrawAudit& audit, nlohmann::json draw) {
  for (const auto* event : {"iced.gallery.source", "iced.surface.sample_acquired", "iced.surface.draw_encoded"}) {
   draw["event"] = event;
   audit.consume(draw);
  }
 };
 AtlasDrawAudit between_stages;
 for (std::size_t index = 0U; index != names.size(); ++index) {
  const std::array<std::uint64_t, 7U> first_rows{0U, 1U, 2U, 10U, 9U, 71U, 0U};
  const double top = index == 5U ? -100.0 : (index == 0U ? -37.25 : 0.0);
  auto draw = atlas_draw_record(70U + index, first_rows[index], 4U, top);
  publish(between_stages, draw);
  draw["event"] = "iced.surface.scroll_stage";
  draw["control"] = names[index];
  between_stages.consume(draw);
  if (index == 0U) publish(between_stages, atlas_draw_record(80U, 0U, 5U, -37.25));
 }
 CHECK(between_stages.valid);
 CHECK(between_stages.grid_round_trip);
 for (const auto* scenario : {"constant", "only-grow", "only-shrink", "surface-high", "surface-low", "width", "height"}) {
  INFO(scenario);
  const std::string_view kind{scenario};
  const bool replacement = kind == "surface-high" || kind == "surface-low" || kind == "width" || kind == "height";
  AtlasDrawAudit audit;
  // These valid draws contain a complete round trip but belong to no stage.
  for (std::uint64_t index = 0U; index != 3U; ++index) publish(audit, atlas_draw_record(10U + index, 0U, index == 1U ? 5U : 4U, 0.0));
  CHECK(audit.valid);
  CHECK_FALSE(audit.grid_round_trip);
  for (std::size_t index = 0U; index != names.size(); ++index) {
   std::uint64_t rows = 4U;
   if (kind == "only-grow") rows = index < 2U ? 4U : 5U;
   if (kind == "only-shrink") rows = index < 2U ? 5U : 4U;
   if (replacement) rows = index % 2U == 0U ? 5U : 4U;
   const std::array<std::uint64_t, 7U> first_rows{0U, 1U, 2U, 10U, 9U, 75U - rows, 0U};
   const double top = index == 5U ? 500.0 - static_cast<double>(rows) * 150.0 : (index == 0U || index == 2U ? -37.25 : 0.0);
   auto draw = atlas_draw_record(30U + index, first_rows[index], rows, top);
   if (index != 0U) {
    if (kind == "surface-high") draw["surface"] = "000000000000000d000000000000000c";
    if (kind == "surface-low") draw["surface"] = "000000000000000b000000000000000d";
    if (kind == "width") draw["width"] = 1024U;
    if (kind == "height") draw["height"] = scalar(draw, "height") + 1U;
   }
   publish(audit, draw);
   CHECK(audit.valid);
   draw["event"] = "iced.surface.scroll_stage";
   draw["control"] = names[index];
   audit.consume(draw);
   if (replacement && index == 1U) {
    CHECK_FALSE(audit.valid);
    CHECK(audit.stages.size() == 1U);
    break;
   }
   CHECK(audit.valid);
  }
  CHECK_FALSE(audit.grid_round_trip);
  if (!replacement) {
   CHECK(audit.stages.size() == names.size());
   CHECK(audit.staged_transitions == (kind == "only-grow" ? 1U : (kind == "only-shrink" ? 2U : 0U)));
  }
 }
}
TEST_CASE("Source retirement remains independent of a retained sample arena", "[workspace][audit]") {
 SurfaceAudit audit;
 for (std::size_t stage = 0U; stage < 15U; ++stage) audit.native(native_surface_record(native_surface_events[stage]));
 for (std::size_t stage = 0U; stage < browser_live_stages; ++stage) audit.browser(browser_surface_record(browser_surface_events[stage]));
 for (const auto* event : {"presentation.source.withdrawal", "presentation.source.retirement"}) audit.native(native_surface_record(event));
 for (const auto* event : {"firefox.workspace.source.withdrawal", "firefox.workspace.source.retired"}) audit.browser(browser_surface_record(event));
 REQUIRE(audit.failure.empty());
 CHECK(audit.surfaces.size() == 1U);
 CHECK(audit.sources.size() == 1U);
 CHECK(audit.evidence_settled());
 audit.browser(browser_surface_record("iced.surface.sample_draw_selected"));
 audit.browser(browser_surface_record("iced.surface.draw_encoded"));
 audit.browser(browser_surface_record("iced.surface.draw_submitted"));
 audit.browser(browser_surface_record("iced.frame.draw_settled"));
 CHECK(audit.failure.empty());
 CHECK(audit.evidence_settled());
 audit.browser(browser_surface_record("firefox.workspace.source.retired"));
 CHECK_FALSE(audit.failure.empty());
}
TEST_CASE("Source withdrawal permits claimed initialization to finish before retirement", "[workspace][audit]") {
 const auto native_early = GENERATE(false, true);
 const auto browser_early = GENERATE(false, true);
 const auto install_native_timeline = GENERATE(false, true);
 SurfaceAudit audit;
 for (const bool browser : {false, true}) {
  const std::array events = browser ? std::array{"firefox.workspace.source.admitted", "firefox.workspace.source.claim_outcome", "firefox.workspace.source.ready", "firefox.workspace.source.withdrawal",
                                       "firefox.workspace.source.retired"}
                                    : std::array{"presentation.source.admission.enqueued", "presentation.source.admission.written", "presentation.source.ready", "presentation.source.withdrawal",
                                       "presentation.source.retirement"};
  const auto observe = [&](SurfaceAudit& target, const std::size_t stage) {
   if (browser)
    target.browser(browser_surface_record(events[stage]));
   else
    target.native(native_surface_record(events[stage]));
  };
  observe(audit, 0U);
  auto missing_claim = audit;
  observe(missing_claim, 2U);
  CHECK_FALSE(missing_claim.failure.empty());
  observe(audit, 1U);
  auto premature_retirement = audit;
  observe(premature_retirement, 4U);
  CHECK_FALSE(premature_retirement.failure.empty());
  const bool early = browser ? browser_early : native_early;
  if (!browser && !install_native_timeline) {
   observe(audit, 3U);
  } else {
   observe(audit, early ? 3U : 2U);
   observe(audit, early ? 2U : 3U);
  }
  auto duplicate = audit;
  observe(duplicate, 3U);
  CHECK_FALSE(duplicate.failure.empty());
  observe(audit, 4U);
  auto after_retirement = audit;
  observe(after_retirement, 2U);
  CHECK_FALSE(after_retirement.failure.empty());
  REQUIRE(audit.failure.empty());
 }
 CHECK(audit.evidence_settled());
}
TEST_CASE("Source cancellation settles the exact unclaimed admission without a ready product", "[workspace][audit]") {
 const auto omitted = GENERATE("", "failure", "withdrawal", "retirement");
 SurfaceAudit audit;
 for (const auto* event : {"presentation.source.admission.enqueued", "presentation.source.admission.written", "presentation.source.withdrawal", "presentation.source.retirement"})
  audit.native(native_surface_record(event));
 audit.browser(browser_surface_record("firefox.workspace.source.admitted"));
 if (std::string_view{omitted} != "withdrawal") audit.browser(browser_surface_record("firefox.workspace.source.withdrawal"));
 auto cancelled = browser_surface_record("firefox.workspace.source.import_failed");
 cancelled["code"] = 1U;
 if (std::string_view{omitted} != "failure") audit.browser(cancelled);
 if (std::string_view{omitted} != "retirement") audit.browser(browser_surface_record("firefox.workspace.source.retired"));
 CHECK(audit.evidence_settled() == std::string_view{omitted}.empty());
 if (std::string_view{omitted}.empty()) {
  audit.browser(cancelled);
  CHECK_FALSE(audit.failure.empty());
 }
}
TEST_CASE("Source publication audit requires exact physical receiver receipts", "[workspace][audit]") {
 for (const std::string_view fault : {"none", "missing", "duplicate", "reordered", "arena", "content", "presentation", "transfer", "source", "allocation", "source-generation", "source-width"}) {
  INFO("physical receipt fault: " << fault);
  SurfaceAudit audit;
  for (const auto* event : native_surface_events) {
   auto record = native_surface_record(event);
   if (std::string_view{event} == "presentation.source.read_submitted") {
    if (fault == "allocation") record["workspace_allocation"] = 999U;
    if (fault == "source") record["workspace_source_low"] = 999U;
   }
   if (std::string_view{event} == "presentation.source.ready") {
    if (fault == "source-generation") record["sequence"] = 8U;
    if (fault == "source-width") record["capacity_width"] = 65U;
   }
   audit.native(record);
  }
  for (const auto* event : browser_surface_events) {
   const std::string_view name{event};
   if (name == "firefox.workspace.copy_completed" && fault == "missing") continue;
   auto record = browser_surface_record(event);
   if (name == "firefox.workspace.frame_forwarded" && fault == "transfer") {
    record["transfer_sequence"] = 2U;
    record["timeline_ready"] = 3U;
    record["timeline_release"] = 4U;
   }
   if (name == "firefox.workspace.copy_completed") {
    if (fault == "arena") record["surface"] = SurfaceAudit::native_identity(native_surface_record("", 13U));
    if (fault == "content") record["content_sequence"] = 24U;
    if (fault == "presentation") record["presentation_revision"] = 2U;
   }
   if (name == "firefox.workspace.frame_dispatched" && fault == "reordered") audit.browser(browser_surface_record("firefox.workspace.copy_completed"));
   audit.browser(record);
   if (name == "firefox.workspace.copy_completed" && fault == "duplicate") audit.browser(record);
  }
  CHECK(audit.evidence_settled() == (fault == "none"));
  CHECK(audit.joined_failure().empty() == (fault == "none"));
 }
}
TEST_CASE("Source publication joins survive independent drains and retirement before copy evidence", "[workspace][audit]") {
 SurfaceAudit audit;
 for (const auto* event : browser_surface_events)
  if (std::string_view{event} != "firefox.workspace.copy_completed") audit.browser(browser_surface_record(event));
 for (const auto* event : native_surface_events) audit.native(native_surface_record(event));
 for (const auto* event : {"presentation.source.withdrawal", "presentation.source.retirement"}) audit.native(native_surface_record(event));
 for (const auto* event : {"firefox.workspace.source.withdrawal", "firefox.workspace.source.retired"}) audit.browser(browser_surface_record(event));
 REQUIRE(audit.failure.empty());
 CHECK_FALSE(audit.evidence_settled());
 audit.SettleScenario();
 CHECK(audit.surfaces.size() == 1U);
 CHECK(audit.sources.size() == 1U);
 audit.browser(browser_surface_record("firefox.workspace.copy_completed"));
 REQUIRE(audit.evidence_settled());
 audit.SettleScenario();
 CHECK(audit.surfaces.empty());
 CHECK(audit.sources.empty());
}
TEST_CASE("Direct fallback custody requires positive source read settlement without copy evidence", "[workspace][audit]") {
 for (const std::string_view fault : {"none", "missing", "mode", "copy", "early-destruction", "pending-release"}) {
  INFO("direct receipt fault: " << fault);
  SurfaceAudit audit;
  const auto native = [&](const char* event) {
   auto record = native_surface_record(event);
   record["direct_sampling"] = true;
   audit.native(record);
  };
  const auto browser = [&](const char* event) {
   auto record = browser_surface_record(event);
   record["direct_sampling"] = true;
   audit.browser(record);
  };
  for (std::size_t stage = 0U; stage < 13U; ++stage) native(native_surface_events[stage]);
  for (std::size_t stage = 0U; stage < browser_live_stages; ++stage) {
   if (std::string_view{browser_surface_events[stage]} == "firefox.workspace.copy_completed") continue;
   browser(browser_surface_events[stage]);
   if (std::string_view{browser_surface_events[stage]} == "iced.surface.texture_create") browser("iced.surface.source_texture_create");
  }
  REQUIRE(audit.failure.empty());
  CHECK(audit.evidence_settled());
  CHECK_FALSE(audit.joined_failure().empty());
  browser("iced.surface.sample_draw_selected");
  browser("iced.surface.draw_encoded");
  browser("iced.surface.draw_submitted");
  CHECK(audit.evidence_settled());
  CHECK_FALSE(audit.joined_failure().empty());
  if (fault != "pending-release") browser("iced.frame.draw_settled");
  if (fault == "early-destruction") browser("iced.surface.source_texture_destroyed");
  if (fault == "copy") browser("firefox.workspace.copy_completed");
  browser("iced.frame.sample_released");
  CHECK_FALSE(audit.evidence_settled());
  native("presentation.release_wait.started");
  CHECK_FALSE(audit.evidence_settled());
  native("presentation.release_wait.completed");
  CHECK_FALSE(audit.evidence_settled());
  if (fault == "mode")
   audit.browser(browser_surface_record("firefox.workspace.read_settled"));
  else if (fault != "missing")
   browser("firefox.workspace.read_settled");
  browser("firefox.workspace.withdrawal");
  browser("iced.surface.source_texture_destroyed");
  browser("iced.surface.texture_destroyed");
  browser("firefox.workspace.retired");
  native("presentation.active.withdrawal");
  native("presentation.retirement");
  INFO(audit.joined_failure());
  CHECK(audit.joined_failure().empty() == (fault == "none"));
 }
}
TEST_CASE("Release-only custody settles exact GPU work without creating a page sample", "[workspace][audit]") {
 const bool direct = GENERATE(false, true);
 const bool receiver_first = GENERATE(false, true);
 SurfaceAudit audit;
 const auto native = [&](const char* event) {
  auto record = native_surface_record(event);
  record["direct_sampling"] = direct;
  audit.native(record);
 };
 for (std::size_t stage = 0U; stage < 13U; ++stage) native(native_surface_events[stage]);
 for (std::size_t stage = 0U; stage < 9U; ++stage) {
  auto record = browser_surface_record(browser_surface_events[stage]);
  record["direct_sampling"] = direct;
  audit.browser(record);
 }
 audit.browser(release_only_record("firefox.workspace.release_only_submitted"));
 REQUIRE(audit.failure.empty());
 CHECK_FALSE(audit.evidence_settled());
 native("presentation.release_wait.started");
 CHECK_FALSE(audit.evidence_settled());
 if (receiver_first) {
  audit.browser(release_only_record("firefox.workspace.release_only_completed"));
  CHECK_FALSE(audit.evidence_settled());
 }
 native("presentation.release_wait.completed");
 if (!receiver_first) {
  CHECK_FALSE(audit.evidence_settled());
  audit.browser(release_only_record("firefox.workspace.release_only_completed"));
 }
 REQUIRE(audit.evidence_settled());
 const auto& state = audit.surfaces.at(SurfaceAudit::native_identity(native_surface_record("")));
 CHECK(state.receipts.at(1U).release_only);
 CHECK(state.receipts.at(1U).direct_sampling == direct);
 CHECK(state.transfers.at({1U, 1U}).released);
 CHECK(state.samples.empty());
 CHECK(state.custody.empty());
 CHECK(audit.draws.empty());
}
TEST_CASE("Two completed outputs join independent presentation and release-only custody", "[workspace][audit]") {
 SurfaceAudit audit;
 record_source_transfer(audit);
 for (std::size_t stage = 4U; stage < 15U; ++stage) {
  auto record = native_surface_record(native_surface_events[stage]);
  if (stage < 7U) record["surface_low"] = 1013U;
  record["workspace_source_low"] = 1013U;
  record["workspace_allocation"] = 2013U;
  record["trace_id"] = 32U;
  record["span_id"] = 32U;
  record["frame_revision"] = 24U;
  record["source_revision"] = 24U;
  if (stage >= 10U) {
   record["value"] = 2U;
   record["presentation_revision"] = 2U;
  }
  audit.native(record);
 }
 for (std::size_t stage = 6U; stage < browser_live_stages; ++stage) {
  auto record = browser_surface_record(browser_surface_events[stage]);
  const auto source = SurfaceAudit::native_identity(native_surface_record("", 1013U));
  if (stage < 9U) record["surface"] = source;
  record["source"] = source;
  record["workspace_allocation"] = 2013U;
  record["content_sequence"] = 24U;
  record["frame_revision"] = 24U;
  record["presentation_revision"] = 2U;
  audit.browser(record);
 }
 audit.browser(release_only_record("firefox.workspace.release_only_submitted"));
 audit.browser(release_only_record("firefox.workspace.release_only_completed"));
 REQUIRE(audit.failure.empty());
 REQUIRE(audit.evidence_settled());
 const auto& state = audit.surfaces.at(SurfaceAudit::native_identity(native_surface_record("")));
 CHECK(state.receipts.size() == 2U);
 CHECK(state.receipts.at(1U).release_only);
 CHECK_FALSE(state.receipts.at(2U).release_only);
 CHECK(state.transfers.at({1U, 1U}).released);
 CHECK(state.transfers.at({2U, 1U}).released);
 REQUIRE(state.samples.size() == 1U);
 CHECK(state.samples.at(2U) == 24U);
 REQUIRE(audit.draws.size() == 1U);
 CHECK(state.custody.at(2U).selected == 1U);
}
TEST_CASE("Release-only receipts require unique physical identity and completion", "[workspace][audit]") {
 const std::string fault =
  GENERATE("missing_submission", "missing_completion", "duplicate_submission", "duplicate_completion", "source", "publication", "transfer", "session", "frame", "ready", "release", "slot", "layer");
 CAPTURE(fault);
 SurfaceAudit audit;
 record_source_transfer(audit);
 auto submitted = release_only_record("firefox.workspace.release_only_submitted");
 auto completed = release_only_record("firefox.workspace.release_only_completed");
 if (fault == "source") completed["source"] = SurfaceAudit::native_identity(native_surface_record("", 13U));
 if (fault == "publication") completed["presentation_revision"] = 2U;
 if (fault == "transfer") completed["transfer_sequence"] = 2U;
 if (fault == "session") completed["content_session"] = 2U;
 if (fault == "frame") completed["content_sequence"] = 24U;
 if (fault == "ready") completed["timeline_ready"] = 3U;
 if (fault == "release") completed["timeline_release"] = 4U;
 if (fault == "slot" || fault == "layer") submitted[fault] = 0U;
 if (fault != "missing_submission") audit.browser(submitted);
 if (fault == "duplicate_submission") audit.browser(submitted);
 if (fault != "missing_completion") audit.browser(completed);
 if (fault == "duplicate_completion") audit.browser(completed);
 CHECK_FALSE(audit.evidence_settled());
}
TEST_CASE("Release-only terminal custody requires positive native completion or retained ownership", "[workspace][audit]") {
 const bool retained = GENERATE(false, true);
 SurfaceAudit audit;
 record_source_transfer(audit, false);
 audit.browser(release_only_record("firefox.workspace.release_only_submitted"));
 audit.native({{"event", "shutdown.requested"}});
 audit.native({{"event", "shutdown.firefox_terminal"}});
 REQUIRE(audit.failure.empty());
 CHECK_FALSE(audit.evidence_settled());
 audit.native(native_surface_record(retained ? "presentation.terminal_read.retained" : "presentation.terminal_read.completed"));
 if (!retained) audit.native(native_surface_record("presentation.retirement"));
 REQUIRE(audit.evidence_settled());
 CHECK(audit.joined_failure().empty());
 const auto& state = audit.surfaces.at(SurfaceAudit::native_identity(native_surface_record("")));
 CHECK(state.receipts.at(1U).stage == 2U);
 CHECK(state.transfers.at({1U, 1U}).terminal == (retained ? SurfaceAudit::TerminalRead::Retained : SurfaceAudit::TerminalRead::Completed));
}
TEST_CASE("Capacity retry preserves logical publication with a new physical source transfer", "[workspace][audit]") {
 SurfaceAudit audit;
 record_source_transfer(audit, false);
 // An unacquired offer is replaceable without a GPU release or sample copy.
 REQUIRE(audit.evidence_settled());
 for (std::size_t stage = 7U; stage < 15U; ++stage) {
  auto record = native_surface_record(native_surface_events[stage]);
  record["trace_id"] = 32U;
  record["span_id"] = 32U;
  if (stage >= 10U) {
   record["transfer_sequence"] = 2U;
   record["timeline_ready"] = 3U;
  }
  // The newer observation still selects the actual retained revision 23.
  record["source_observation_revision"] = 30U;
  audit.native(record);
 }
 for (std::size_t stage = 9U; stage < browser_live_stages; ++stage) {
  auto record = browser_surface_record(browser_surface_events[stage]);
  record["transfer_sequence"] = 2U;
  record["timeline_ready"] = 3U;
  record["timeline_release"] = 4U;
  audit.browser(record);
 }
 REQUIRE(audit.failure.empty());
 CHECK(audit.evidence_settled());
 audit.browser(browser_surface_record("firefox.workspace.copy_completed"));
 CHECK_FALSE(audit.failure.empty());
}
TEST_CASE("Unacquired offers reject invented physical settlement receipts", "[workspace][audit]") {
 for (const auto* event : {"firefox.workspace.frame_released", "firefox.workspace.copy_completed", "firefox.workspace.read_settled"}) {
  SurfaceAudit audit;
  record_source_transfer(audit, false);
  REQUIRE(audit.evidence_settled());
  audit.browser(browser_surface_record(event));
  CHECK_FALSE(audit.failure.empty());
 }
}
TEST_CASE("Detached unacquired offers retire without inventing physical read custody", "[workspace][audit]") {
 SurfaceAudit audit;
 record_source_transfer(audit, false);
 audit.browser(browser_surface_record("firefox.workspace.source.withdrawal"));
 REQUIRE(audit.failure.empty());
 CHECK(audit.evidence_settled());
 const auto& state = audit.surfaces.at(SurfaceAudit::native_identity(native_surface_record("")));
 CHECK(state.receipts.empty());
 CHECK_FALSE(state.transfers.begin()->second.releasing);
 CHECK(state.samples.empty());
}
TEST_CASE("Source admission audit requires canonical allocation provenance", "[workspace][audit]") {
 for (const auto* field : {"sequence", "capacity_width", "capacity_height", "workspace_source_high", "workspace_source_low", "workspace_allocation", "workspace_bytes", "workspace_pitch",
       "workspace_width", "workspace_height"}) {
  SurfaceAudit audit;
  auto record = native_surface_record("presentation.source.admission.enqueued");
  record.erase(field);
  audit.native(record);
  CHECK_FALSE(audit.failure.empty());
 }
 for (const auto* field : {"arena", "width", "height", "workspace_allocation"}) {
  SurfaceAudit audit;
  auto record = browser_surface_record("firefox.workspace.source.admitted");
  record.erase(field);
  audit.browser(record);
  CHECK_FALSE(audit.failure.empty());
 }
}
TEST_CASE("held placeholder motion evidence uses only the target hover interval", "[workspace][audit]") {
 for (const std::string_view timing : {"earlier", "during", "later"}) {
  BrowserAudit audit;
  const nlohmann::json motion{
   {"event", "integration.explore_mouse"}, {"detail", "shared-input-admitted"}, {"a", 150.0}, {"b", 50.0}, {"c", static_cast<std::uint64_t>(mmltk::controller::WorkspaceMouseKind::Motion)}};
  audit.consume(motion);
  auto draw = atlas_draw_record(77U, 10U, 4U, 0.0);
  draw["ready_slots"] = std::vector<bool>{true, false};
  for (const auto* event : {"iced.gallery.source", "iced.surface.sample_acquired", "iced.surface.draw_encoded"}) {
   draw["event"] = event;
   audit.consume(draw);
  }
  auto pixel = draw;
  pixel["event"] = "integration.atlas_ready_cell";
  pixel["compiled_index"] = 40U;
  pixel["sampled_pixels"] = 16U;
  pixel["cell_sample_x"] = 50000U;
  pixel["cell_sample_y"] = 50000U;
  pixel["cell_sample_rgba"] = 0xff705030U;
  pixel["matched"] = true;
  audit.consume(pixel);
  draw["event"] = "iced.surface.scroll_stage";
  draw["control"] = "held-visible";
  audit.consume(draw);
  REQUIRE(audit.atlas_draws.valid);
  REQUIRE(audit.held_visible_ordinal != 0U);
  if (timing == "during") audit.consume(motion);
  audit.consume({{"event", "integration.pending_hover"}, {"a", 41U}, {"b", 77U}});
  if (timing == "later") audit.consume(motion);
  CHECK(audit.held_placeholder_motion(41U, 77U) == (timing == "during"));
  CHECK_FALSE(audit.held_placeholder_motion(40U, 77U));
  CHECK_FALSE(audit.held_placeholder_motion(41U, 78U));
 }
}
TEST_CASE("fixed gallery return rejects any intermediate readiness or ready-pixel loss", "[workspace][audit]") {
 for (const std::string_view defect : {"none", "placeholder", "missing-cell", "black-cell", "wrong-cell", "origin", "capacity", "clip"}) {
  INFO(defect);
  AtlasDrawAudit audit;
  constexpr std::array names{"return-cached", "return-aligned", "return-extra", "return-restored"};
  for (std::size_t step = 0U; step < names.size(); ++step) {
   const auto rows = step == 2U ? 6U : 5U;
   auto draw = atlas_draw_record(300U + step, 0U, rows, step == 2U ? -75.0 : 0.0);
   draw["clip"][3] = 712.5;
   std::vector<std::uint64_t> indices(rows * 4U);
   std::iota(indices.begin(), indices.end(), 0U);
   draw["visible_indices"] = indices;
   draw["ready_slots"] = std::vector<bool>(indices.size(), true);
   if (step == 2U && defect == "placeholder") draw["ready_slots"][0] = false;
   auto source = draw;
   source["event"] = "iced.gallery.source";
   audit.consume(source);
   auto acquired = draw;
   acquired["event"] = "iced.surface.sample_acquired";
   audit.consume(acquired);
   if (step == 2U && defect == "origin") draw["row_origin"] = 8U;
   if (step == 2U && defect == "capacity") draw["content_height"] = 600U;
   if (step == 2U && defect == "clip") draw["clip"][3] = 750.0;
   audit.consume(draw);
   for (const auto index : indices) {
    if (step == 2U && defect == "missing-cell" && index == 0U) continue;
    auto pixel = draw;
    pixel["event"] = "integration.atlas_ready_cell";
    pixel["compiled_index"] = index;
    pixel["sampled_pixels"] = 16U;
    pixel["cell_sample_x"] = 50000U;
    pixel["cell_sample_y"] = 50000U;
    pixel["cell_sample_rgba"] = step == 2U && defect == "wrong-cell" && index == 0U ? 0xff806040U : 0xff705030U;
    pixel["overlay_boxes"] = true;
    pixel["overlay_masks"] = true;
    pixel["overlay_labels"] = true;
    pixel["matched"] = !(step == 2U && defect == "black-cell" && index == 0U);
    audit.consume(pixel);
   }
   draw["event"] = "iced.surface.scroll_stage";
   draw["control"] = names[step];
   audit.consume(draw);
  }
  CHECK((audit.valid && audit.return_round_trip) == (defect == "none"));
 }
}
TEST_CASE("physical ledger distinguishes abandonment and rejects obsolete image copies", "[workspace][audit]") {
 for (const std::string_view defect : {"none", "settlement", "early-release", "native-copy", "capture"}) {
  SurfaceAudit audit;
  for (const auto* event : native_surface_events) audit.native(native_surface_record(event));
  for (const auto* event : browser_surface_events) {
   if (std::string_view{event} == "iced.surface.draw_submitted") continue;
   if (std::string_view{event} == "iced.frame.draw_settled") {
    if (defect == "early-release") audit.browser(browser_surface_record("iced.frame.sample_released"));
    if (defect == "settlement") continue;
    event = "iced.frame.draw_abandoned";
   }
   audit.browser(browser_surface_record(event));
  }
  if (defect == "native-copy") audit.native(native_surface_record("presentation.copy.started"));
  if (defect == "capture") audit.browser(browser_surface_record("iced.surface.capture_encoded"));
  CHECK(audit.joined_failure().empty() == (defect == "none"));
 }
}
TEST_CASE("read admission checks actual eligible demand and independently clipped neighbor windows", "[workspace][audit]") {
 for (const std::string_view defect : {"none", "immediate", "preferred", "outside"}) {
  NativeAudit audit;
  nlohmann::json admission{{"kind", "gui_runtime"}, {"event", "gallery.read.scheduled"}, {"sequence", 7U}, {"detail", 20U}, {"admission_position", 20U}, {"admission_columns", 4U},
   {"admission_first_row", 6U}, {"admission_row_count", 5U}, {"admission_forward", true}, {"admission_tier", 2U}, {"admission_immediate_eligible", 0U}, {"admission_forward_eligible", 0U},
   {"admission_backward_eligible", 7U}};
  if (defect == "immediate") admission["admission_immediate_eligible"] = 1U;
  if (defect == "preferred") admission["admission_forward_eligible"] = 1U;
  if (defect == "outside") admission["admission_position"] = 0U;
  audit.consume(admission);
  CHECK(audit.admission_seen);
  CHECK(audit.admission_priority_valid == (defect == "none"));
 }
}
TEST_CASE("cached viewport evidence joins initial readiness before new read admission", "[workspace][audit]") {
 for (const bool forward : {false, true}) {
  for (const bool restored_first : {false, true}) {
   NativeAudit audit;
   audit.consume({{"kind", "gui_runtime"}, {"owner", "explore"}, {"event", "acceptance.placeholder.slot"}, {"sequence", 7U}, {"value", 0U}, {"detail", 20U}, {"capacity_width", 1U}});
   audit.consume({{"kind", "gui_runtime"}, {"owner", "explore"}, {"event", "acceptance.placeholder.slot"}, {"sequence", 7U}, {"value", 1U}, {"detail", 21U}, {"capacity_width", 0U}});
   const nlohmann::json restored{{"kind", "gui_runtime"}, {"owner", "explore"}, {"event", "acceptance.placeholder.complete"}, {"sequence", 7U}, {"value", 2U}, {"capacity_width", 1U},
    {"admission_columns", 2U}, {"admission_first_row", 10U}, {"admission_row_count", 1U}, {"admission_forward", forward}};
   const nlohmann::json admission{{"kind", "gui_runtime"}, {"owner", "explore"}, {"event", "gallery.read.scheduled"}, {"sequence", 7U}, {"detail", 21U}, {"admission_position", 21U},
    {"admission_columns", 2U}, {"admission_first_row", 10U}, {"admission_row_count", 1U}, {"admission_forward", forward}, {"admission_tier", 0U}};
   audit.consume(restored_first ? restored : admission);
   audit.consume(restored_first ? admission : restored);
   CHECK(audit.cached_first_valid == restored_first);
   CHECK(audit.initial_cache.at(7U).slots == std::set<std::uint64_t>{0U});
   CHECK(audit.initial_cache.at(7U).forward == forward);
   CHECK(audit.admission_priority_valid);
  }
 }
}
}  // namespace mmltk::acceptance::wayland

namespace mmltk::acceptance::wayland {
TEST_CASE("packaged compilation exposes three laid-out track facts including known-zero acquisition", "[workspace][audit]") {
 BrowserAudit audit;
 for (const auto& [name, detail] : std::array<std::pair<const char*, const char*>, 3>{{
  {"Acquisition", "Acquisition · unnecessary · 0 / 0"},
  {"Labels/masks", "Labels/masks · active · 3 / 8"},
  {"Pixels", "Pixels · active · 2 / 8"}}}) {
  audit.consume({{"event", "integration.compile_track_text"}, {"control", name}, {"detail", detail}, {"a", 1}, {"b", 2}, {"c", 100}, {"d", 16}});
 }
 CHECK(audit.compile_tracks.size() == 3);
}
}  // namespace mmltk::acceptance::wayland
