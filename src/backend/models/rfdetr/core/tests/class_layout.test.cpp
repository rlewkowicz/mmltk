#include "src/backend/ml/torch/tests/catch_support.h"
#include <array>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include <catch2/catch_test_macros.hpp>
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/keypoint_layout.h"
#include "src/backend/models/rfdetr/core/model_info.h"
#include <catch2/generators/catch_generators.hpp>
namespace r = mmltk::backend::models::rfdetr;
namespace catalog = mmltk::backend::data::catalog;
namespace {
r::ModelInfo task_output_info(r::ModelTask task) {
 using Role = r::RfdetrOutputRole;
 r::ModelInfo info;
 info.outputs = {{"pred_logits", {2, 4, 3}, "float32", Role::Logits}, {"pred_boxes", {2, 4, 4}, "float32", Role::Boxes}};
 info.output_contract = r::RfdetrOutputContract{1U, task, {}};
 if (task == r::ModelTask::Segmentation) info.outputs.push_back({"pred_masks", {2, 4, 12, 16}, "float32", Role::Masks});
 if (task == r::ModelTask::Keypoints) {
  info.outputs.push_back({"pred_keypoints", {2, 4, 2, 8}, "float32", Role::Keypoints});
  info.output_contract->keypoints_per_class = {2};
 }
 return info;
}
}  // namespace
TEST_CASE("Native sigmoid layout preserves foreground zero and reserved output", "[rfdetr][layout]") {
 const r::ResolvedClassLayout layout(r::native_training_class_layout(catalog::ClassCatalog({"background", "person", "car"})));
 CHECK(layout.semantic());
 CHECK(layout.output_width() == 4U);
 CHECK(layout.eligible_count() == 3U);
 CHECK(layout.physical_references()[0] == 0);
 CHECK(layout.physical_references()[3] == -1);
 CHECK(layout.record().slots[3].role == r::ClassSlotRole::Unused);
 CHECK(layout.record().no_object == r::NoObjectEncoding::AllNegative);
 CHECK(r::decode_class_layout(r::encode_class_layout(layout.record())) == layout.record());
 std::vector<std::string> names;
 for (unsigned index = 0; index < 256; ++index) names.push_back("class-" + std::to_string(index));
 const r::ResolvedClassLayout full(r::native_training_class_layout(catalog::ClassCatalog(std::move(names))));
 CHECK(full.output_width() == 257U);
 CHECK(full.physical_references()[255] == 255);
}
TEST_CASE("Declared background can occupy each physical position", "[rfdetr][layout]") {
 for (unsigned background = 0; background < 3; ++background) {
  auto record = r::native_training_class_layout(catalog::ClassCatalog({"cat", "dog"}));
  record.no_object = r::NoObjectEncoding::ExplicitBackground;
  unsigned foreground = 0;
  for (unsigned slot = 0; slot < 3; ++slot)
   record.slots[slot] = slot == background ? r::ModelClassSlot{r::ClassSlotRole::Background, std::nullopt} : r::ModelClassSlot{r::ClassSlotRole::Foreground, foreground++};
  const r::ResolvedClassLayout layout(record);
  CHECK(layout.eligible_count() == 2);
  CHECK(layout.physical_references()[background] == -1);
  foreground = 0;
  for (unsigned slot = 0; slot < 3; ++slot)
   if (slot != background) {
    CHECK(layout.physical_references()[slot] == foreground);
    ++foreground;
   }
 }
 const r::ResolvedClassLayout empty(r::native_training_class_layout(catalog::ClassCatalog{}));
 CHECK(empty.eligible_count() == 0);
 REQUIRE(empty.physical_references().size() == 1);
 CHECK(empty.physical_references()[0] == -1);
}
TEST_CASE("Unbound external identity stays raw and COCO last slot stays foreground", "[rfdetr][layout]") {
 const r::ResolvedClassLayout raw(r::unresolved_class_layout(91));
 CHECK_FALSE(raw.semantic());
 CHECK(raw.domain() == catalog::ClassReferenceDomain::RawOutputSlot);
 CHECK(raw.physical_references()[90] == 90);
 CHECK_THROWS(raw.require_execution(true));
 const r::ResolvedClassLayout coco(r::coco_class_layout({r::ClassLayoutOrigin::VerifiedAsset, "fixture", {}}));
 CHECK(raw.eligible_count() == 91);
 CHECK(coco.eligible_count() == 80);
 REQUIRE(coco.physical_references().size() == 91);
 CHECK(coco.physical_references()[0] == -1);
 CHECK(coco.physical_references()[1] == 0);
 CHECK(coco.physical_references()[12] == -1);
 CHECK(coco.physical_references()[13] == 11);
 CHECK(coco.physical_references()[90] == 79);
}
TEST_CASE("Class layout rejects ambiguous roles and unsupported execution", "[rfdetr][layout]") {
 auto record = r::native_training_class_layout(catalog::ClassCatalog({"cat", "dog"}));
 record.slots[1].foreground_index = 0;
 CHECK_THROWS(r::ResolvedClassLayout(record));
 record.slots[1].foreground_index = 2;
 CHECK_THROWS(r::ResolvedClassLayout(record));
 record = r::native_training_class_layout(catalog::ClassCatalog({"cat"}));
 record.slots[1].role = r::ClassSlotRole::Background;
 CHECK_THROWS(r::ResolvedClassLayout(record));
 record.no_object = r::NoObjectEncoding::ExplicitBackground;
 record.scores = r::ClassScoreEncoding::SoftmaxLogits;
 CHECK_THROWS(r::ResolvedClassLayout(record).require_execution());
 record.version = 99;
 CHECK_THROWS(r::ResolvedClassLayout(record));
 CHECK_THROWS(r::decode_class_layout("{}"));
 CHECK_THROWS(r::decode_class_layout(std::string(r::kClassLayoutByteBudget + 1, ' ')));
}
TEST_CASE("Output names distinguish four-wide logits from boxes", "[rfdetr][layout]") {
 r::ModelInfo info;
 info.outputs = {{"pred_boxes", {1, 300, 4}, "float32"}, {"pred_logits", {1, 300, 4}, "float32"}};
 const auto roles = r::validate_rfdetr_output_layout(info);
 CHECK(roles.index<r::RfdetrOutputRole::Logits>() == 1);
 CHECK(roles.index<r::RfdetrOutputRole::Boxes>() == 0);
 CHECK(info.num_classes == 4);
 info.outputs[0].name = "some_box_tensor";
 for (auto& output : info.outputs) output.role = r::RfdetrOutputRole::Unspecified;
 CHECK_THROWS(r::validate_rfdetr_output_layout(info));
 info.outputs[0].role = r::RfdetrOutputRole::Boxes;
 CHECK_NOTHROW(r::validate_rfdetr_output_layout(info));
 info.outputs[0].shape[1] = 299;
 CHECK_THROWS(r::validate_rfdetr_output_layout(info));
}
TEST_CASE("Admitted preset candidate defaults agree across all model tasks", "[model][rfdetr][layout]") {
 namespace r = mmltk::backend::models::rfdetr;
 const std::array expected{300, 300, 300, 300, 100, 100, 200, 200, 300, 300, 300, 200, 200, 300, 300, 100};
 REQUIRE(r::kPresetCatalog.size() == expected.size());
 for (std::size_t index = 0; index < expected.size(); ++index) {
  CHECK(r::kPresetCatalog[index].selected_query_count == expected[index]);
  CHECK(r::native_config_from_preset(r::kPresetCatalog[index]).num_select == expected[index]);
 }
}
TEST_CASE("Class descriptors admit every canonical named output role", "[model][rfdetr][layout]") {
 r::ModelClassDescriptor descriptor;
 descriptor.artifact_sha256 = std::string(64U, 'a');
 descriptor.layout = r::native_training_class_layout(catalog::ClassCatalog({"person"}));
 for (const auto& entry : mmltk::frameworks::reflection::enum_entries<r::RfdetrOutputRole>()) {
  if (entry.value != r::RfdetrOutputRole::Unspecified) descriptor.output_roles.push_back({std::string(entry.name), entry.value});
 }
 CHECK(descriptor.output_roles.size() == r::kMaximumNamedOutputRoles);
 CHECK(r::decode_class_descriptor(r::encode_class_descriptor(descriptor)).output_roles == descriptor.output_roles);
 descriptor.output_roles.back().role = descriptor.output_roles.front().role;
 CHECK_THROWS(r::encode_class_descriptor(descriptor));
}
TEST_CASE("Artifact output roles preserve persisted identity and all task orderings", "[model][rfdetr][layout]") {
 using Role = r::RfdetrOutputRole;
 constexpr std::array spellings{"Unspecified", "Logits", "Boxes", "Masks", "Keypoints", "KeypointScores", "KeypointPrecision"};
 for (std::size_t index = 0; index < spellings.size(); ++index)
  CHECK(mmltk::frameworks::reflection::enum_name(static_cast<Role>(index)) == spellings[index]);
 const auto task = GENERATE(r::ModelTask::Detection, r::ModelTask::Segmentation, r::ModelTask::Keypoints);
 const bool renamed = GENERATE(false, true);
 const auto dtype = GENERATE("float32", "float16", "bfloat16", "float64");
 auto original = task_output_info(task);
 for (auto& output : original.outputs) output.dtype = dtype;
 original.outputs[0].shape[0] = -1;
 std::array<std::size_t, 3> order{0, 1, 2};
 do {
  auto info = original;
  for (std::size_t index = 0; index < info.outputs.size(); ++index) {
   info.outputs[index] = original.outputs[order[index]];
   if (renamed) info.outputs[index].name = "producer_" + info.outputs[index].name;
   else info.outputs[index].role = Role::Unspecified;
  }
  const auto roles = r::validate_rfdetr_output_layout(info);
  CHECK(roles.index<Role::Logits>() == static_cast<std::size_t>(std::ranges::find(order, 0) - order.begin()));
  CHECK(roles.index<Role::Boxes>() == static_cast<std::size_t>(std::ranges::find(order, 1) - order.begin()));
  const auto task_index = static_cast<std::size_t>(std::ranges::find(order, 2) - order.begin());
  CHECK(roles.index<Role::Masks>() == (task == r::ModelTask::Segmentation ? std::optional{task_index} : std::nullopt));
  CHECK(roles.index<Role::Keypoints>() == (task == r::ModelTask::Keypoints ? std::optional{task_index} : std::nullopt));
  CHECK(info.num_queries == 4);
  CHECK(info.num_classes == 3);
  CHECK(info.has_masks == (task == r::ModelTask::Segmentation));
  CHECK(info.output_contract == original.output_contract);
  if (task != r::ModelTask::Keypoints) {
   info.output_contract.reset();
   CHECK_NOTHROW(r::validate_rfdetr_output_layout(info));
   CHECK(info.output_contract == original.output_contract);
  }
 } while (std::next_permutation(order.begin(), order.begin() + original.outputs.size()));
}
TEST_CASE("Artifact task roles reject contradictory incomplete and unsupported outputs", "[model][rfdetr][layout]") {
 using Role = r::RfdetrOutputRole;
 const auto task = GENERATE(r::ModelTask::Detection, r::ModelTask::Segmentation, r::ModelTask::Keypoints);
 auto info = task_output_info(task);
 REQUIRE_NOTHROW(r::validate_rfdetr_output_layout(info));
 SECTION("missing logits") { info.outputs.erase(info.outputs.begin()); }
 SECTION("missing boxes") { info.outputs.erase(info.outputs.begin() + 1); }
 SECTION("missing task output or unexpected task") {
  if (task == r::ModelTask::Detection) info.output_contract->task = r::ModelTask::Segmentation;
  else info.outputs.pop_back();
 }
 SECTION("extra task tensor") {
  if (task == r::ModelTask::Segmentation) info.outputs.push_back({"pred_keypoints", {2, 4, 2, 8}, "float32"});
  else info.outputs.push_back({"pred_masks", {2, 4, 12, 16}, "float32"});
 }
 SECTION("canonical name conflicts with explicit role") { info.outputs[0].role = Role::Boxes; }
 SECTION("renamed duplicate role") { info.outputs.push_back({"producer_scores", {2, 4, 3}, "float32", Role::Logits}); }
 SECTION("unsupported descriptor role") {
  info.outputs.push_back({"producer_optional", {2, 4, 2}, "float32", GENERATE(Role::KeypointScores, Role::KeypointPrecision, static_cast<Role>(255))});
 }
 SECTION("unresolved producer name") { info.outputs[0].name = "producer_scores"; info.outputs[0].role = Role::Unspecified; }
 SECTION("rank") { info.outputs[0].shape.push_back(1); }
 SECTION("zero batch") { info.outputs[0].shape[0] = 0; }
 SECTION("invalid dynamic batch") { info.outputs[0].shape[0] = -2; }
 SECTION("batch disagreement") { info.outputs[1].shape[0] = 3; }
 SECTION("query disagreement") { info.outputs[1].shape[1] = 3; }
 SECTION("empty query axis") { info.outputs[0].shape[1] = 0; }
 SECTION("empty class axis") { info.outputs[0].shape[2] = 0; }
 SECTION("empty terminal extent") { info.outputs.back().shape.back() = 0; }
 SECTION("box width") { info.outputs[1].shape[2] = 5; }
 SECTION("dtype disagreement") { info.outputs[1].dtype = "float16"; }
 SECTION("nonfloating outputs") { for (auto& output : info.outputs) output.dtype = "int32"; }
 CHECK_THROWS_AS(r::validate_rfdetr_output_layout(info), std::invalid_argument);
}

