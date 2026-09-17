#include "src/backend/ml/torch/tests/catch_support.h"
#include <catch2/generators/catch_generators.hpp>
#include <ATen/ATen.h>
#include <ATen/TensorIndexing.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime_api.h>
#include <unistd.h>
#include <vector>
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
    if (!(status == CUDA_SUCCESS && mmap) && ::access("/dev/gdrdrv", R_OK | W_OK) != 0)
        SKIP("GDR backend unavailable; assignment transfer and autograd hardware behavior remain unverified");
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
        workspace.prepare_cost({3, 2, 8, 4}, at::Device(at::kCUDA, device_index));
        auto* const backing = workspace.device_layer(0).data_ptr();
        workspace.device_layer(0).fill_(7);
        workspace.device_layer(1).fill_(8);
        workspace.device_layer(2).fill_(9);
        auto large = workspace.read_cost();
        REQUIRE(large.numel() == 192);
        large = at::Tensor{};
        workspace.prepare_cost({1, 1, 2, 1}, at::Device(at::kCUDA, device_index));
        REQUIRE(workspace.device_layer(0).data_ptr() == backing);
        workspace.device_layer(0).fill_(11);
        const auto small = workspace.read_cost();
        REQUIRE(small.is_contiguous());
        REQUIRE(small.numel() == 2);
        REQUIRE(small.flatten()[1].item<float>() == 11.0F);
        const auto counters = workspace.statistics();
        REQUIRE(counters.cost_submissions == 2);
        REQUIRE(counters.cost_dependencies == 2);
        REQUIRE(counters.cost_bytes == (192 + 2) * sizeof(float));
        REQUIRE_THROWS(workspace.prepare_cost({1, -1}, at::Device(at::kCUDA, device_index)));
        if (devices > 1) {
            const auto other_device = static_cast<c10::DeviceIndex>((device + 1) % devices);
            REQUIRE_THROWS(workspace.prepare_cost({1, 1, 2, 1}, at::Device(at::kCUDA, other_device)));
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
        escaped_loss.backward();
        REQUIRE(at::equal(mmltk::backend::ml::cuda::numa_readback(values.grad()), at::tensor({1.f, 0.f, 1.f, 0.f, 1.f, 0.f}).view({2, 3})));
    }
}
