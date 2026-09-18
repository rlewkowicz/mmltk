#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <cuda.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include "src/common/system/cpu_affinity.h"
namespace {
TEST_CASE("CUDA device enumeration distinguishes unavailable capability from runtime failures", "[frameworks][gpu]") {
    using mmltk::testsupport::classify_cuda_device_count;
    for (const int count : {0, 1, 2}) {
        const auto result = classify_cuda_device_count(cudaSuccess, count);
        REQUIRE(result.has_value());
        CHECK(*result == count);
    }
    for (const auto status : {cudaErrorNoDevice, cudaErrorInsufficientDriver}) {
        const auto result = classify_cuda_device_count(status, 2);
        REQUIRE(result.has_value());
        CHECK(*result == 0);
    }
    for (const auto status : {cudaErrorInitializationError, cudaErrorUnknown, cudaErrorMemoryAllocation}) {
        const auto result = classify_cuda_device_count(status, 2);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == status);
    }
    const auto invalid_count = classify_cuda_device_count(cudaSuccess, -1);
    REQUIRE_FALSE(invalid_count.has_value());
    CHECK(invalid_count.error() == cudaSuccess);
}
TEST_CASE("Device placement rejects every forbidden explicit CPU before locality filtering", "[frameworks][gpu][placement]") {
    using namespace mmltk::common::system;
    const auto topology = NumaTopology::Capture();
    const auto allowed = allowed_cpu_set();
    int forbidden = 0;
    while (std::binary_search(allowed.begin(), allowed.end(), forbidden)) ++forbidden;
    const auto requested = std::to_string(allowed.front()) + "," + std::to_string(forbidden);
    try {
        (void)mmltk::frameworks::gpu::resolve_device_execution(-1, topology, -1, requested);
        FAIL("forbidden explicit CPU was accepted");
    } catch (const std::runtime_error& error) { CHECK(std::string_view(error.what()).find("outside the current allowed cpuset") != std::string_view::npos); }
}
TEST_CASE("CUDA-visible placement resolves the device PCI identity", "[frameworks][gpu][placement][hardware]") {
    if (cuInit(0) != CUDA_SUCCESS) SKIP("CUDA driver unavailable; device placement hardware remains unverified");
    int count = 0;
    REQUIRE(cuDeviceGetCount(&count) == CUDA_SUCCESS);
    if (!count) SKIP("No CUDA-visible device; placement hardware remains unverified");
    const auto topology = mmltk::common::system::NumaTopology::Capture();
    for (int ordinal = 0; ordinal < count; ++ordinal) {
        CUdevice device;
        REQUIRE(cuDeviceGet(&device, ordinal) == CUDA_SUCCESS);
        std::array<char, 32> pci{};
        REQUIRE(cuDeviceGetPCIBusId(pci.data(), pci.size(), device) == CUDA_SUCCESS);
        std::string expected(pci.data());
        std::ranges::transform(expected, expected.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto selected = mmltk::frameworks::gpu::test_support::selected_test_device(ordinal, topology);
        CHECK(selected.pci_identity == expected);
        CHECK(selected.device == ordinal);
        CHECK(selected.placement.numa_node >= 0);
        REQUIRE_FALSE(selected.placement.cpus.empty());
        const auto constrained = mmltk::frameworks::gpu::resolve_device_execution(ordinal, topology, selected.placement.numa_node,
                                                                                  mmltk::common::system::format_cpu_list(topology.permitted_cpus));
        CHECK(constrained.placement.cpus == selected.placement.cpus);
    }
}
TEST_CASE("Graphics UUID resolution preserves CUDA-visible identity across device order", "[frameworks][gpu][hardware][device_uuid]") {
    if (cuInit(0) != CUDA_SUCCESS) SKIP("CUDA driver unavailable; graphics device mapping remains unverified");
    int count = 0;
    REQUIRE(cuDeviceGetCount(&count) == CUDA_SUCCESS);
    if (!count) SKIP("No CUDA-visible device; graphics device mapping remains unverified");
    for (int ordinal = count - 1; ordinal >= 0; --ordinal) {
        CUdevice device;
        CUuuid uuid{};
        REQUIRE(cuDeviceGet(&device, ordinal) == CUDA_SUCCESS);
        REQUIRE(cuDeviceGetUuid(&uuid, device) == CUDA_SUCCESS);
        std::array<std::uint8_t, 16U> bytes{};
        std::memcpy(bytes.data(), uuid.bytes, bytes.size());
        CHECK(mmltk::frameworks::gpu::resolve_device_uuid(bytes) == ordinal);
    }
    CHECK_THROWS_WITH(mmltk::frameworks::gpu::resolve_device_uuid({}), "workspace graphics device UUID is not visible to CUDA");
}
}  // namespace
