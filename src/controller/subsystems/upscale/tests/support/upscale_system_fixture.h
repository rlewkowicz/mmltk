#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "src/test_support/async_test_utils.hpp"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/controller/subsystems/upscale/upscale_system.h"
#include "src/controller/presentation/tests/support/visual_system_fixture.h"
namespace mmltk::controller::visual_test_support {
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
class TestUpscalePreparation : public UpscaleAlgorithm {
public:
 void Resample(mmltk::frameworks::gpu::ImagePlaneView source, mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t) override {
  Fill(target, *reinterpret_cast<const std::uint8_t*>(source.data));
 }
};
class TestUpscaleAlgorithm final : public TestUpscalePreparation {
public:
 void Semantics(const mmltk::frameworks::gpu::ImagePlaneView source, const mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t) override {
  if (semantics_) semantics_->fetch_add(1U);
  Fill(target, source.valid() ? *reinterpret_cast<const std::uint8_t*>(source.data) : 0U);
 }
 explicit TestUpscaleAlgorithm(std::shared_ptr<std::atomic<UpscaleKernel>> kernel, std::shared_ptr<MutationCommitProbe> gate = {}, std::shared_ptr<std::atomic_uint32_t> runs = {},
  std::uint32_t gate_run = 1U, std::shared_ptr<std::atomic_uint32_t> semantics = {})
     : kernel_(std::move(kernel)), gate_(std::move(gate)), runs_(std::move(runs)), gate_run_(gate_run), semantics_(std::move(semantics)) {}
 [[nodiscard]] static VisualRuntimeFactory CreateRuntime(
  std::shared_ptr<FakeImageBackend> backend, std::shared_ptr<std::atomic<UpscaleKernel>> kernel, std::shared_ptr<std::atomic_uint32_t> runs, std::shared_ptr<std::atomic_uint32_t> semantics = {}) {
  return RuntimeFactory(
   0, std::move(backend), mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
   [kernel = std::move(kernel), runs = std::move(runs), semantics = std::move(semantics)] { return std::make_unique<TestUpscaleAlgorithm>(kernel, nullptr, runs, 1U, semantics); }, 4U);
 }
 void Warm() override {}
 void Run(const UpscaleKernel kernel, mmltk::frameworks::gpu::ImagePlaneView, const mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t, const std::function<bool()>&,
  UpscalePurpose = UpscalePurpose::Normal) override {
  kernel_->store(kernel, std::memory_order_release);
  if (runs_) runs_->fetch_add(1U, std::memory_order_acq_rel);
  if (gate_ && (!runs_ || runs_->load() == gate_run_)) {
   gate_->committed.set_value();
   gate_->released.wait();
   gate_.reset();
  }
  Fill(target, static_cast<std::uint8_t>(kernel) + 1U);
 }

private:
 std::shared_ptr<std::atomic<UpscaleKernel>> kernel_;
 std::shared_ptr<MutationCommitProbe> gate_;
 std::shared_ptr<std::atomic_uint32_t> runs_;
 std::uint32_t gate_run_;
 std::shared_ptr<std::atomic_uint32_t> semantics_;
};
struct UpscaleExtentProbe final {
 std::mutex mutex;
 std::vector<mmltk::frameworks::gpu::ImagePlaneView> sources;
 // CLEANUP-IGNORE: Captured output views and scalar admission dimensions are distinct test evidence.
 std::vector<mmltk::frameworks::gpu::ImagePlaneView> targets;
};
class ExtentUpscaleAlgorithm final : public TestUpscalePreparation {
public:
 void Semantics(mmltk::frameworks::gpu::ImagePlaneView, const mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t) override {
  if (target.valid()) Fill(target, 0U);
 }
 explicit ExtentUpscaleAlgorithm(std::shared_ptr<UpscaleExtentProbe> probe) : probe_(std::move(probe)) {}
 void Warm() override {}
 void Run(UpscaleKernel, const mmltk::frameworks::gpu::ImagePlaneView source, const mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t, const std::function<bool()>&,
  UpscalePurpose = UpscalePurpose::Normal) override {
  {
   std::scoped_lock lock(probe_->mutex);
   probe_->sources.push_back(source);
   probe_->targets.push_back(target);
  }
  Fill(target, *reinterpret_cast<const std::uint8_t*>(source.data));
 }

private:
 std::shared_ptr<UpscaleExtentProbe> probe_;
};
[[nodiscard]] inline UpscaleRequest test_upscale_request(UpscaleRequest request) {
 request.document = test_document({}).document->facts();
 return request;
}
}  // namespace mmltk::controller::visual_test_support