TEST_CASE("Model schema admission bounds escaped JSON and validates optional raw evidence", "[rfdetr][layout][keypoints]") {
 catalog::ClassKeypointSchema joints;
 for (unsigned index = 0; index < 128; ++index) {
  joints.names.push_back(std::to_string(index) + std::string(190, '\x01'));
  joints.horizontal_flip.push_back(index);
  joints.oks_sigmas.push_back(.05);
 }
 catalog::OrderedClassCatalog catalog_record;
 for (unsigned index = 0; index < 6; ++index) catalog_record.names.push_back({"class-" + std::to_string(index)});
 catalog_record.keypoints.emplace(catalog_record.names.size(), joints);
 auto record = r::native_training_class_layout(catalog::ClassCatalog(catalog_record));
 const auto encoded = r::encode_class_layout(record);
 REQUIRE(encoded.size() < r::kClassLayoutByteBudget);
 CHECK(r::decode_class_layout(encoded) == record);
 r::ModelClassDescriptor descriptor;
 descriptor.artifact_sha256 = std::string(64, 'a');
 descriptor.layout = record;
 CHECK(r::decode_class_descriptor(r::encode_class_descriptor(descriptor)).layout == record);
 for (unsigned index = 6; index < 8; ++index) {
  catalog_record.names.push_back({"class-" + std::to_string(index)});
  catalog_record.keypoints->push_back(joints);
 }
 // Independent data catalogs admit this schema; model publication has a lower
 // byte budget, including JSON escaping of the joint-name control characters.
 const catalog::ClassCatalog large(catalog_record);
 CHECK_THROWS(r::ResolvedClassLayout(r::native_training_class_layout(large)));
 auto raw = r::unresolved_class_layout(2);
 raw.class_name_evidence.names = {{""}, {""}};
 raw.class_name_evidence.keypoints = std::vector{catalog::coco_person_keypoint_schema(), catalog::coco_person_keypoint_schema()};
 CHECK_FALSE(r::ResolvedClassLayout(raw).semantic());
 CHECK(r::decode_class_layout(r::encode_class_layout(raw)) == raw);
 raw.class_name_evidence.keypoints->pop_back();
 CHECK_THROWS(r::ResolvedClassLayout(raw));
 raw.class_name_evidence.keypoints->push_back(catalog::coco_person_keypoint_schema());
 raw.class_name_evidence.keypoints->front().horizontal_flip[0] = 255;
 CHECK_THROWS(r::encode_class_layout(raw));
}

