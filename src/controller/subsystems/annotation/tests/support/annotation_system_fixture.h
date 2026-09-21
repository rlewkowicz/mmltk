#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <catch2/catch_test_macros.hpp>
#include "src/test_support/async_test_utils.hpp"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/controller/subsystems/annotation/detail/annotation_render_state.h"
#include "src/controller/subsystems/annotation/tests/support/annotation_test_utils.hpp"
#include "src/controller/presentation/tests/support/visual_system_fixture.h"
namespace mmltk::controller::visual_test_support {
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
struct AnnotationRenderProbe final {
 std::atomic_uint64_t calls{0U};
 std::atomic_uint64_t samples{0U};
 std::atomic<std::uint8_t> semantic_value{0xa5U};
 std::atomic_bool fail_open{false};
 std::atomic_bool fail_render{false};
 std::atomic_bool exact_content{true};
 std::mutex mutex;
 std::shared_ptr<MutationCommitProbe> hold;
 std::shared_ptr<MutationCommitProbe> sample_hold;
 std::shared_ptr<MutationCommitProbe> open_hold;
 void Wait(std::shared_ptr<MutationCommitProbe>& pending) {
  std::shared_ptr<MutationCommitProbe> gate;
  {
   std::scoped_lock lock(mutex);
   gate = std::exchange(pending, {});
  }
  if (gate) {
   gate->committed.set_value();
   gate->released.wait();
  }
 }
};
class TestAnnotationAlgorithm final : public AnnotationAlgorithm {
public:
 static VisualRuntimeFactory CreateRuntime(std::shared_ptr<FakeImageBackend> backend, std::shared_ptr<AnnotationRenderProbe> probe = {}) {
  return RuntimeFactory(0, std::move(backend), mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                        [probe = std::move(probe)] { return std::make_unique<TestAnnotationAlgorithm>(probe); });
 }
 explicit TestAnnotationAlgorithm(std::shared_ptr<AnnotationRenderProbe> probe = {}) : probe_(std::move(probe)) {}
 void Open(mmltk::frameworks::gpu::ImagePlaneView, VisualRegion, VisualExtent) override {
  if (probe_) probe_->Wait(probe_->open_hold);
  if (probe_ && probe_->fail_open.exchange(false)) throw std::runtime_error("deterministic source preparation failure");
 }
 contracts::AnnotationColor Sample(contracts::AnnotationPoint) override {
  if (probe_) ++probe_->samples;
  if (probe_) probe_->Wait(probe_->sample_hold);
  return {120.0F, 1.0F, 1.0F};
 }
 void Render(const AnnotationRenderState& description, const mmltk::frameworks::gpu::ImagePlaneView source, const mmltk::frameworks::gpu::ImagePlaneView clean,
             const mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t) const override {
  if (probe_) {
   const auto scene = *description.scene;
   const auto editor = description.editor;
   const auto preview = description.preview;
   const auto preview_object = description.preview_object;
   probe_->calls.fetch_add(1U, std::memory_order_release);
   probe_->Wait(probe_->hold);
   if (*description.scene != scene || description.editor != editor || description.preview != preview || description.preview_object != preview_object)
    probe_->exact_content = false;
   if (probe_->fail_render.exchange(false)) throw std::runtime_error("deterministic render failure");
  }
  if (source.valid()) mmltk::frameworks::gpu::test_support::CopyImagePlane(clean, source);
  Fill(semantic, probe_ ? probe_->semantic_value.load() : std::uint8_t{0xa5U});
 }

private:
 std::shared_ptr<AnnotationRenderProbe> probe_;
};
[[nodiscard]] inline mmltk::frameworks::gpu::BorrowedImageProductReadView hold_annotation_frame(AnnotationSystem& annotation, EventGate& events,
                                                                                                contracts::AnnotationTool tool) {
 const auto edit = annotation.Edit({.edit = {.value = AnnotationToolEdit{tool}}});
 mmltk::testsupport::await_annotation_command(annotation, events, edit.revision);
 mmltk::testsupport::await_annotation_render(annotation, events);
 auto retained = annotation.BorrowFrame();
 REQUIRE(retained.valid());
 return retained;
}
}  // namespace mmltk::controller::visual_test_support
