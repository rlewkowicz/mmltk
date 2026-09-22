#include <array>
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include <catch2/catch_test_macros.hpp>
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/model_info.h"
namespace r = mmltk::backend::models::rfdetr;
namespace catalog = mmltk::backend::data::catalog;
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
 CHECK(roles.logits == 1);
 CHECK(roles.boxes == 0);
 CHECK(info.num_classes == 4);
 info.outputs[0].name = "some_box_tensor";
 for (auto& output : info.outputs) output.role = r::RfdetrOutputRole::Unspecified;
 CHECK_THROWS(r::validate_rfdetr_output_layout(info));
 info.outputs[0].role = r::RfdetrOutputRole::Boxes;
 CHECK_NOTHROW(r::validate_rfdetr_output_layout(info));
 info.outputs[0].shape[1] = 299;
 CHECK_THROWS(r::validate_rfdetr_output_layout(info));
}
TEST_CASE("Admitted preset candidate defaults agree across detection and segmentation", "[model][rfdetr][layout]") {
 namespace r = mmltk::backend::models::rfdetr;
 const std::array expected{300, 300, 300, 300, 100, 100, 200, 200, 300, 300};
 REQUIRE(r::kPresetCatalog.size() == expected.size());
 for (std::size_t index = 0; index < expected.size(); ++index) {
  CHECK(r::kPresetCatalog[index].selected_query_count == expected[index]);
  CHECK(r::native_config_from_preset(r::kPresetCatalog[index]).num_select == expected[index]);
 }
}