TEST_CASE("Joint configurations preserve independent foreground and physical slot capacities", "[rfdetr][layout][keypoints]") {
 auto config = r::native_config_from_preset(*r::find_preset_catalog_entry("rf-detr-keypoint-preview"));
 config.num_classes = r::kMaximumClassOutputSlots;
 config.keypoints_per_class.assign(r::kMaximumClassOutputSlots, 0);
 config.keypoints_per_class.back() = 17;
 CHECK(r::native_model_config_valid(config));
 const r::KeypointTensorLayout sparse(config.keypoints_per_class);
 CHECK(sparse.slot_count() == 4096);
 CHECK(sparse.compact_count() == 17);
 CHECK(sparse.maximum_joints() == 17);
 CHECK(sparse.compact_offset(4095) == 0);
 CHECK(sparse.padded_count() == 4096 * 17);
 CHECK(std::ranges::equal(r::KeypointTensorLayout::from_active_mask(sparse.active_mask()).counts(), config.keypoints_per_class));
 for (std::size_t index = 0; index < catalog::kClassCatalogCapacity; ++index) config.keypoints_per_class[index] = 1;
 CHECK_FALSE(r::native_model_config_valid(config));
 CHECK_THROWS(r::KeypointTensorLayout(config.keypoints_per_class));
 CHECK_THROWS(r::KeypointTensorLayout(std::vector<std::uint16_t>(r::kMaximumClassOutputSlots + 1, 0)));
 CHECK_THROWS(r::KeypointTensorLayout(std::array<std::uint16_t, 1>{257}));
 const r::KeypointTensorLayout full_joints(std::array<std::uint16_t, 1>{256});
 CHECK(full_joints.compact_count() == 256);
 CHECK(full_joints.maximum_joints() == 256);
}

