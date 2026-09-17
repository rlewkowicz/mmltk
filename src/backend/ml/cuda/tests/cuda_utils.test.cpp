#include "src/backend/ml/torch/tests/catch_support.h"
#include <cmath>
#include <cstdint>
#include <vector>
#include "src/test_support/cuda_test_utils.hpp"
#include "src/test_support/async_test_utils.hpp"
#include "src/backend/ml/cuda/numa_host_tensor.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include "src/common/system/execution_policy.h"
import mmltk.backend.ml.cuda.gpu_quiescence;
import mmltk.backend.ml.cuda.torch_scope;
TEST_CASE("Torch execution binds a context before pinned host work on an unbound thread", "[cuda][numa][torch-scope]") {
    namespace torch_cuda = mmltk::backend::ml::cuda;
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) SKIP("CUDA unavailable");
    CUDA_ASSERT_OK(cudaSetDevice(0));
    CUcontext original{};
    REQUIRE(cuCtxGetCurrent(&original) == CUDA_SUCCESS);
    REQUIRE(original != nullptr);
    const mmltk::testsupport::ScopedTestCleanup restore_context{[&] { static_cast<void>(cuCtxSetCurrent(original)); }};
    const auto execution = mmltk::frameworks::gpu::test_support::selected_test_device(0, mmltk::common::system::NumaTopology::Capture());
    const auto& p = execution.placement;
    const mmltk::common::system::ScopedExecutionPolicy policy({p.cpus, {}, 0, p.numa_node, -10, false});
    const auto work = [](void* opaque) {
        torch_cuda::NumaHostTensor storage(0);
        auto view = storage.view({4}, at::kFloat);
        unsigned int flags = 0;
        *static_cast<CUresult*>(opaque) = cuMemHostGetFlags(&flags, view.data_ptr());
    };
    for (const bool inference : {false, true}) {
        REQUIRE(cuCtxSetCurrent(nullptr) == CUDA_SUCCESS);
        CUresult registered = CUDA_ERROR_NOT_INITIALIZED;
        if (inference) {
            REQUIRE_NOTHROW(torch_cuda::run_with_torch_cuda_scope({.device = 0, .inference_mode = true}, &registered, work));
        } else {
            REQUIRE_NOTHROW(torch_cuda::run_on_torch_cuda_stream(0, 0U, &registered, work));
        }
        CHECK(registered == CUDA_SUCCESS);
    }
}
TEST_CASE("NUMA tensors retain zero-copy storage and compact active shapes", "[cuda][numa][host-tensor]") {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) SKIP("CUDA unavailable; NUMA host tensor hardware behavior remains unverified");
    CUDA_ASSERT_OK(cudaSetDevice(0));
    const auto execution = mmltk::frameworks::gpu::test_support::selected_test_device(0, mmltk::common::system::NumaTopology::Capture());
    const auto& p = execution.placement;
    mmltk::common::system::ScopedExecutionPolicy policy({p.cpus, {}, 0, p.numa_node, -10, false});
    at::Tensor retained;
    {
        mmltk::backend::ml::cuda::NumaHostTensor owner(0);
        retained = owner.view({1024}, at::kLong);
        retained.fill_(17);
        auto* const first = retained.data_ptr();
        auto small = owner.view({2, 4}, at::kLong);
        REQUIRE(small.is_contiguous());
        REQUIRE(small.data_ptr() == first);
        REQUIRE(small.numel() == 8);
        unsigned int registration_flags = 0U;
        REQUIRE(cuMemHostGetFlags(&registration_flags, small.data_ptr()) == CUDA_SUCCESS);
        auto larger = owner.view({8192}, at::kLong);
        REQUIRE(larger.data_ptr() != first);
        larger.fill_(29);
        REQUIRE(retained[1023].item<std::int64_t>() == 17);
        REQUIRE_THROWS(owner.view({-1}, at::kLong));
    }
    REQUIRE(retained[0].item<std::int64_t>() == 17);
    auto device = retained.to(at::Device(at::kCUDA, 0), at::kLong, true);
    REQUIRE(at::equal(mmltk::backend::ml::cuda::numa_readback(device), retained));
}
