#include "src/frameworks/gpu/device_execution.h"
#include <cuda.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include "src/common/system/cpu_affinity.h"
namespace mmltk::frameworks::gpu {
namespace {
void check(CUresult result, const char* operation) {
    if (result != CUDA_SUCCESS) {
        const char* detail = nullptr;
        (void)cuGetErrorString(result, &detail);
        throw std::runtime_error(std::string(operation) + ": " + (detail ? detail : "CUDA driver failure"));
    }
}
}  // namespace
DeviceExecution resolve_device_execution(int ordinal, const mmltk::common::system::NumaTopology& topology, int numa_node, const std::string& eligible_cpus) {
    const auto eligible = eligible_cpus.empty() ? std::vector<int>{} : mmltk::common::system::resolve_cpu_affinity(eligible_cpus);
    check(cuInit(0), "initialize GPU placement discovery");
    CUdevice device;
    check(cuDeviceGet(&device, ordinal), "resolve actual CUDA-visible device");
    DeviceExecution result;
    result.device = ordinal;
    std::array<char, 32> address{};
    check(cuDeviceGetPCIBusId(address.data(), address.size(), device), "resolve GPU PCI identity");
    result.pci_identity = address.data();
    std::ranges::transform(result.pci_identity, result.pci_identity.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#if CUDA_VERSION >= 12040
    const auto attribute = cuDeviceGetAttribute(&result.reported_numa_node, CU_DEVICE_ATTRIBUTE_HOST_NUMA_ID, device);
    if (attribute != CUDA_SUCCESS && attribute != CUDA_ERROR_INVALID_VALUE && attribute != CUDA_ERROR_NOT_SUPPORTED)
        check(attribute, "resolve GPU host NUMA identity");
    if (attribute != CUDA_SUCCESS) result.reported_numa_node = -1;
#endif
    if (result.reported_numa_node < 0) {
        std::ifstream input("/sys/bus/pci/devices/" + result.pci_identity + "/numa_node");
        if (!(input >> result.reported_numa_node)) result.reported_numa_node = -1;
    }
    try {
        result.placement = mmltk::common::system::resolve_placement(topology, result.reported_numa_node, numa_node, eligible);
    } catch (const std::invalid_argument& error) {
        throw std::invalid_argument("CUDA device " + std::to_string(ordinal) + " (PCI " + result.pci_identity + ") placement: " + error.what() +
                                    "; select --numa-node <GPU-local permitted node>");
    }
    return result;
}
}  // namespace mmltk::frameworks::gpu
