#include "src/backend/ml/torch/tests/catch_support.h"
#include <catch2/generators/catch_generators.hpp>
#include <ATen/ATen.h>
#include <ATen/TensorIndexing.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime_api.h>
#include <unistd.h>
#include <vector>
#include <cstring>
#include <limits>
#include "detail/detr_matcher_cuda.h"
#include <stdexcept>
#include <string_view>
#include "detail/matcher_workspace.h"
#include "src/backend/ml/cuda/numa_host_tensor.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include "src/common/system/execution_policy.h"
using namespace mmltk::backend::models::rfdetr;
namespace {
using Indices = std::vector<std::vector<std::pair<at::Tensor, at::Tensor>>>;
at::Tensor longs(std::initializer_list<std::int64_t> values) { return at::tensor(values, at::TensorOptions().dtype(at::kLong)); }
Indices assignments() { return {{{longs({2, 0}), longs({1, 0})}, {longs({1}), longs({0})}}, {{longs({0}), longs({1})}, {longs({}), longs({})}}}; }
void require_transport(bool h2d, int device) {
 if (h2d) return;
 int mmap = 0;
 const auto status = cuDeviceGetAttribute(&mmap, static_cast<CUdevice_attribute>(152), device);
 if (!(status == CUDA_SUCCESS && mmap) && ::access("/dev/gdrdrv", R_OK | W_OK) != 0) SKIP("GDR backend unavailable; assignment transfer and autograd hardware behavior remain unverified");
}
struct MatcherDeviceScope final {
 explicit MatcherDeviceScope(const int device) : MatcherDeviceScope(device, false, true) {}
 MatcherDeviceScope(const int device, const bool h2d) : MatcherDeviceScope(device, true, h2d) {}
 c10::DeviceIndex device_index;
 c10::cuda::CUDAGuard guard;
 mmltk::frameworks::gpu::DeviceExecution execution;
 mmltk::common::system::ScopedExecutionPolicy policy;

private:
 MatcherDeviceScope(const int device, const bool verify_transport, const bool h2d)
     : device_index(static_cast<c10::DeviceIndex>(device)),
       guard(device_index),
       execution(select_execution(device, verify_transport, h2d)),
       policy({execution.placement.cpus, {}, 0, execution.placement.numa_node, -10, false}) {}
 [[nodiscard]] static mmltk::frameworks::gpu::DeviceExecution select_execution(const int device, const bool verify_transport, const bool h2d) {
  REQUIRE(cudaSetDevice(device) == cudaSuccess);
  if (verify_transport) require_transport(h2d, device);
  return mmltk::frameworks::gpu::test_support::selected_test_device(device, mmltk::common::system::NumaTopology::Capture());
 }
};
}  // namespace
TEST_CASE("Matcher assignment CPU projections share one immutable packed set", "[rfdetr][matcher][numa]") {
 const auto topology = mmltk::common::system::NumaTopology::Capture();
 MatcherWorkspace workspace(topology.permitted_nodes.front(), true);
 workspace.enable_statistics();
 auto packed = workspace.pack(assignments(), {0, 2}, at::Device(at::kCPU));
 REQUIRE(packed.size() == 2);
 REQUIRE(at::equal(packed[0].source.first, longs({0, 0, 1})));
 REQUIRE(at::equal(packed[0].source.second, longs({2, 0, 1})));
 REQUIRE(at::equal(packed[0].global_targets, longs({1, 0, 2})));
 REQUIRE(at::equal(packed[1].global_targets, longs({1})));
 REQUIRE(workspace.statistics().materializations == 1);
 REQUIRE(workspace.statistics().assignment_bytes == 4 * 3 * sizeof(std::int64_t));
 REQUIRE(workspace.statistics().uploads == 0);
 REQUIRE_THROWS(workspace.cpu_indices(-1));
 REQUIRE_THROWS(workspace.pack(assignments(), {0}, at::Device(at::kCPU)));
 auto empty = workspace.pack(Indices(2), {}, at::Device(at::kCPU));
 REQUIRE(empty[0].source.first.numel() == 0);
 REQUIRE(at::equal(packed[0].source.second, longs({2, 0, 1})));
 at::Tensor retained;
 {
  MatcherWorkspace scoped(topology.permitted_nodes.front(), true);
  retained = scoped.cpu_indices(2);
  retained.fill_(31);
 }
 REQUIRE(retained.flatten()[3].item<std::int64_t>() == 31);
}
// CLEANUP-IGNORE: Rectangular packing and immutable projections require independent topology snapshots and workspace lifetimes.
TEST_CASE("Rectangular assignment packing uses full target offsets and retained active extents", "[rfdetr][matcher][numa]") {
 const auto topology = mmltk::common::system::NumaTopology::Capture();
 MatcherWorkspace workspace(topology.permitted_nodes.front(), true);
 workspace.enable_statistics();
 const auto rows = at::arange(300, at::TensorOptions().dtype(at::kLong));
 const auto shorter_rows = rows.narrow(0, 0, 150);
 const auto none = longs({});
 const Indices rectangular{
  {{rows, rows}, {none, none}, {longs({0}), longs({0})}},
  {{shorter_rows, shorter_rows}, {none, none}, {longs({0}), longs({0})}},
 };
 const auto packed = workspace.pack(rectangular, {0, 404, 404}, at::Device(at::kCPU));
 REQUIRE(packed[0].source.second.numel() == 301);
 REQUIRE(packed[1].source.second.numel() == 151);
 REQUIRE(packed[0].global_targets[300].item<int64_t>() == 404);
 REQUIRE(packed[1].global_targets[150].item<int64_t>() == 404);
 REQUIRE(workspace.statistics().assignment_bytes == (301 + 151) * 3 * sizeof(int64_t));
 const auto next = workspace.pack(assignments(), {0, 2}, at::Device(at::kCPU));
 REQUIRE(next[0].global_targets.numel() == 3);
 REQUIRE(at::equal(packed[0].source.second.narrow(0, 0, 300), rows));
 REQUIRE(at::equal(packed[1].global_targets.narrow(0, 0, 150), shorter_rows));
}
TEST_CASE("Matcher costs copy only compact active shapes after high-water growth", "[rfdetr][matcher][cuda][numa]") {
 int devices = 0;
 if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA unavailable; active cost transfer remains unverified");
 for (int device = 0; device < devices; ++device) {
  MatcherDeviceScope device_scope(device);
  const auto device_index = device_scope.device_index;
  const auto& p = device_scope.execution.placement;
  MatcherWorkspace workspace(p.numa_node, true);
  workspace.enable_statistics();
  workspace.prepare_cost({8, 3, 5}, {0, 4, 1}, at::Device(at::kCUDA, device_index));
  auto* const backing = workspace.device_layer(0).data_ptr();
  workspace.device_layer(0).fill_(7);
  workspace.device_layer(1).fill_(8);
  workspace.device_layer(2).fill_(9);
  auto large = workspace.read_cost();
  REQUIRE(large.numel() == 80);
  REQUIRE(workspace.cpu_matrix(0, 0).numel() == 0);
  REQUIRE(workspace.cpu_matrix(1, 1).sizes() == at::IntArrayRef({3, 4}));
  REQUIRE(workspace.cpu_matrix(2, 2).stride(0) == 1);
  REQUIRE(workspace.cpu_matrix(2, 2)[4][0].item<float>() == 9.F);
  large = at::Tensor{};
  workspace.prepare_cost({2}, {1}, at::Device(at::kCUDA, device_index));
  REQUIRE(workspace.device_layer(0).data_ptr() == backing);
  workspace.device_layer(0).fill_(11);
  const auto small = workspace.read_cost();
  REQUIRE(small.is_contiguous());
  REQUIRE(small.numel() == 2);
  REQUIRE(small.flatten()[1].item<float>() == 11.0F);
  const auto counters = workspace.statistics();
  REQUIRE(counters.cost_submissions == 2);
  REQUIRE(counters.cost_dependencies == 2);
  REQUIRE(counters.cost_bytes == (80 + 2) * sizeof(float));
  const auto require_completed = [&] {
   const auto matrix = workspace.cpu_matrix(0, 0);
   REQUIRE(matrix.sizes() == at::IntArrayRef({2, 1}));
   REQUIRE(matrix.eq(11).all().item<bool>());
  };
  constexpr auto limit = std::numeric_limits<int64_t>::max() / sizeof(float);
  const auto reject_layout = [&](at::IntArrayRef queries, at::IntArrayRef counts, const at::Device& target_device) {
   REQUIRE_THROWS(workspace.prepare_cost(queries, counts, target_device));
   require_completed();
  };
  const at::Device gpu(at::kCUDA, device_index);
  reject_layout({1}, {-1}, gpu);
  reject_layout({-1}, {1}, gpu);
  reject_layout({1}, {limit, 1}, gpu);
  reject_layout({std::numeric_limits<int64_t>::max()}, {2}, gpu);
  reject_layout({limit, 1}, {1}, gpu);
  // Compact storage fits, but the former padded shape must still be rejected.
  reject_layout({limit / 2, 0}, {1, 1}, gpu);
  reject_layout({2}, {1}, at::Device(at::kCPU));
  reject_layout({2}, {1}, at::Device(at::kCUDA));
  REQUIRE_THROWS(workspace.cpu_matrix(2, 0));
  // An exception after queued writes leaves the workspace responsible for settlement.
  try {
   MatcherWorkspace pending(p.numa_node, true);
   pending.prepare_cost({16}, {3}, at::Device(at::kCUDA, device_index));
   pending.device_layer(0).fill_(13);
   REQUIRE_THROWS(pending.cpu_matrix(0, 0));
   pending.prepare_cost({2}, {1}, at::Device(at::kCUDA, device_index));
   pending.device_layer(0).fill_(17);
   REQUIRE(pending.read_cost()[0].item<float>() == 17.F);
   pending.prepare_cost({16}, {3}, at::Device(at::kCUDA, device_index));
   pending.device_layer(0).fill_(19);
   throw std::runtime_error("abandon pending matcher generation");
  } catch (const std::runtime_error& error) { REQUIRE(std::string_view(error.what()) == "abandon pending matcher generation"); }
  if (devices > 1) {
   const auto other_device = static_cast<c10::DeviceIndex>((device + 1) % devices);
   reject_layout({2}, {1}, at::Device(at::kCUDA, other_device));
  }
  struct Layout final {
   std::vector<int64_t> queries, counts;
  };
  const std::vector<Layout> layouts{
   {{2}, {1}},                       // Equal-size reuse.
   {{8, 3, 5}, {0, 4, 1}},           // Regrow within the initial high water.
   {{5, 8, 3}, {2, 0, 3}},           // Same sizes, changed layer and image intervals.
   {{1, 0}, {0, 1}},                 // Shrink both metadata vectors, including an empty layer.
   {{3, 2, 1}, {1, 0, 2}},           // Regrow metadata and storage within capacity.
   {{9, 7, 5, 3}, {0, 3, 0, 4, 2}},  // Genuine metadata and cost-storage growth.
   {{2}, {0, 0}},                    // Empty image intervals after growth.
  };
  std::uint64_t expected_bytes = counters.cost_bytes;
  for (std::size_t iteration = 0; iteration < layouts.size(); ++iteration) {
   const auto& layout = layouts[iteration];
   workspace.prepare_cost(layout.queries, layout.counts, gpu);
   if (iteration < 5) REQUIRE(workspace.device_layer(0).data_ptr() == backing);
   int64_t targets = 0;
   for (const auto count : layout.counts) targets += count;
   int64_t elements = 0;
   for (std::size_t layer = 0; layer < layout.queries.size(); ++layer) {
    auto output = workspace.device_layer(layer);
    output.copy_(at::arange(output.numel(), output.options()) + static_cast<int64_t>(layer) * 1000);
    elements += layout.queries[layer] * targets;
   }
   REQUIRE(workspace.read_cost().numel() == elements);
   for (std::size_t layer = 0; layer < layout.queries.size(); ++layer) {
    int64_t begin = static_cast<int64_t>(layer) * 1000;
    for (std::size_t image = 0; image < layout.counts.size(); ++image) {
     const auto matrix = workspace.cpu_matrix(layer, image);
     REQUIRE(matrix.sizes() == at::IntArrayRef({layout.queries[layer], layout.counts[image]}));
     REQUIRE(at::equal(matrix.flatten(), at::arange(begin, begin + matrix.numel(), matrix.options())));
     begin += matrix.numel();
    }
   }
   expected_bytes += elements * sizeof(float);
   const auto reused = workspace.statistics();
   REQUIRE(reused.cost_submissions == counters.cost_submissions + iteration + 1);
   REQUIRE(reused.cost_dependencies == reused.cost_submissions);
   REQUIRE(reused.cost_bytes == expected_bytes);
  }
 }
}
TEST_CASE("Assignment transports retain autograd indices across overlapping results", "[rfdetr][matcher][cuda][numa]") {
 const bool h2d = GENERATE(true, false);
 int devices = 0;
 if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA unavailable; assignment lifetime remains unverified");
 for (int device = 0; device < devices; ++device) {
  MatcherDeviceScope device_scope(device, h2d);
  const auto device_index = device_scope.device_index;
  const auto& p = device_scope.execution.placement;
  MatcherWorkspace workspace(p.numa_node, h2d);
  workspace.enable_statistics();
  const at::Device gpu(at::kCUDA, device_index);
  auto values = at::ones({2, 3}, at::TensorOptions().dtype(at::kFloat).device(gpu));
  values.set_requires_grad(true);
  auto packed = workspace.pack(assignments(), {0, 2}, gpu);
  REQUIRE(at::equal(mmltk::backend::ml::cuda::numa_readback(packed[0].source.second), longs({2, 0, 1})));
  auto loss = values.index({packed[0].source.first, packed[0].source.second}).sum();
  packed.clear();
  // Releasing forward locals must not permit overwrite of autograd's saved indices.
  auto different = assignments();
  different[0][0].first = longs({1, 1});
  auto next = workspace.pack(different, {0, 2}, gpu);
  loss.backward({}, true);
  REQUIRE(at::equal(mmltk::backend::ml::cuda::numa_readback(values.grad()), at::tensor({1.f, 0.f, 1.f, 0.f, 1.f, 0.f}).view({2, 3})));
  values.grad().zero_();
  auto third = workspace.pack(assignments(), {0, 2}, gpu);
  loss.backward();
  REQUIRE(at::equal(mmltk::backend::ml::cuda::numa_readback(values.grad()), at::tensor({1.f, 0.f, 1.f, 0.f, 1.f, 0.f}).view({2, 3})));
  next.clear();
  third.clear();
  workspace.complete_assignments(c10::cuda::getCurrentCUDAStream(device_index).stream());
  const auto warm = workspace.statistics().storage_growth;
  {
   auto reused = workspace.pack(assignments(), {0, 2}, gpu);
   REQUIRE(at::equal(reused[0].cpu_source.second, longs({2, 0, 1})));
  }
  workspace.complete_assignments(c10::cuda::getCurrentCUDAStream(device_index).stream());
  const auto counters = workspace.statistics();
  REQUIRE(counters.materializations == 4);
  REQUIRE(counters.uploads == 4);
  REQUIRE(counters.h2d_submissions == (h2d ? 4 : 0));
  REQUIRE(counters.gdr_writes == (h2d ? 0 : 4));
  REQUIRE(counters.storage_growth == warm);
  at::Tensor escaped_loss;
  values.grad().zero_();
  try {
   MatcherWorkspace cancelled(p.numa_node, h2d);
   auto escaped = cancelled.pack(assignments(), {0, 2}, gpu);
   escaped_loss = values.index({escaped[0].source.first, escaped[0].source.second}).sum();
   throw std::runtime_error("cancel owner after submitting forward");
  } catch (const std::runtime_error& error) { REQUIRE(std::string_view(error.what()) == "cancel owner after submitting forward"); }
  // The result's retained context/storage outlives exception-driven owner
  // destruction, and remains usable by a later backward consumer.
  // CLEANUP-IGNORE: Backward after owner destruction independently proves escaped storage lifetime, unlike workspace reuse.
  escaped_loss.backward();
  REQUIRE(at::equal(mmltk::backend::ml::cuda::numa_readback(values.grad()), at::tensor({1.f, 0.f, 1.f, 0.f, 1.f, 0.f}).view({2, 3})));
 }
}
TEST_CASE("Compact matcher matrices preserve CUDA pair bits across independent lookup ranges", "[rfdetr][matcher][cuda][numa]") {
 int devices = 0;
 if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA unavailable; compact pair arithmetic remains unverified");
 MatcherDeviceScope scope(0);
 const at::Device device(at::kCUDA, scope.device_index);
 const auto options = at::TensorOptions().dtype(at::kFloat).device(device);
 const std::vector<int64_t> counts{0, 4, 2, 4}, offsets{0, 2, 0, 2}, queries{2, 5};
 const auto lookup = at::tensor(offsets, options.dtype(at::kLong));
 const auto device_counts = at::tensor(counts, options.dtype(at::kLong));
 auto labels = at::tensor({0L, 1L, 0L, 1L, 0L, 1L}, options.dtype(at::kLong));
 auto boxes = at::arange(24, options).view({6, 4}) / 32;
 auto masks = at::arange(6 * 35, options).view({6, 35}).remainder(2);
 MatcherWorkspace mixed(scope.execution.placement.numa_node, true);
 mixed.enable_statistics();
 mixed.prepare_cost(queries, counts, device);
 auto prefixes = mixed.output_offsets(offsets, lookup);
 REQUIRE(prefixes.data_ptr() != lookup.data_ptr());
 REQUIRE(at::equal(mmltk::backend::ml::cuda::numa_readback(prefixes), longs({0, 0, 4, 6})));
 std::vector<at::Tensor> logits, predictions, sampled;
 for (size_t layer = 0; layer < queries.size(); ++layer) {
  const auto q = queries[layer];
  logits.push_back(at::arange(4 * q * 2, options).view({4, q, 2}) / 8 - 2);
  predictions.push_back(at::arange(4 * q * 4, options).view({4, q, 4}) / 64);
  sampled.push_back(at::arange(4 * q * 35, options).view({4, q, 35}) / 64 - 1);
  logits.back()[1][0][0].fill_(std::numeric_limits<float>::infinity());
  logits.back()[2][0][1].fill_(std::numeric_limits<float>::quiet_NaN());
  predictions.back()[3][0][0].fill_(-0.F);
  sampled.back()[3][0][0].fill_(-std::numeric_limits<float>::infinity());
  pairwise_detection_cost_cuda_out(mixed.device_layer(layer), logits.back(), predictions.back(), labels, boxes, lookup, device_counts, prefixes, 5, 4, 1, 5, 2, .25);
  pairwise_mask_cost_cuda_add_(mixed.device_layer(layer), sampled.back(), masks, lookup, device_counts, prefixes, 5, 4, 1, 1);
 }
 (void)mixed.read_cost();
 REQUIRE(mixed.statistics().cost_submissions == 1);
 REQUIRE(mixed.statistics().cost_dependencies == 1);
 REQUIRE(mixed.statistics().cost_bytes == sizeof(float) * (2 + 5) * (0 + 4 + 2 + 4));
 for (size_t layer = 0; layer < queries.size(); ++layer) {
  for (size_t image = 1; image < counts.size(); ++image) {
   MatcherWorkspace single(scope.execution.placement.numa_node, true);
   single.prepare_cost({queries[layer]}, {counts[image]}, device);
   const auto zero = longs({0}).to(device);
   const auto count = longs({counts[image]}).to(device);
   auto output = single.device_layer(0);
   pairwise_detection_cost_cuda_out(output, logits[layer].narrow(0, image, 1), predictions[layer].narrow(0, image, 1), labels.narrow(0, offsets[image], counts[image]),
    boxes.narrow(0, offsets[image], counts[image]), zero, count, zero, queries[layer], counts[image], 1, 5, 2, .25);
   pairwise_mask_cost_cuda_add_(output, sampled[layer].narrow(0, image, 1), masks.narrow(0, offsets[image], counts[image]), zero, count, zero, queries[layer], counts[image], 1, 1);
   const auto expected = single.read_cost();
   const auto actual = mixed.cpu_matrix(layer, image);
   REQUIRE(actual.is_contiguous());
   REQUIRE(actual.nbytes() == expected.nbytes());
   REQUIRE(std::memcmp(actual.data_ptr(), expected.data_ptr(), actual.nbytes()) == 0);
  }
 }
 MatcherWorkspace canonical(scope.execution.placement.numa_node, true);
 canonical.prepare_cost({2}, {0, 4, 2, 4}, device);
 const auto canonical_offsets = longs({0, 0, 4, 6}).to(device);
 REQUIRE(canonical.output_offsets({0, 0, 4, 6}, canonical_offsets).data_ptr() == canonical_offsets.data_ptr());
 const auto out = canonical.device_layer(0);
 const auto invoke = [&](const at::Tensor& destination, const at::Tensor& prediction, const at::Tensor& metadata, double coefficient, int64_t padded_queries, int64_t max_targets) {
  pairwise_detection_cost_cuda_out(destination, prediction, predictions[0], labels, boxes, lookup, metadata, canonical_offsets, padded_queries, max_targets, coefficient, 5, 2, .25);
 };
 REQUIRE_THROWS(invoke(out.to(at::kDouble), logits[0], device_counts, 1, 5, 4));
 REQUIRE_THROWS(invoke(out, logits[0].to(at::kCPU), device_counts, 1, 5, 4));
 REQUIRE_THROWS(invoke(out, logits[0], device_counts.to(at::kFloat), 1, 5, 4));
 REQUIRE_THROWS(invoke(out, logits[0].flatten(), device_counts, 1, 5, 4));
 REQUIRE_THROWS(invoke(out, logits[0], device_counts, std::numeric_limits<double>::infinity(), 5, 4));
 REQUIRE_THROWS(invoke(out, logits[0], device_counts, 1, 1, 4));
 REQUIRE_THROWS(invoke(out, logits[0], device_counts, 1, 5, std::numeric_limits<int64_t>::max()));
 REQUIRE_THROWS(pairwise_mask_cost_cuda_add_(out, sampled[0], masks, lookup, device_counts, canonical_offsets, 5, 4, std::numeric_limits<double>::quiet_NaN(), 1));
 REQUIRE_THROWS(pairwise_mask_cost_cuda_add_(out, sampled[0], masks.narrow(1, 0, 34), lookup, device_counts, canonical_offsets, 5, 4, 1, 1));
 // Device lookup counts cannot widen an output interval into the next image.
 invoke(out, logits[0], device_counts, 1, 5, 4);
 const auto guarded_expected = mmltk::backend::ml::cuda::numa_readback(out);
 const auto wide_counts = longs({100, 100, 100, 100}).to(device);
 out.fill_(23);
 invoke(out, logits[0], wide_counts, 1, 5, 4);
 const auto guarded_actual = mmltk::backend::ml::cuda::numa_readback(out);
 REQUIRE(std::memcmp(guarded_actual.data_ptr(), guarded_expected.data_ptr(), guarded_actual.nbytes()) == 0);
 const auto invalid_lookup = longs({-1, -1, 6, std::numeric_limits<int64_t>::max()}).to(device);
 out.fill_(23);
 pairwise_detection_cost_cuda_out(out, logits[0], predictions[0], labels, boxes, invalid_lookup, device_counts, canonical_offsets, 5, 4, 1, 5, 2, .25);
 pairwise_mask_cost_cuda_add_(out, sampled[0], masks, invalid_lookup, device_counts, canonical_offsets, 5, 4, 1, 1);
 REQUIRE(mmltk::backend::ml::cuda::numa_readback(out).eq(23).all().item<bool>());
 auto invalid_labels = at::full_like(labels, 2);
 pairwise_detection_cost_cuda_out(out, logits[0], predictions[0], invalid_labels, boxes, lookup, device_counts, canonical_offsets, 5, 4, 1, 5, 2, .25);
 REQUIRE(mmltk::backend::ml::cuda::numa_readback(out).eq(23).all().item<bool>());
}
