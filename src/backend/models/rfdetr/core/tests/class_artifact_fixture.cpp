#include "class_artifact_fixture.h"
#include <onnx/onnx_pb.h>
#include <algorithm>
#include <array>
#include <span>
#include <vector>
namespace mmltk::backend::models::rfdetr::test_support {
namespace rfdetr = mmltk::backend::models::rfdetr;
void write_prediction_model(const std::filesystem::path& path, std::int64_t queries, bool include_masks, std::optional<ModelClassLayout> layout) {
 namespace onnx = mmltk_onnx;
 onnx::ModelProto model;
 model.set_ir_version(8);
 model.add_opset_import()->set_version(13);
 if (layout) {
  auto* metadata = model.add_metadata_props();
  metadata->set_key("mmltk.rfdetr.class_layout");
  metadata->set_value(rfdetr::encode_class_layout(*layout));
 }
 auto* graph = model.mutable_graph();
 graph->set_name("prediction-delivery");
 const auto value = [](onnx::ValueInfoProto* destination, const std::string& name, std::span<const std::int64_t> dimensions) {
  destination->set_name(name);
  auto* type = destination->mutable_type()->mutable_tensor_type();
  type->set_elem_type(onnx::TensorProto::FLOAT);
  for (auto extent : dimensions) type->mutable_shape()->add_dim()->set_dim_value(extent);
 };
 value(graph->add_input(), "images", std::array<std::int64_t, 4>{1, 3, 8, 8});
 auto* mean = graph->add_node();
 mean->set_op_type("ReduceMean");
 mean->add_input("images");
 mean->add_output("mean");
 auto* keepdims = mean->add_attribute();
 keepdims->set_name("keepdims");
 keepdims->set_type(onnx::AttributeProto::INT);
 keepdims->set_i(0);
 auto* zero = graph->add_initializer();
 zero->set_name("zero");
 zero->set_data_type(onnx::TensorProto::FLOAT);
 zero->add_float_data(0.0F);
 auto* multiply = graph->add_node();
 multiply->set_op_type("Mul");
 multiply->add_input("mean");
 multiply->add_input("zero");
 multiply->add_output("offset");
 const auto output = [&](const std::string& name, std::span<const std::int64_t> dimensions, std::span<const float> values) {
  value(graph->add_output(), name, dimensions);
  auto* constants = graph->add_initializer();
  constants->set_name(name + "_values");
  constants->set_data_type(onnx::TensorProto::FLOAT);
  for (auto extent : dimensions) constants->add_dims(extent);
  for (auto scalar : values) constants->add_float_data(scalar);
  auto* add = graph->add_node();
  add->set_op_type("Add");
  add->add_input(constants->name());
  add->add_input("offset");
  add->add_output(name);
 };
 std::vector<float> logits(queries * 2, -10.F), boxes(queries * 4, .5F), masks(queries * 4, -1.F);
 logits[0] = 10.F;
 logits[3] = 9.F;
 boxes[2] = boxes[3] = 1.F;
 std::fill_n(masks.begin(), 4, 1.F);
 output("pred_logits", std::array<std::int64_t, 3>{1, queries, 2}, logits);
 output("pred_boxes", std::array<std::int64_t, 3>{1, queries, 4}, boxes);
 if (include_masks) output("pred_masks", std::array<std::int64_t, 4>{1, queries, 2, 2}, masks);
 std::ofstream file(path, std::ios::binary);
 REQUIRE(model.SerializeToOstream(&file));
}
}
