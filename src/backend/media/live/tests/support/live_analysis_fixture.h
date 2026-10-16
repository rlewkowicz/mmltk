#pragma once
#include "src/backend/media/live/live_analysis_product.h"
#include "src/backend/data/catalog/tests/keypoint_schema_fixture.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
namespace mmltk::backend::media::live::test_support {
// Device-copy injection uses ordinary memory; all layout, count, schema,
// allocation, and publication logic remains in LiveAnalysisProduct.
struct LiveAnalysisFixture final {
 using RuntimeAnnotation = mmltk::backend::ml::runtime::AnalysisAnnotationStorage;
 using Element = mmltk::backend::ml::runtime::AnalysisElementType;
 using Joint = mmltk::backend::ml::runtime::AnalysisKeypoint;
 std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> catalog = MakeCatalog();
 std::shared_ptr<std::int64_t> count = std::make_shared<std::int64_t>(2);
 std::array<std::array<float, 4>, 2> boxes{{{1, 1, 2, 2}, {1, 1, 3, 3}}};
 std::array<std::int32_t, 2> classes{0, 1}, joint_counts{2, 0};
 std::array<float, 2> scores{.9F, .8F};
 std::array<std::array<std::uint8_t, 3>, 2> colors{{{255, 0, 0}, {0, 255, 0}}};
 std::array<std::uint8_t, 8> masks{1, 0, 0, 1, 0, 1, 1, 0};
 std::array<Joint, 4> joints{{{1.25F, 1.5F, .75F, 4.F, -.5F, 9.F}, {2.F, 2.5F, .6F, 1.F, .25F, 2.F}, {}, {}}};
 static std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> MakeCatalog() {
  using namespace mmltk::backend::data::catalog;
  OrderedClassCatalog record{
   .names = {{"person"}, {"box"}}, .keypoints = std::vector<ClassKeypointSchema>{{.names = {"left", "right"}, .skeleton = {{0, 1}}, .horizontal_flip = {1, 0}, .oks_sigmas = {.1, .1}}, {}}
  };
  return std::make_shared<ClassCatalog>(record);
 }
 template <class Range>
 static mmltk::backend::ml::runtime::AnalysisDeviceBuffer Buffer(Range& values, std::initializer_list<std::uint32_t> extents, Element type) {
  mmltk::backend::ml::runtime::AnalysisDeviceBuffer result{
   .address = reinterpret_cast<std::uintptr_t>(values.data()),
   .capacity_bytes = std::as_bytes(std::span{values}).size(),
   .shape = {.rank = static_cast<std::uint8_t>(extents.size())},
   .element_type = type
  };
  std::size_t axis = 0;
  for (const auto extent : extents) result.shape.extents[axis++] = extent;
  return result;
 }
 RuntimeAnnotation Annotation(bool pending = true) {
  RuntimeAnnotation result{
   .source_region = {1, 1, 2, 2},
   .value_capacity = 2,
   .boxes_xyxy = Buffer(boxes, {2, 4}, Element::Float32),
   .class_references = Buffer(classes, {2}, Element::Int32),
   .confidences = Buffer(scores, {2}, Element::Float32),
   .colors_rgb = Buffer(colors, {2, 3}, Element::Uint8),
   .masks = Buffer(masks, {2, 2, 2}, Element::Uint8),
   .masks_available = true,
   .keypoint_capacity = 2,
   .keypoints = Buffer(joints, {2, 2, static_cast<std::uint32_t>(mmltk::backend::ml::runtime::kAnalysisKeypointChannels)}, Element::Float32),
   .keypoint_counts = Buffer(joint_counts, {2}, Element::Int32),
   .keypoints_available = true,
   .class_domain = mmltk::backend::data::catalog::ClassReferenceDomain::Foreground,
   .class_catalog = catalog
  };
  if (pending)
   result.count.PublishPending(count.get(), count.get(), count, 2);
  else
   result.count.SetKnown(static_cast<std::size_t>(*count), 2);
  return result;
 }
 static void DeliverLargeSchema(LiveAnalysisProduct& product, LiveFrameId frame, std::uint32_t count) {
  namespace catalog = mmltk::backend::data::catalog;
  constexpr auto capacity = static_cast<std::uint32_t>(mmltk::backend::ml::runtime::kMaximumAnalysisRegions);
  if (!count || count > capacity * capacity) throw std::invalid_argument("fixture exceeds Live region capacity");
  const auto schema = catalog::test_support::maximum_keypoint_schema();
  auto classes = std::make_shared<const catalog::ClassCatalog>(catalog::OrderedClassCatalog{{{"custom"}}, std::vector{schema}});
  std::vector<std::array<float, 4>> boxes(capacity, {0, 0, 3, 3});
  std::vector<std::int32_t> categories(capacity, 0), counts(capacity, 256);
  std::vector<float> scores(capacity, .9F);
  std::vector<std::array<std::uint8_t, 3>> colors(capacity, {255, 0, 0});
  std::vector<Joint> joints(std::size_t(capacity) * 256, {2, 2, .9F, 1, 0, 1});
  RuntimeAnnotation annotation{
   .source_region = {0, 0, 4, 4},
   .value_capacity = capacity,
   .boxes_xyxy = Buffer(boxes, {capacity, 4}, Element::Float32),
   .class_references = Buffer(categories, {capacity}, Element::Int32),
   .confidences = Buffer(scores, {capacity}, Element::Float32),
   .colors_rgb = Buffer(colors, {capacity, 3}, Element::Uint8),
   .keypoint_capacity = 256,
   .keypoints = Buffer(joints, {capacity, 256, static_cast<std::uint32_t>(mmltk::backend::ml::runtime::kAnalysisKeypointChannels)}, Element::Float32),
   .keypoint_counts = Buffer(counts, {capacity}, Element::Int32),
   .keypoints_available = true,
   .class_domain = catalog::ClassReferenceDomain::Foreground,
   .class_catalog = std::move(classes)
  };
  // Reach aggregate metadata admission through valid bounded regions.
  std::vector<RuntimeAnnotation> regions;
  for (std::uint32_t begin = 0; begin < count; begin += capacity) {
   regions.push_back(annotation);
   regions.back().count.SetKnown(std::min(count - begin, capacity), capacity);
  }
  DeliverAnnotations(product, frame, regions);
 }
 static cudaError_t Copy(void* target, const void* source, std::size_t bytes, std::uintptr_t) noexcept {
  std::memcpy(target, source, bytes);
  return cudaSuccess;
 }
 static void DeliverAnnotations(LiveAnalysisProduct& product, LiveFrameId frame, std::span<const RuntimeAnnotation> annotations) {
  product.PrepareDevice(frame, 4, 4, annotations);
  if (product.EnqueueDeviceCopy(1, &Copy) != cudaSuccess) throw std::runtime_error("fixture copy failed");
  product.CompleteDeviceCopy();
 }
 void Deliver(LiveAnalysisProduct& product, LiveFrameId frame, bool pending = true) {
  auto annotation = Annotation(pending);
  DeliverAnnotations(product, frame, std::span{&annotation, 1});
 }
};
}  // namespace mmltk::backend::media::live::test_support
