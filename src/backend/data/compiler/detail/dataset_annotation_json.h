#pragma once
#include "src/backend/data/detail/compile_json_number.h"
#include "src/common/types/workspace_buffer.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <simdjson.h>
#include <stdexcept>
#include <string_view>
#include <variant>
namespace mmltk::backend::data::compiler_internal {
struct DatasetAnnotationScalar {
 std::optional<std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string_view>> value;
 template <class T>
 const T* get() const {
  return value ? std::get_if<T>(&*value) : nullptr;
 }
 [[nodiscard]] bool is_bool() const { return get<bool>(); }
 [[nodiscard]] bool is_int64() const { return get<std::int64_t>(); }
 [[nodiscard]] bool is_uint64() const { return get<std::uint64_t>(); }
};
struct DatasetAnnotationValue : DatasetAnnotationScalar {
 std::optional<std::size_t> array_size;
 // Boxes retain a fixed prefix; keypoints use a supplied bounded complete array.
 std::array<DatasetAnnotationScalar, 4> coordinates;
 std::span<DatasetAnnotationScalar> sequence;
};
struct DatasetAnnotationJson final {
 std::optional<DatasetAnnotationValue> class_name, bbox_xyxy, mask_rle, mask_rle_encoding, image_size_wh, area, iscrowd, ignore, id, image_id, category_id, keypoints;
};
MMLTK_REFLECT_FIELDS(DatasetAnnotationJson)
struct DatasetAnnotationFrame {
 simdjson::ondemand::object_iterator object;
 simdjson::ondemand::array_iterator array;
 DatasetAnnotationValue* coordinates = nullptr;
 bool is_object = false, child_consumed = false;
};
inline void consume_dataset_annotation_value(simdjson::ondemand::value source, common::types::WorkspaceBuffer<DatasetAnnotationFrame>& stack, DatasetAnnotationValue* target = nullptr) {
 stack.clear();
 DatasetAnnotationScalar* scalar = target;
 DatasetAnnotationValue* array_target = target;
 for (;;) {
  switch (source.type().value()) {
   case simdjson::ondemand::json_type::object: {
    DatasetAnnotationFrame frame;
    frame.is_object = true;
    frame.object = source.get_object().value().begin().value();
    stack.push_back(frame);
    break;
   }
   case simdjson::ondemand::json_type::array: {
    DatasetAnnotationFrame frame;
    frame.array = source.get_array().value().begin().value();
    frame.coordinates = array_target;
    if (array_target) array_target->array_size = 0;
    stack.push_back(frame);
    break;
   }
   case simdjson::ondemand::json_type::string: {
    const auto text = source.get_string().value();
    if (scalar) scalar->value = text;
    break;
   }
   case simdjson::ondemand::json_type::number:
    detail::consume_compile_json_number(source, [&](auto number) {
     if (scalar) scalar->value = number;
    }, [](const char* error) { throw std::runtime_error(error); });
    break;
   case simdjson::ondemand::json_type::boolean: {
    const auto boolean = source.get_bool().value();
    if (scalar) scalar->value = boolean;
    break;
   }
   case simdjson::ondemand::json_type::null:
    if (!source.is_null().value()) throw std::runtime_error("invalid JSON value");
    if (scalar) scalar->value = nullptr;
    break;
  }
  // The supplied stack validates arbitrarily nested unknown values without
  // recursion or a payload tree. A valid line has at most bytes / 2 containers.
  for (;;) {
   if (stack.empty()) return;
   auto& frame = stack.back();
   scalar = nullptr;
   array_target = nullptr;
   if (frame.is_object) {
    if (frame.child_consumed) ++frame.object;
    if (frame.object == simdjson::ondemand::object_iterator{}) {
     stack.resize(stack.size() - 1);
     continue;
    }
    auto field = *frame.object;
    (void)field.unescaped_key().value();
    source = field.value().value();
   } else {
    if (frame.child_consumed) ++frame.array;
    if (frame.array == simdjson::ondemand::array_iterator{}) {
     stack.resize(stack.size() - 1);
     continue;
    }
    source = (*frame.array).value();
    if (frame.coordinates) {
     auto& count = *frame.coordinates->array_size;
     if (!frame.coordinates->sequence.empty()) {
      if (count >= frame.coordinates->sequence.size()) throw std::runtime_error("keypoint array exceeds admitted capacity");
      scalar = &frame.coordinates->sequence[count];
     } else if (count < frame.coordinates->coordinates.size()) scalar = &frame.coordinates->coordinates[count];
     ++count;
    }
   }
   frame.child_consumed = true;
   break;
  }
 }
}
[[nodiscard]] inline DatasetAnnotationJson project_dataset_annotation(simdjson::ondemand::object object, common::types::WorkspaceBuffer<DatasetAnnotationFrame>& stack, std::span<DatasetAnnotationScalar> joints) {
 DatasetAnnotationJson result;
 for (auto field : object) {
  const auto key = field.unescaped_key().value();
  DatasetAnnotationValue* target = nullptr;
  frameworks::reflection::visit_materialized_members<DatasetAnnotationJson>([&]<class Member>(const auto& fact) {
   // `class` is the single external spelling that is a C++ keyword.
   const auto name = Member::pointer == &DatasetAnnotationJson::class_name ? std::string_view{"class"} : fact.member_name;
   if (key == name) target = &(result.*Member::pointer).emplace();
  });
  if (target && key == "keypoints") {
   std::ranges::fill(joints, DatasetAnnotationScalar{});
   target->sequence = joints;
  }
  consume_dataset_annotation_value(field.value(), stack, target);
 }
 // Document-order assignment deliberately retains the final duplicate key.
 return result;
}
}  // namespace mmltk::backend::data::compiler_internal