TEST_CASE("Joint Transfer projects catalog correspondence through physical slots in destination order", "[rfdetr][layout][keypoints]") {
 const catalog::ClassKeypointSchema pair{{"left", "right"}, {{0, 1}}, {1, 0}, {.1, .1}};
 const catalog::ClassKeypointSchema pin{{"tip"}, {}, {0}, {.2}};
 catalog::OrderedClassCatalog source_record;
 source_record.names = {{"paired"}, {"pin"}, {"empty"}};
 source_record.keypoints = std::vector{pair, pin, catalog::ClassKeypointSchema{}};
 const r::ResolvedClassLayout active_first(r::native_training_class_layout(catalog::ClassCatalog(source_record)));
 // The reserved fourth classifier slot is omitted from the joint schema.
 const r::KeypointTensorLayout source_joints(std::array<std::uint16_t, 3>{2, 1, 0});
 CHECK_NOTHROW(source_joints.require_class_layout(active_first));
 CHECK(source_joints.padded_count() == 6);
 auto destination_record = source_record;
 destination_record.names = {{"pin"}, {"empty"}, {"paired"}};
 destination_record.keypoints = std::vector{pin, catalog::ClassKeypointSchema{}, catalog::permute_keypoint_schema(pair, std::array<catalog::JointIndex, 2>{1, 0})};
 auto background_record = r::native_training_class_layout(catalog::ClassCatalog(destination_record));
 background_record.no_object = r::NoObjectEncoding::ExplicitBackground;
 background_record.slots = {{r::ClassSlotRole::Background, std::nullopt}, {r::ClassSlotRole::Foreground, 0}, {r::ClassSlotRole::Foreground, 1}, {r::ClassSlotRole::Foreground, 2}};
 const r::ResolvedClassLayout background_first(background_record);
 const r::KeypointTensorLayout destination_joints(std::array<std::uint16_t, 4>{0, 1, 0, 2});
 CHECK_NOTHROW(destination_joints.require_class_layout(background_first));
 using Rows = std::vector<std::pair<std::int64_t, std::int64_t>>;
 CHECK(r::keypoint_transfer_rows(active_first, source_joints, background_first, destination_joints) == Rows{{2, 0}, {1, 1}, {0, 2}});
 CHECK(r::keypoint_transfer_rows(background_first, destination_joints, active_first, source_joints) == Rows{{2, 0}, {1, 1}, {0, 2}});
 CHECK_THROWS(source_joints.require_class_layout(background_first));
 CHECK_THROWS(destination_joints.require_class_layout(active_first));
 CHECK_THROWS(r::KeypointTensorLayout(std::array<std::uint16_t, 2>{0, 1}).require_class_layout(background_first));
 auto sparse_record = background_record;
 sparse_record.slots.assign(r::kMaximumClassOutputSlots, {r::ClassSlotRole::Unused, std::nullopt});
 sparse_record.slots[0] = {r::ClassSlotRole::Background, std::nullopt};
 sparse_record.slots[7] = {r::ClassSlotRole::Foreground, 0};
 sparse_record.slots[2048] = {r::ClassSlotRole::Foreground, 1};
 sparse_record.slots.back() = {r::ClassSlotRole::Foreground, 2};
 const r::ResolvedClassLayout sparse(sparse_record);
 std::vector<std::uint16_t> sparse_counts(r::kMaximumClassOutputSlots, 0);
 sparse_counts[7] = 1;
 sparse_counts.back() = 2;
 const r::KeypointTensorLayout sparse_joints(sparse_counts);
 CHECK_NOTHROW(sparse_joints.require_class_layout(sparse));
 CHECK(sparse_joints.compact_offset(4095) == 1);
 CHECK(sparse_joints.padded_offset(4095) == 8190);
 CHECK(r::keypoint_transfer_rows(active_first, source_joints, sparse, sparse_joints) == Rows{{2, 0}, {1, 1}, {0, 2}});
 CHECK_THROWS(r::keypoint_transfer_rows(r::ResolvedClassLayout(r::unresolved_class_layout(4)), source_joints, sparse, sparse_joints));
 auto disjoint_record = source_record;
 disjoint_record.keypoints = std::vector{catalog::ClassKeypointSchema{{"new_left", "new_right"}, {{0, 1}}, {1, 0}, {.1, .1}}, catalog::ClassKeypointSchema{{"new_tip"}, {}, {0}, {.2}}, catalog::ClassKeypointSchema{}};
 const r::ResolvedClassLayout disjoint(r::native_training_class_layout(catalog::ClassCatalog(disjoint_record)));
 CHECK(r::keypoint_transfer_rows(active_first, source_joints, disjoint, source_joints).empty());
}

