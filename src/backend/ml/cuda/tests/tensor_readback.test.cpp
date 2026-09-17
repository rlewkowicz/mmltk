#include "src/backend/ml/torch/tests/catch_support.h"
#include <ATen/ATen.h>
#include <cuda.h>
#include <ATen/ops/_neg_view.h>
#include <torch/serialize.h>
#include <array>
#include <atomic>
#include <sstream>
#include "src/test_support/cuda_test_utils.hpp"
#include "src/backend/ml/cuda/tensor_readback.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include "src/common/system/execution_policy.h"
namespace {
[[nodiscard]] mmltk::common::system::ScopedExecutionPolicy readback_test_policy() {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) SKIP("CUDA unavailable");
    // CLEANUP-IGNORE: Readback owns this device and placement scope independently of NUMA tensor storage tests.
    CUDA_ASSERT_OK(cudaSetDevice(0));
    const auto execution = mmltk::frameworks::gpu::test_support::selected_test_device(0, mmltk::common::system::NumaTopology::Capture());
    const auto& placement = execution.placement;
    return mmltk::common::system::ScopedExecutionPolicy({placement.cpus, {}, 0, placement.numa_node, -10, false});
}
}  // namespace
TEST_CASE("serialization readback preserves CPU views without registered capacity", "[readback][cpu]") {
    using mmltk::backend::ml::cuda::TensorReadbackBuffers;
    TensorReadbackBuffers readback;
    auto source = at::arange(30, at::kLong).reshape({5, 6});
    const std::array sources{source.narrow(0, 1, 3).transpose(0, 1), at::empty({0, 4}, at::kBool), at::scalar_tensor(7.5)};
    readback.Begin();
    readback.Reserve(sources);
    for (std::size_t index = 0; index < sources.size(); ++index) {
        auto view = readback.Stage(index);
        readback.Complete();
        REQUIRE(view.is_cpu());
        REQUIRE(view.is_contiguous());
        REQUIRE(at::equal(view, sources[index]));
        REQUIRE(view.scalar_type() == sources[index].scalar_type());
        REQUIRE(view.sizes() == sources[index].sizes());
    }
    REQUIRE(readback.capacity_bytes() == 0);
    readback.Release();
    readback.Begin();
    REQUIRE_THROWS(readback.Reserve(std::array{at::Tensor{}}));
    readback.Release();
}
TEST_CASE("serialization slots retain simultaneous exact GPU extents and reuse capacity", "[cuda][readback]") {
    const auto policy = readback_test_policy();
    mmltk::backend::ml::cuda::TensorReadbackBuffers readback;
    auto storage = at::arange(128, at::TensorOptions().dtype(at::kFloat).device(at::Device(at::kCUDA, 0)));
    const std::array sources{storage.narrow(0, 7, 32).reshape({4, 8}).transpose(0, 1), storage.narrow(0, 64, 8), storage.narrow(0, 0, 0)};
    readback.Begin();
    readback.Reserve(sources);
    auto first = readback.Stage(0);
    auto second = readback.Stage(1);
    auto empty = readback.Stage(2);
    readback.Complete();
    REQUIRE(first.data_ptr() != second.data_ptr());
    REQUIRE(first.storage().nbytes() == first.nbytes());
    REQUIRE(second.storage().nbytes() == second.nbytes());
    REQUIRE(empty.numel() == 0);
    REQUIRE(at::equal(first, sources[0].cpu()));
    REQUIRE(at::equal(second, sources[1].cpu()));
    REQUIRE_THROWS(readback.Release());
    auto* address = first.data_ptr();
    const auto capacity = readback.capacity_bytes();
    auto detached_reader = first.detach();
    first = at::Tensor{};
    second = at::Tensor{};
    empty = at::Tensor{};
    REQUIRE_THROWS(readback.Release());
    detached_reader = at::Tensor{};
    readback.Release();
    readback.Begin();
    readback.Reserve(std::array{storage.narrow(0, 3, 2)});
    auto small = readback.Stage(0);
    readback.Complete();
    REQUIRE(small.data_ptr() == address);
    REQUIRE(small.storage().nbytes() == small.nbytes());
    REQUIRE(readback.capacity_bytes() == capacity);
    REQUIRE(at::equal(small, at::tensor({3.0F, 4.0F})));
    small = at::Tensor{};
    readback.ReleaseSettled();
    REQUIRE(readback.capacity_bytes() == 0);
}
namespace {
thread_local bool reject_completion = false;
thread_local int remaining_copies = -1;
}  // namespace
extern "C" cudaError_t __real_cudaStreamSynchronize(cudaStream_t);
extern "C" cudaError_t __real_cudaMemcpyAsync(void*, const void*, std::size_t, cudaMemcpyKind, cudaStream_t);
extern "C" cudaError_t __wrap_cudaStreamSynchronize(cudaStream_t stream) {
    const auto status = __real_cudaStreamSynchronize(stream);
    return reject_completion && status == cudaSuccess ? cudaErrorUnknown : status;
}
extern "C" cudaError_t __wrap_cudaMemcpyAsync(void* to, const void* from, std::size_t size, cudaMemcpyKind kind, cudaStream_t stream) {
    if (remaining_copies == 0) return cudaErrorUnknown;
    if (remaining_copies > 0) --remaining_copies;
    return __real_cudaMemcpyAsync(to, from, size, kind, stream);
}
TEST_CASE("readback archive tensors preserve dtype, view flags and distinct active storage", "[cuda][readback][archive]") {
    const auto policy = readback_test_policy();
    mmltk::backend::ml::cuda::TensorReadbackBuffers readback;
    for (const auto dtype : {at::kFloat, at::kHalf, at::kBFloat16, at::kLong, at::kBool, at::kComplexFloat}) {
        auto tensor = at::arange(16, at::kFloat).to(at::Device(at::kCUDA, 0), dtype).reshape({4, 4});
        if (dtype == at::kComplexFloat)
            tensor = tensor.conj();
        else if (dtype != at::kBool)
            tensor = at::_neg_view(tensor);
        const std::array sources{tensor.transpose(0, 1), tensor + 1, at::scalar_tensor(3, at::kLong)};
        readback.Begin();
        readback.Reserve(sources);
        std::stringstream serialized;
        {
            torch::serialize::OutputArchive archive;
            for (std::size_t index = 0; index < sources.size(); ++index) archive.write(std::to_string(index), readback.Stage(index));
            readback.Complete();
            REQUIRE_THROWS(readback.Release());
            archive.save_to(serialized);
        }
        readback.Release();
        torch::serialize::InputArchive archive;
        archive.load_from(serialized);
        for (std::size_t index = 0; index < sources.size(); ++index) {
            at::Tensor loaded;
            archive.read(std::to_string(index), loaded);
            REQUIRE(loaded.scalar_type() == sources[index].scalar_type());
            REQUIRE(loaded.sizes() == sources[index].sizes());
            REQUIRE(at::equal(loaded, sources[index].cpu()));
        }
    }
}
TEST_CASE("failed readback proof retains the actual source and registered receiver after partial enqueue", "[cuda][readback][failure]") {
    const auto policy = readback_test_policy();
    void* allocation = nullptr;
    CUDA_ASSERT_OK(cudaMalloc(&allocation, 16 * sizeof(float)));
    auto released = std::make_shared<std::atomic_bool>(false);
    auto source = at::from_blob(
        allocation, {16},
        [released](void* pointer) {
            *released = true;
            static_cast<void>(cudaFree(pointer));
        },
        at::TensorOptions().device(at::Device(at::kCUDA, 0)).dtype(at::kFloat));
    source.fill_(2.0);
    void* receiver = nullptr;
    {
        mmltk::backend::ml::cuda::TensorReadbackBuffers readback;
        readback.Begin();
        readback.Reserve(std::array{source, source});
        auto first = readback.Stage(0);
        receiver = first.data_ptr();
        remaining_copies = 0;
        reject_completion = true;
        bool failed = false;
        try {
            (void)readback.Stage(1);
        } catch (const std::exception&) { failed = true; }
        remaining_copies = -1;
        reject_completion = false;
        REQUIRE(failed);
        REQUIRE_FALSE(readback.admission_open());
        REQUIRE_THROWS(readback.Begin());
        source = at::Tensor{};
    }
    REQUIRE_FALSE(released->load());
    unsigned flags = 0;
    REQUIRE(cuMemHostGetFlags(&flags, receiver) == CUDA_SUCCESS);
}
