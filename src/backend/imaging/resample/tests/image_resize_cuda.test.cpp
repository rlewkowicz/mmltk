// SPDX-License-Identifier: MIT
// Öztireli/Gross (2015) perceptual downscaling; independent oracle provenance in perceptual_downscale_reference.h.
#include "src/backend/imaging/resample/image_resize_cuda.h"
#include "src/backend/imaging/resample/detail/perceptual_downscale_completion.h"
#include "src/backend/imaging/resample/tests/perceptual_downscale_reference.h"
#include "src/backend/imaging/resample/tests/perceptual_downscale_traversal.h"
#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/imported_image_buffer.h"
#include "src/frameworks/gpu/tests/vulkan_workspace_fixture.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_authority.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include "src/common/system/execution_policy.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
namespace mmltk::backend::imaging::resample {
namespace {
using namespace test_perceptual;
namespace gpu = frameworks::gpu;
void cuda_check(cudaError_t status) {
    if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
}
bool has_cuda() {
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}
struct DeviceImage {
    RgbImageLayout layout;
    void* pixels = nullptr;
    CUcontext context = nullptr;
    explicit DeviceImage(RgbImageLayout image_layout) : layout(image_layout) {
        if (cuCtxGetCurrent(&context) != CUDA_SUCCESS || !context) throw std::runtime_error("missing device image context");
        cuda_check(cudaMalloc(&pixels, layout.capacity_bytes));
    }
    ~DeviceImage() {
        if (!pixels || cuCtxPushCurrent(context) != CUDA_SUCCESS) return;
        (void)cudaFree(pixels);
        CUcontext popped = nullptr;
        (void)cuCtxPopCurrent(&popped);
    }
    DeviceImage(const DeviceImage&) = delete;
    DeviceImage& operator=(const DeviceImage&) = delete;
    RgbConstImageView read() const { return {pixels, layout}; }
    RgbMutableImageView write() { return {pixels, layout}; }
};
struct ForeignStream {
    gpu::DeviceContext context;
    cudaStream_t stream = nullptr;
    explicit ForeignStream(const gpu::DeviceExecution& execution)
        : context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, -1, execution) {
        context.Bind();
        cuda_check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    }
    ~ForeignStream() {
        if (stream) {
            context.Bind();
            (void)cudaStreamDestroy(stream);
        }
    }
};
struct CudaFixture {
    gpu::DeviceExecution execution;
    std::unique_ptr<common::system::ScopedExecutionPolicy> policy;
    std::unique_ptr<gpu::DeviceContext> context_owner;
    std::unique_ptr<gpu::PinnedHostBuffer> staging;
    cudaStream_t stream = nullptr, other = nullptr;
    cudaEvent_t readback = nullptr;
    gpu::TerminalCudaRetirementOwner retirement;
    explicit CudaFixture(std::size_t retirement_capacity = 1) : retirement(retirement_capacity) {
        try {
            cuda_check(cudaSetDevice(0));
            execution = gpu::test_support::selected_test_device(0, common::system::NumaTopology::Capture());
            const auto& p = execution.placement;
            policy = std::make_unique<common::system::ScopedExecutionPolicy>(
                common::system::ExecutionPolicyRequest{p.cpus, "perceptual-test", 0, p.numa_node, -10, false});
            context_owner = std::make_unique<gpu::DeviceContext>(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::PrimaryInterop, -1, execution);
            context_owner->Bind();
            CUcontext context = nullptr;
            if (cuCtxGetCurrent(&context) != CUDA_SUCCESS || !context) throw std::runtime_error("perceptual test requires the CUDA primary context");
            staging = std::make_unique<gpu::PinnedHostBuffer>(context, p);
            staging->ensure_bytes(1024U * 1024U);
            cuda_check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
            cuda_check(cudaStreamCreateWithFlags(&other, cudaStreamNonBlocking));
            cuda_check(cudaEventCreateWithFlags(&readback, cudaEventDisableTiming));
        } catch (...) {
            close();
            throw;
        }
    }
    void close() noexcept {
        if (stream) (void)cudaStreamSynchronize(stream);
        if (other) (void)cudaStreamSynchronize(other);
        if (staging) (void)staging->ReleaseSettled();
        if (readback) (void)cudaEventDestroy(readback);
        if (other) (void)cudaStreamDestroy(other);
        if (stream) (void)cudaStreamDestroy(stream);
        readback = nullptr;
        other = nullptr;
        stream = nullptr;
    }
    ~CudaFixture() { close(); }
    std::shared_ptr<DeviceImage> upload(const Image& image) {
        auto device = std::make_shared<DeviceImage>(image.layout);
        REQUIRE(image.layout.capacity_bytes <= staging->capacity_bytes());
        std::memcpy(staging->data(), image.storage.data(), image.layout.capacity_bytes);
        cuda_check(cudaMemcpyAsync(device->pixels, staging->data(), image.layout.capacity_bytes, cudaMemcpyHostToDevice, stream));
        // Staging is reused by the fixture after this exact copy completes.
        cuda_check(cudaEventRecord(readback, stream));
        cuda_check(cudaEventSynchronize(readback));
        return device;
    }
    Image download(const std::shared_ptr<DeviceImage>& image, cudaStream_t from) {
        Image host(image->layout.width, image->layout.height, image->layout.format);
        host.layout = image->layout;
        host.storage.resize((host.layout.capacity_bytes + 3) / 4);
        REQUIRE(host.layout.capacity_bytes <= staging->capacity_bytes());
        cuda_check(cudaMemcpyAsync(staging->data(), image->pixels, host.layout.capacity_bytes, cudaMemcpyDeviceToHost, from));
        cuda_check(cudaEventRecord(readback, from));
        cuda_check(cudaEventSynchronize(readback));
        std::memcpy(host.storage.data(), staging->data(), host.layout.capacity_bytes);
        return host;
    }
};
// Fake only the actual completion owner's CUDA calls. Production performs no
// global test dispatch or diagnostics. Each retained pair is checked by weak
// ownership and exact event/stream identity.
struct FakeCompletion {
    struct Event {
        bool complete = false;
    };
    std::vector<std::unique_ptr<Event>> events;
    cudaError_t record_status = cudaSuccess, wait_status = cudaSuccess, create_status = cudaSuccess, query_status = cudaSuccess;
    unsigned stream_waits = 0, orders = 0, destroys = 0;
    cudaStream_t waited = nullptr;
    static inline thread_local FakeCompletion* active = nullptr;
    FakeCompletion() { active = this; }
    ~FakeCompletion() { active = nullptr; }
    perceptual::CompletionApi api() {
        return {+[](cudaEvent_t* out, unsigned) -> cudaError_t {
                    if (active->create_status != cudaSuccess) return active->create_status;
                    active->events.push_back(std::make_unique<Event>());
                    *out = reinterpret_cast<cudaEvent_t>(active->events.back().get());
                    return cudaSuccess;
                },
                +[](cudaEvent_t event, cudaStream_t) -> cudaError_t {
                    if (active->record_status == cudaSuccess) reinterpret_cast<Event*>(event)->complete = false;
                    return active->record_status;
                },
                +[](cudaEvent_t event) -> cudaError_t {
                    if (active->query_status != cudaSuccess) return active->query_status;
                    return reinterpret_cast<Event*>(event)->complete ? cudaSuccess : cudaErrorNotReady;
                },
                +[](cudaEvent_t event) -> cudaError_t {
                    if (active->wait_status == cudaSuccess) reinterpret_cast<Event*>(event)->complete = true;
                    return active->wait_status;
                },
                +[](cudaStream_t stream) -> cudaError_t {
                    ++active->stream_waits;
                    active->waited = stream;
                    return active->wait_status;
                },
                +[](cudaStream_t, cudaEvent_t, unsigned) -> cudaError_t {
                    ++active->orders;
                    return cudaSuccess;
                },
                +[](cudaEvent_t) -> cudaError_t {
                    ++active->destroys;
                    return cudaSuccess;
                }};
    }
};
}  // namespace
TEST_CASE("CUDA perceptual serial traversal retains every accumulator bit", "[backend][data][image_resize][perceptual][cuda]") {
    if (!has_cuda()) SKIP("CUDA unavailable; exact traversal acceptance is not established");
    CudaFixture fixture;
    cudaDeviceProp properties{};
    cuda_check(cudaGetDeviceProperties(&properties, 0));
    INFO("CUDA device 0: " << properties.name << " CC " << properties.major << "." << properties.minor);
    constexpr std::array cases{
        std::array{1U, 19U, 1U, 7U}, std::array{19U, 1U, 7U, 1U}, std::array{17U, 13U, 7U, 5U},
        std::array{32U, 24U, 8U, 6U}, std::array{14U, 14U, 2U, 2U}, std::array{15U, 14U, 2U, 2U},
        std::array{16U, 16U, 2U, 2U}, std::array{257U, 5U, 19U, 3U}, std::array{67U, 61U, 1U, 1U}};
    for (auto format : formats)
        for (const auto& dims : cases)
            for (unsigned pattern : {0U, 5U, 6U, 7U}) {
                INFO("format " << static_cast<int>(format) << " source " << dims[0] << "x" << dims[1]
                               << " destination " << dims[2] << "x" << dims[3] << " pattern " << pattern);
                // Offset subview retains padded rows and independent plane slices.
                Image source(dims[0] + 2, dims[1] + 2, format, 7);
                source.fill(pattern);
                if (format == RgbPixelFormat::PlanarUnitSrgbF32) {
                    source.set(1, 1, 0, std::numeric_limits<double>::quiet_NaN());
                    source.set(1, 1, 1, std::numeric_limits<double>::infinity());
                    source.set(1, 1, 2, -1);
                }
                auto input = fixture.upload(source);
                auto view = input->read();
                const auto offset = view.layout.row_stride_bytes + (format == RgbPixelFormat::RGB8 ? 3U : 4U);
                view.data = static_cast<const std::uint8_t*>(view.data) + offset;
                view.layout.width = dims[0];
                view.layout.height = dims[1];
                view.layout.capacity_bytes -= offset;
                const auto count = std::size_t(dims[2]) * dims[3];
                const auto bytes = 2 * count * sizeof(TraversalMoments);
                auto result_layout = input->layout;
                result_layout.capacity_bytes = bytes;
                auto results = std::make_shared<DeviceImage>(result_layout);
                cuda_check(compare_traversal(view, dims[2], dims[3], static_cast<TraversalMoments*>(results->pixels), fixture.stream));
                cuda_check(cudaMemcpyAsync(fixture.staging->data(), results->pixels, bytes, cudaMemcpyDeviceToHost, fixture.stream));
                cuda_check(cudaEventRecord(fixture.readback, fixture.stream));
                cuda_check(cudaEventSynchronize(fixture.readback));
                const auto* actual = static_cast<const TraversalMoments*>(fixture.staging->data());
                for (std::size_t i = 0; i < count; ++i)
                    for (unsigned field = 0; field < 8; ++field) {
                        INFO("footprint " << i << " moment field " << field);
                        REQUIRE(actual[2 * i].bits[field] == actual[2 * i + 1].bits[field]);
                    }
            }
}
TEST_CASE("CUDA perceptual allocation-local preparation matches fresh owners", "[backend][data][image_resize][perceptual][cuda]") {
    if (!has_cuda()) SKIP("CUDA unavailable; preparation reuse acceptance is not established");
    CudaFixture fixture(2);
    GpuPerceptualDownscaler retained(*fixture.context_owner, fixture.retirement);
    constexpr std::array sequence{RgbPixelFormat::PlanarUnitSrgbF32, RgbPixelFormat::RGB8, RgbPixelFormat::RGBA8,
                                  RgbPixelFormat::PlanarUnitSrgbF32, RgbPixelFormat::RGB8};
    for (const auto& dims : {std::array{19U, 15U, 9U, 7U}, std::array{19U, 15U, 9U, 7U},
                            std::array{7U, 5U, 3U, 2U}, std::array{129U, 127U, 65U, 63U}}) {
        struct PreparedCase {
            Image source;
            std::shared_ptr<DeviceImage> input, output, expected;
        };
        std::vector<PreparedCase> prepared;
        for (auto format : sequence) {
            Image source(dims[0], dims[1], format, 7), shape(dims[2], dims[3], format, 11);
            source.fill(7);
            auto input = fixture.upload(source), output = std::make_shared<DeviceImage>(shape.layout), expected = std::make_shared<DeviceImage>(shape.layout);
            cuda_check(cudaMemsetAsync(output->pixels, 0xCD, output->layout.capacity_bytes, fixture.stream));
            cuda_check(cudaMemsetAsync(expected->pixels, 0xCD, expected->layout.capacity_bytes, fixture.stream));
            prepared.push_back({std::move(source), std::move(input), std::move(output), std::move(expected)});
        }
        cuda_check(cudaEventRecord(fixture.readback, fixture.stream));
        cuda_check(cudaEventSynchronize(fixture.readback));
        // Submit the entire format transition sequence without settling the
        // owner. Cross-stream dependencies must cover pending preparation.
        for (std::size_t i = 0; i < prepared.size(); ++i) {
            auto& item = prepared[i];
            retained.downscale(item.input->read(), item.output->write(), i % 2 ? fixture.other : fixture.stream, item.input, item.output);
        }
        retained.finish();
        for (auto& item : prepared) {
            GpuPerceptualDownscaler fresh(*fixture.context_owner, fixture.retirement);
            fresh.downscale(item.input->read(), item.expected->write(), fixture.stream, item.input, item.expected);
            fresh.finish();
            const auto actual = fixture.download(item.output, fixture.stream);
            const auto oracle = fixture.download(item.expected, fixture.stream);
            REQUIRE(std::memcmp(actual.storage.data(), oracle.storage.data(), item.output->layout.capacity_bytes) == 0);
            REQUIRE(padding_intact(actual));
            REQUIRE(maximum_error(actual, reference(item.source, dims[2], dims[3])) <= reference_tolerance(item.source.layout.format));
        }
    }
}
TEST_CASE("CUDA perceptual settled preparation survives admission failure and retry", "[backend][data][image_resize][perceptual][cuda]") {
    if (!has_cuda()) SKIP("CUDA unavailable; settled preparation retry acceptance is not established");
    CudaFixture fixture(2);
    for (auto format : {RgbPixelFormat::RGB8, RgbPixelFormat::RGBA8}) {
        INFO("format " << static_cast<int>(format));
        Image source(19, 15, format, 7), shape(9, 7, format, 11);
        source.fill(7);
        auto input = fixture.upload(source), output = std::make_shared<DeviceImage>(shape.layout), expected = std::make_shared<DeviceImage>(shape.layout);
        cuda_check(cudaMemsetAsync(output->pixels, 0xCD, output->layout.capacity_bytes, fixture.stream));
        cuda_check(cudaMemsetAsync(expected->pixels, 0xCD, expected->layout.capacity_bytes, fixture.stream));
        GpuPerceptualDownscaler retained(*fixture.context_owner, fixture.retirement);
        retained.downscale(input->read(), output->write(), fixture.stream, input, output);
        // Complete the recorded byte-table submission externally. The owner
        // has not settled its submission, as its retained custody demonstrates.
        cuda_check(cudaEventRecord(fixture.readback, fixture.stream));
        cuda_check(cudaEventSynchronize(fixture.readback));
        REQUIRE(input.use_count() == 2);
        REQUIRE(output.use_count() == 2);
        // Host storage passes layout/custody validation, then fails span
        // admission after the public call has settled the completed submission.
        REQUIRE_THROWS_AS(retained.downscale(source.read(), output->write(), fixture.other, input, output), std::invalid_argument);
        REQUIRE(input.use_count() == 1);
        REQUIRE(output.use_count() == 1);
        REQUIRE_FALSE(fixture.retirement.fact().terminal);
        retained.downscale(input->read(), output->write(), fixture.other, input, output);
        retained.finish();
        GpuPerceptualDownscaler fresh(*fixture.context_owner, fixture.retirement);
        fresh.downscale(input->read(), expected->write(), fixture.stream, input, expected);
        fresh.finish();
        const auto actual = fixture.download(output, fixture.other);
        const auto oracle = fixture.download(expected, fixture.stream);
        REQUIRE(std::memcmp(actual.storage.data(), oracle.storage.data(), shape.layout.capacity_bytes) == 0);
        REQUIRE(padding_intact(actual));
        REQUIRE(maximum_error(actual, reference(source, 9, 7)) <= reference_tolerance(format));
        REQUIRE_FALSE(fixture.retirement.fact().terminal);
    }
}
TEST_CASE("CUDA perceptual resampling agrees with independent CPU geometry and color", "[backend][data][image_resize][perceptual][cuda]") {
    if (!has_cuda()) SKIP("CUDA unavailable; device acceptance is not established");
    CudaFixture fixture;
    GpuPerceptualDownscaler resizer(*fixture.context_owner, fixture.retirement);
    RgbImageResizer cpu;
    for (auto format : formats)
        for (const auto& dims : geometries)
            for (unsigned pattern : {0U, 3U, 5U, 6U, 7U}) {
                INFO("format " << static_cast<int>(format) << " source " << dims[0] << "x" << dims[1] << " destination " << dims[2] << "x" << dims[3]
                               << " pattern " << pattern);
                Image source(dims[0], dims[1], format, 3), expected(dims[2], dims[3], format, 5);
                source.fill(pattern);
                auto input = fixture.upload(source), output = std::make_shared<DeviceImage>(expected.layout);
                cuda_check(cudaMemsetAsync(output->pixels, 0xCD, output->layout.capacity_bytes, fixture.stream));
                resizer.downscale(input->read(), output->write(), fixture.stream, input, output);
                auto actual = fixture.download(output, fixture.stream);
                cpu.downscale(source.read(), expected.write());
                const double tolerance = reference_tolerance(format);
                REQUIRE(maximum_error(actual, expected) <= tolerance);
                REQUIRE(maximum_error(actual, reference(source, dims[2], dims[3])) <= tolerance);
                REQUIRE(padding_intact(actual));
                // Same prepared geometry and workspace, ordered onto a second stream.
                resizer.downscale(input->read(), output->write(), fixture.other, input, output);
                REQUIRE(maximum_error(actual, fixture.download(output, fixture.other)) <= tolerance);
                resizer.finish();
            }
    REQUIRE_FALSE(fixture.retirement.fact().terminal);
}
TEST_CASE("CUDA perceptual custody survives caller release and teardown", "[backend][data][image_resize][perceptual][cuda]") {
    if (!has_cuda()) SKIP("CUDA unavailable; device lifetime acceptance is not established");
    CudaFixture fixture;
    Image source(129, 127, RgbPixelFormat::RGBA8);
    source.fill(5);
    Image shape(2, 3, RgbPixelFormat::RGBA8);
    auto input = fixture.upload(source), output = std::make_shared<DeviceImage>(shape.layout);
    std::weak_ptr<DeviceImage> weak = input;
    {
        GpuPerceptualDownscaler resizer(*fixture.context_owner, fixture.retirement);
        resizer.downscale(input->read(), output->write(), fixture.stream, input, output);
        input.reset();
        REQUIRE_FALSE(weak.expired());
        // Destruction is the cancellation boundary: already-issued work
        // completes before resources are released, even without finish().
    }
    REQUIRE(weak.expired());
    REQUIRE(maximum_error(fixture.download(output, fixture.stream), reference(source, 2, 3)) <= 1.0 / 255 + 1e-12);
}
TEST_CASE("CUDA perceptual identity preserves aliases and padded copies in every format", "[backend][data][image_resize][perceptual][cuda]") {
    if (!has_cuda()) SKIP("CUDA unavailable; identity-copy acceptance is not established");
    CudaFixture fixture;
    GpuPerceptualDownscaler resizer(*fixture.context_owner, fixture.retirement);
    for (auto format : formats) {
        Image source(17, 13, format, 3), shape(17, 13, format, 5);
        source.fill(7);
        auto input = fixture.upload(source), output = std::make_shared<DeviceImage>(shape.layout);
        resizer.downscale(input->read(), input->write(), fixture.stream, input, input);
        const auto alias = fixture.download(input, fixture.stream);
        REQUIRE(std::memcmp(alias.storage.data(), source.storage.data(), source.layout.capacity_bytes) == 0);
        cuda_check(cudaMemsetAsync(output->pixels, 0xCD, output->layout.capacity_bytes, fixture.stream));
        resizer.downscale(input->read(), output->write(), fixture.stream, input, output);
        const auto actual = fixture.download(output, fixture.stream);
        REQUIRE(maximum_error(actual, source) == 0);
        REQUIRE(padding_intact(actual));
        resizer.finish();
    }
}
TEST_CASE("CUDA perceptual central variance keeps both threshold branches", "[backend][data][image_resize][perceptual][cuda]") {
    if (!has_cuda()) SKIP("CUDA unavailable; numerical acceptance is not established");
    CudaFixture fixture;
    GpuPerceptualDownscaler resizer(*fixture.context_owner, fixture.retirement);
    for (double delta : {0.001999, 0.002001}) {
        auto source = threshold_source(34, 6, delta);
        Image shape(17, 3, RgbPixelFormat::PlanarUnitSrgbF32);
        auto input = fixture.upload(source), output = std::make_shared<DeviceImage>(shape.layout);
        resizer.downscale(input->read(), output->write(), fixture.stream, input, output);
        REQUIRE(maximum_error(fixture.download(output, fixture.stream), reference(source, 17, 3)) < 2e-6);
        resizer.finish();
    }
}
TEST_CASE("CUDA perceptual finish settles admitted work after external admission closes", "[backend][data][image_resize][perceptual][cuda]") {
    if (!has_cuda()) SKIP("CUDA unavailable; terminal settlement is not established");
    CudaFixture fixture(2);
    auto independent = gpu::ReserveTerminalCudaLease(fixture.retirement);
    GpuPerceptualDownscaler resizer(*fixture.context_owner, fixture.retirement);
    Image source(129, 127, RgbPixelFormat::RGB8), shape(9, 7, RgbPixelFormat::RGB8);
    source.fill(5);
    auto input = fixture.upload(source), output = std::make_shared<DeviceImage>(shape.layout);
    std::weak_ptr<DeviceImage> source_weak = input;
    const auto source_view = input->read();
    resizer.downscale(source_view, output->write(), fixture.stream, input, output);
    input.reset();
    REQUIRE_FALSE(source_weak.expired());
    auto independent_custody = std::make_shared<int>(1);
    std::move(independent).Install(gpu::TerminalCudaCustody::Share(std::move(independent_custody)), cudaErrorUnknown);
    REQUIRE_FALSE(fixture.retirement.admission_open());
    REQUIRE_THROWS(resizer.downscale(source_view, output->write(), fixture.stream, source_weak.lock(), output));
    REQUIRE_FALSE(source_weak.expired());
    REQUIRE_NOTHROW(resizer.finish());
    REQUIRE(source_weak.expired());
    REQUIRE(fixture.retirement.fact().occupancy == 1);
    REQUIRE(maximum_error(fixture.download(output, fixture.stream), reference(source, 9, 7)) <= 1.0 / 255 + 1e-12);
}
TEST_CASE("CUDA perceptual extent refusal leaves output and owner reusable", "[backend][data][image_resize][perceptual][cuda]") {
    if (!has_cuda()) SKIP("CUDA unavailable");
    CudaFixture fixture;
    Image source(17, 13, RgbPixelFormat::RGB8), shape(7, 5, RgbPixelFormat::RGB8);
    source.fill(1);
    auto input = fixture.upload(source), output = std::make_shared<DeviceImage>(shape.layout);
    GpuPerceptualDownscaler resizer(*fixture.context_owner, fixture.retirement);
    auto bad = output->write();
    bad.layout.capacity_bytes = 1;
    REQUIRE_THROWS(resizer.downscale(input->read(), bad, fixture.stream, input, output));
    REQUIRE_THROWS(resizer.downscale(input->read(), output->write(), fixture.stream, {}, output));
    resizer.downscale(input->read(), output->write(), fixture.stream, input, output);
    resizer.finish();
    REQUIRE(maximum_error(fixture.download(output, fixture.stream), reference(source, 7, 5)) <= 1.0 / 255 + 1e-12);
}
TEST_CASE("CUDA perceptual admission proves device intervals and stream identity", "[backend][data][image_resize][perceptual][cuda]") {
    if (!has_cuda()) SKIP("CUDA unavailable; range admission is not established");
    CudaFixture fixture;
    Image source(19, 15, RgbPixelFormat::RGB8), shape(9, 7, RgbPixelFormat::RGB8);
    source.fill(5);
    auto input = fixture.upload(source), output = std::make_shared<DeviceImage>(shape.layout);
    GpuPerceptualDownscaler resizer(*fixture.context_owner, fixture.retirement);
    cuda_check(cudaMemsetAsync(output->pixels, 0xCD, output->layout.capacity_bytes, fixture.stream));
    const auto untouched = fixture.download(output, fixture.stream).storage;
    const auto reject = [&](RgbConstImageView source_view, RgbMutableImageView output_view, cudaStream_t stream,
                            const std::shared_ptr<const void>& source_owner) {
        const auto source_refs = source_owner.use_count(), output_refs = output.use_count();
        REQUIRE_THROWS(resizer.downscale(source_view, output_view, stream, source_owner, output));
        REQUIRE(source_owner.use_count() == source_refs);
        REQUIRE(output.use_count() == output_refs);
        REQUIRE_FALSE(fixture.retirement.fact().terminal);
        REQUIRE(fixture.download(output, fixture.stream).storage == untouched);
    };
    reject(source.read(), output->write(), fixture.stream, input);  // Host memory is never a device view.
    auto tiny_layout = source.layout;
    tiny_layout.capacity_bytes = 1;
    auto tiny = std::make_shared<DeviceImage>(tiny_layout);
    reject({tiny->pixels, source.layout}, output->write(), fixture.stream, tiny);
    CUdeviceptr allocation_base = 0;
    std::size_t allocation_bytes = 0;
    REQUIRE(cuMemGetAddressRange(&allocation_base, &allocation_bytes, reinterpret_cast<CUdeviceptr>(input->pixels)) == CUDA_SUCCESS);
    auto crossing = input->read();
    crossing.data = reinterpret_cast<const void*>(allocation_base + allocation_bytes - 1);
    reject(crossing, output->write(), fixture.stream, input);
    auto output_crossing = output->write();
    REQUIRE(cuMemGetAddressRange(&allocation_base, &allocation_bytes, reinterpret_cast<CUdeviceptr>(output->pixels)) == CUDA_SUCCESS);
    output_crossing.data = reinterpret_cast<void*>(allocation_base + allocation_bytes - 1);
    reject(input->read(), output_crossing, fixture.stream, input);
    {
        ForeignStream foreign(fixture.execution);
        auto foreign_input = std::make_shared<DeviceImage>(source.layout);
        REQUIRE_THROWS_AS(resizer.finish(), std::invalid_argument);
        REQUIRE_FALSE(fixture.retirement.fact().terminal);
        fixture.context_owner->Bind();
        reject(foreign_input->read(), output->write(), fixture.stream, foreign_input);
        reject(input->read(), output->write(), foreign.stream, input);
        // The foreign allocation is destroyed on its own retained context.
    }
    fixture.context_owner->Bind();
    auto source_subview = input->read();
    const auto source_offset = source.layout.row_stride_bytes + 3;
    source_subview.data = static_cast<const std::uint8_t*>(input->pixels) + source_offset;
    source_subview.layout.width = 17;
    source_subview.layout.height = 13;
    source_subview.layout.capacity_bytes -= source_offset;
    auto destination_subview = output->write();
    const auto destination_offset = shape.layout.row_stride_bytes + 3;
    destination_subview.data = static_cast<std::uint8_t*>(output->pixels) + destination_offset;
    destination_subview.layout.width = 7;
    destination_subview.layout.height = 5;
    destination_subview.layout.capacity_bytes -= destination_offset;
    // Null stream is valid under the already-bound exact context.
    resizer.downscale(source_subview, destination_subview, nullptr, input, output);
    resizer.finish();
    Image cropped_source(17, 13, RgbPixelFormat::RGB8), cropped_result(7, 5, RgbPixelFormat::RGB8);
    const auto actual = fixture.download(output, nullptr);
    for (unsigned y = 0; y < 13; ++y)
        for (unsigned x = 0; x < 17; ++x)
            for (unsigned k = 0; k < 3; ++k) cropped_source.set(x, y, k, source.at(x + 1, y + 1, k));
    for (unsigned y = 0; y < 5; ++y)
        for (unsigned x = 0; x < 7; ++x)
            for (unsigned k = 0; k < 3; ++k) cropped_result.set(x, y, k, actual.at(x + 1, y + 1, k));
    REQUIRE(maximum_error(cropped_result, reference(cropped_source, 7, 5)) <= 1.0 / 255 + 1e-12);
    void* pool_pointer = nullptr;
    cuda_check(cudaMallocAsync(&pool_pointer, source.layout.capacity_bytes, fixture.stream));
    std::shared_ptr<void> pooled(pool_pointer, [context = *fixture.context_owner](void* pointer) {
        context.Bind();
        (void)cudaFree(pointer);
    });
    cuda_check(cudaMemcpyAsync(pool_pointer, input->pixels, source.layout.capacity_bytes, cudaMemcpyDeviceToDevice, fixture.stream));
    resizer.downscale({pool_pointer, source.layout}, output->write(), fixture.stream, pooled, output);
    resizer.finish();
    REQUIRE(maximum_error(fixture.download(output, fixture.stream), reference(source, 9, 7)) <= 1.0 / 255 + 1e-12);
    REQUIRE(cuMemGetAddressRange(&allocation_base, &allocation_bytes, reinterpret_cast<CUdeviceptr>(pool_pointer)) == CUDA_SUCCESS);
    crossing.data = reinterpret_cast<const void*>(allocation_base + allocation_bytes - 1);
    // Pool ownership admits the real allocation, never its adjacent virtual span.
    REQUIRE_THROWS_AS(resizer.downscale(crossing, output->write(), fixture.stream, pooled, output), std::invalid_argument);
    source_subview.data = static_cast<const std::uint8_t*>(pool_pointer) + source_offset;
    resizer.downscale(source_subview, destination_subview, fixture.stream, pooled, output);
    resizer.finish();
}
TEST_CASE("perceptual completion retains exact custody after failed recording and settlement", "[backend][data][image_resize][perceptual]") {
    FakeCompletion fake;
    perceptual::CudaDownscaleCompletion completion(fake.api());
    auto source = std::make_shared<int>(1), destination = std::make_shared<int>(2);
    std::weak_ptr<int> source_weak = source, destination_weak = destination;
    auto stream = reinterpret_cast<cudaStream_t>(std::uintptr_t{1});
    auto& slot = completion.reserve(stream, source, destination);
    source.reset();
    destination.reset();
    fake.record_status = cudaErrorInvalidResourceHandle;
    REQUIRE(completion.record(slot) == cudaErrorInvalidResourceHandle);
    fake.wait_status = cudaErrorUnknown;
    REQUIRE(completion.settle(true) == cudaErrorUnknown);
    REQUIRE_FALSE(source_weak.expired());
    REQUIRE_FALSE(destination_weak.expired());
    REQUIRE(fake.waited == stream);
    REQUIRE(fake.destroys == 0);
    fake.wait_status = cudaSuccess;
    REQUIRE(completion.settle(true) == cudaSuccess);
    REQUIRE(source_weak.expired());
    REQUIRE(destination_weak.expired());
    REQUIRE(completion.release() == cudaSuccess);
    REQUIRE(fake.destroys == 1);
}
TEST_CASE("perceptual completion bounds admission and reuses completed events", "[backend][data][image_resize][perceptual]") {
    FakeCompletion fake;
    perceptual::CudaDownscaleCompletion completion(fake.api());
    auto custody = std::make_shared<int>(1);
    auto stream = reinterpret_cast<cudaStream_t>(std::uintptr_t{1});
    for (unsigned i = 0; i < 16; ++i) {
        auto distinct = std::make_shared<int>(static_cast<int>(i));
        auto& slot = completion.reserve(stream, distinct, custody);
        REQUIRE(completion.order(slot) == cudaSuccess);
        REQUIRE(completion.record(slot) == cudaSuccess);
    }
    REQUIRE(fake.orders == 0);
    REQUIRE_THROWS(completion.reserve(stream, custody, custody));
    REQUIRE(completion.settle(false) == cudaSuccess);
    REQUIRE(fake.events.size() == 16);
    REQUIRE(completion.settle(true) == cudaSuccess);
    auto& slot = completion.reserve(reinterpret_cast<cudaStream_t>(std::uintptr_t{2}), custody, custody);
    REQUIRE(completion.order(slot) == cudaSuccess);
    REQUIRE(fake.orders == 1);
    REQUIRE(completion.record(slot) == cudaSuccess);
    REQUIRE(fake.events.size() == 16);
    REQUIRE(completion.release() == cudaSuccess);
    REQUIRE(fake.destroys == 16);
}
TEST_CASE("perceptual completion extends one batch aggregate without per-image slots", "[backend][data][image_resize][perceptual]") {
    FakeCompletion fake;
    perceptual::CudaDownscaleCompletion completion(fake.api());
    auto owner = std::make_shared<std::array<int, 64>>();
    for (unsigned i = 0; i < 64; ++i) {
        std::shared_ptr<const void> alias(owner, &(*owner)[i]);
        auto& slot = completion.reserve(nullptr, alias, alias);
        REQUIRE(completion.record(slot) == cudaSuccess);
    }
    REQUIRE(fake.events.size() == 1);
    REQUIRE(completion.release() == cudaSuccess);
}
TEST_CASE("perceptual admission distinguishes caller facts from physical Driver failures", "[backend][data][image_resize][perceptual]") {
    using perceptual::AdmissionQuery;
    using perceptual::DriverAdmission;
    for (const auto query : {AdmissionQuery::Context, AdmissionQuery::Pointer, AdmissionQuery::Range, AdmissionQuery::Stream}) {
        const DriverAdmission valid{CUDA_SUCCESS, true, query}, foreign{CUDA_SUCCESS, false, query};
        REQUIRE_FALSE(valid.caller_rejection());
        REQUIRE_FALSE(valid.physical_failure());
        REQUIRE(foreign.caller_rejection());
        REQUIRE_FALSE(foreign.physical_failure());
        for (const auto status : {CUDA_ERROR_DEINITIALIZED, CUDA_ERROR_NOT_INITIALIZED, CUDA_ERROR_INVALID_CONTEXT, CUDA_ERROR_CONTEXT_IS_DESTROYED,
                                  CUDA_ERROR_ILLEGAL_ADDRESS, CUDA_ERROR_LAUNCH_FAILED}) {
            const DriverAdmission failed{status, false, query};
            REQUIRE(failed.status == status);
            REQUIRE(failed.physical_failure());
            REQUIRE_FALSE(failed.caller_rejection());
        }
    }
    for (const auto query : {AdmissionQuery::Pointer, AdmissionQuery::Range, AdmissionQuery::Stream}) {
        const DriverAdmission malformed{CUDA_ERROR_INVALID_VALUE, false, query};
        REQUIRE(malformed.caller_rejection());
        REQUIRE_FALSE(malformed.physical_failure());
    }
    const DriverAdmission unsupported{CUDA_ERROR_NOT_FOUND, false, AdmissionQuery::Range};
    const DriverAdmission destroyed_stream{CUDA_ERROR_INVALID_HANDLE, false, AdmissionQuery::Stream};
    REQUIRE(unsupported.caller_rejection());
    REQUIRE_FALSE(unsupported.physical_failure());
    REQUIRE(destroyed_stream.caller_rejection());
    REQUIRE_FALSE(destroyed_stream.physical_failure());
}
TEST_CASE("perceptual event allocation preserves status and existing custody before new admission", "[backend][data][image_resize][perceptual]") {
    for (const auto status :
         {cudaErrorMemoryAllocation, cudaErrorContextIsDestroyed, cudaErrorCudartUnloading, cudaErrorIllegalAddress, cudaErrorLaunchFailure}) {
        FakeCompletion fake;
        perceptual::CudaDownscaleCompletion completion(fake.api());
        auto previous = std::make_shared<int>(1), incoming = std::make_shared<int>(2);
        std::weak_ptr<int> pending = previous;
        auto& slot = completion.reserve(nullptr, previous, previous);
        REQUIRE(completion.record(slot) == cudaSuccess);
        previous.reset();
        REQUIRE(completion.settle(false) == cudaSuccess);
        REQUIRE_FALSE(pending.expired());
        fake.create_status = status;
        try {
            (void)completion.reserve(nullptr, incoming, incoming);
            FAIL("event allocation must propagate its exact failure");
        } catch (const perceptual::CompletionAllocationFailure& failure) {
            REQUIRE(failure.status() == status);
            REQUIRE(failure.physical_failure() == (status != cudaErrorMemoryAllocation));
        }
        REQUIRE(incoming.use_count() == 1);
        REQUIRE_FALSE(pending.expired());
        REQUIRE(fake.events.size() == 1);
        fake.create_status = cudaSuccess;
        REQUIRE(completion.release() == cudaSuccess);
        REQUIRE(pending.expired());
        // Ordinary allocation pressure leaves the completion owner reusable.
        if (status == cudaErrorMemoryAllocation) {
            auto& resumed = completion.reserve(nullptr, incoming, incoming);
            REQUIRE(completion.record(resumed) == cudaSuccess);
            REQUIRE(completion.release() == cudaSuccess);
            REQUIRE(incoming.use_count() == 1);
        }
    }
}
TEST_CASE("perceptual unproved completion retains its aggregate under terminal authority", "[backend][data][image_resize][perceptual]") {
    FakeCompletion fake;
    gpu::TerminalCudaRetirementOwner authority(1);
    auto lease = gpu::ReserveTerminalCudaLease(authority);
    auto completion = std::make_shared<perceptual::CudaDownscaleCompletion>(fake.api());
    auto custody = std::make_shared<int>(1);
    std::weak_ptr<int> weak = custody;
    auto& slot = completion->reserve(nullptr, custody, custody);
    REQUIRE(completion->record(slot) == cudaSuccess);
    custody.reset();
    fake.query_status = cudaErrorContextIsDestroyed;
    const auto failure = completion->settle(false);
    REQUIRE(fake.stream_waits == 0);
    REQUIRE(failure == cudaErrorContextIsDestroyed);
    std::move(lease).Install(gpu::TerminalCudaCustody::Share(std::move(completion)), failure);
    REQUIRE_FALSE(weak.expired());
    REQUIRE(authority.fact().occupancy == 1);
    REQUIRE_FALSE(authority.admission_open());
    REQUIRE_FALSE(authority.Reserve().has_value());
}
TEST_CASE("CUDA perceptual resampling retains actual imported Vulkan mapped subviews", "[backend][data][image_resize][perceptual][cuda][hardware]") {
    if (!has_cuda()) SKIP("CUDA unavailable; imported mapped view acceptance unexecuted");
    CudaFixture fixture;
    auto exporter = std::make_unique<gpu::test_support::VulkanWorkspaceFixture>(0, 21U, 17U);
    const auto layout = exporter->layout();
    auto imported = std::make_shared<gpu::ImportedImageBuffer>();
    std::string error;
    REQUIRE(imported->Import(*fixture.context_owner, exporter->Export(), layout, 1U, &error));
    fixture.context_owner->Bind();
    Image source(21, 17, RgbPixelFormat::RGBA8), cropped(19, 15, RgbPixelFormat::RGBA8), shape(9, 7, RgbPixelFormat::RGBA8);
    source.fill(3);
    std::memcpy(fixture.staging->data(), source.storage.data(), source.layout.capacity_bytes);
    cuda_check(cudaMemcpy2DAsync(reinterpret_cast<void*>(imported->data()), imported->pitch_bytes(), fixture.staging->data(), source.layout.row_stride_bytes,
                                 21U * 4U, 17U, cudaMemcpyHostToDevice, fixture.stream));
    exporter.reset();
    const auto offset = imported->pitch_bytes() + 4U;
    RgbConstImageView subview{reinterpret_cast<const void*>(imported->data() + offset),
                              {19, 15, imported->pitch_bytes(), 0, imported->pitch_bytes() * 16U - 4U, RgbPixelFormat::RGBA8}};
    auto destination = std::make_shared<DeviceImage>(shape.layout);
    GpuPerceptualDownscaler resizer(*fixture.context_owner, fixture.retirement);
    auto invalid = subview;
    invalid.layout.capacity_bytes = 1;
    REQUIRE_THROWS(resizer.downscale(invalid, destination->write(), fixture.stream, imported, destination));
    resizer.downscale(subview, destination->write(), fixture.stream, imported, destination);
    std::weak_ptr<gpu::ImportedImageBuffer> retained = imported;
    imported.reset();
    REQUIRE_FALSE(retained.expired());
    resizer.finish();
    CHECK(retained.expired());
    for (unsigned y = 0; y < 15; ++y)
        for (unsigned x = 0; x < 19; ++x)
            for (unsigned channel = 0; channel < 4; ++channel) cropped.set(x, y, channel, source.at(x + 1, y + 1, channel));
    CHECK(maximum_error(fixture.download(destination, fixture.stream), reference(cropped, 9, 7)) <= 1.0 / 255 + 1e-12);
}
}  // namespace mmltk::backend::imaging::resample