TEST_CASE("Keypoint artifact admission requires a typed task and exact physical joint schema", "[model][rfdetr][layout][keypoints]") {
 catalog::OrderedClassCatalog names;
 names.names = {{"marker"}, {"plain"}};
 names.keypoints = std::vector{catalog::ClassKeypointSchema{{"left", "right"}, {{0,1}}, {1,0}, {.1,.1}}, catalog::ClassKeypointSchema{}};
 r::ModelInfo info;
 info.class_layout = r::native_training_class_layout(catalog::ClassCatalog(names));
 info.outputs = {{"pred_keypoints", {1, 4, 2, 8}, "float32"}, {"pred_boxes", {1, 4, 4}, "float32"}, {"pred_logits", {1, 4, 3}, "float32"}};
 CHECK_THROWS(r::validate_rfdetr_output_layout(info));
 // Omitted trailing classifier slots have zero joint contribution.
 info.output_contract = r::RfdetrOutputContract{1U, r::ModelTask::Keypoints, {2}};
 const auto roles = r::validate_rfdetr_output_layout(info);
 CHECK(roles.index<r::RfdetrOutputRole::Keypoints>() == 0U);
 CHECK(roles.index<r::RfdetrOutputRole::Logits>() == 2U);
 CHECK_FALSE(roles.index<r::RfdetrOutputRole::Masks>());
 CHECK(r::decode_output_contract(r::encode_output_contract(*info.output_contract)) == info.output_contract);
 const auto admitted = info;
 SECTION("wrong task") { info.output_contract->task = r::ModelTask::Detection; }
 SECTION("wrong channel width") { info.outputs[0].shape[3] = 7; }
 SECTION("wrong padded joints") { info.outputs[0].shape[2] = 3; }
 SECTION("wrong physical schema") { info.output_contract->keypoints_per_class = {0, 2}; }
 SECTION("unnamed schema") { info.class_layout = r::unresolved_class_layout(3); }
 SECTION("invalid dtype") { info.outputs[0].dtype = "int32"; }
 SECTION("duplicate tensor role") { info.outputs[0].role = r::RfdetrOutputRole::Boxes; }
 SECTION("unsupported contract version") { info.output_contract->version = 2; }
 CHECK_THROWS(r::validate_rfdetr_output_layout(info));
 auto legacy = admitted;
 legacy.outputs.erase(legacy.outputs.begin());
 legacy.output_contract.reset();
 CHECK_NOTHROW(r::validate_rfdetr_output_layout(legacy));
 CHECK(legacy.output_contract->task == r::ModelTask::Detection);
 legacy.outputs.push_back({"pred_masks", {1,4,12,16}, "float32"});
 legacy.output_contract.reset();
 CHECK_NOTHROW(r::validate_rfdetr_output_layout(legacy));
 CHECK(legacy.output_contract->task == r::ModelTask::Segmentation);
}
