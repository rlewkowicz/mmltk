#include "src/test_support/async_test_utils.hpp"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include <catch2/catch_test_macros.hpp>
#include <cuda.h>
#include <limits>
#include "src/common/system/numa_memory.h"
namespace {
TEST_CASE("Registered local storage preserves portable high-water registration", "[frameworks][gpu][pinned][hardware]") {
 if (cuInit(0) != CUDA_SUCCESS) SKIP("CUDA unavailable; pinned host placement remains unverified");
 int count{};
 REQUIRE(cuDeviceGetCount(&count) == CUDA_SUCCESS);
 if (!count) SKIP("No CUDA-visible device; pinned host placement remains unverified");
 CUdevice device;
 REQUIRE(cuDeviceGet(&device, 0) == CUDA_SUCCESS);
 CUcontext context;
 REQUIRE(cuDevicePrimaryCtxRetain(&context, device) == CUDA_SUCCESS);
 struct PrimaryLease {
  CUdevice device;
  ~PrimaryLease() { (void)cuDevicePrimaryCtxRelease(device); }
 } lease{device};
 const auto selected = mmltk::frameworks::gpu::test_support::selected_test_device(0, mmltk::common::system::NumaTopology::Capture());
 mmltk::frameworks::gpu::PinnedHostBuffer host(
  context, selected.placement, true, +[](void* data, std::size_t bytes, unsigned flags) -> CUresult {
   if (bytes > mmltk::common::system::host_page_size()) return CUDA_ERROR_OUT_OF_MEMORY;
   return cuMemHostRegister(data, bytes, flags);
  });
 host.ensure_bytes(64);
 auto* first = host.data();
 REQUIRE(first);
 const auto check_portable = [&](void* storage) {
  REQUIRE(cuCtxPushCurrent(context) == CUDA_SUCCESS);
  bool bound = true;
  mmltk::testsupport::ScopedTestCleanup pop_context{[&] {
   if (bound) {
    CUcontext ignored{};
    (void)cuCtxPopCurrent(&ignored);
   }
  }};
  unsigned flags{};
  REQUIRE(cuMemHostGetFlags(&flags, storage) == CUDA_SUCCESS);
  CHECK((flags & CU_MEMHOSTREGISTER_PORTABLE) != 0);
  CUcontext popped{};
  REQUIRE(cuCtxPopCurrent(&popped) == CUDA_SUCCESS);
  bound = false;
  CHECK(popped == context);
 };
 check_portable(first);
 host.ensure_bytes(32);
 CHECK(host.data() == first);
 CHECK(host.node() == selected.placement.numa_node);
 CHECK_THROWS_AS(host.ensure_bytes(std::numeric_limits<std::size_t>::max()), std::bad_alloc);
 CHECK(host.data() == first);
 const auto capacity = host.capacity_bytes();
 CHECK_THROWS_AS(host.ensure_bytes(capacity + 1U), std::runtime_error);
 CHECK(host.data() == first);
 CHECK(host.capacity_bytes() == capacity);
 check_portable(first);
 REQUIRE(host.ReleaseSettled() == CUDA_SUCCESS);
 CHECK(host.data() == nullptr);
}
}  // namespace
