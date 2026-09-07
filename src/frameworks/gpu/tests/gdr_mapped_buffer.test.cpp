#include "src/frameworks/gpu/gdr_mapped_buffer.h"
#include "src/frameworks/gpu/detail/gdr_buffer_backend.h"
#include "third_party/gdrcopy/src/gdr_backend_selection.h"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <future>
#include <filesystem>
#include <fcntl.h>
#include <cerrno>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
using mmltk::frameworks::gpu::GdrMappedBuffer;
namespace detail = mmltk::frameworks::gpu::detail;
constexpr std::size_t page = 65536;
const auto owner_context = reinterpret_cast<CUcontext>(1);
const auto first_stream = reinterpret_cast<CUstream>(1);
const auto second_stream = reinterpret_cast<CUstream>(2);

class FakeGdrBackend final : public detail::GdrBufferBackend {
   public:
    struct Allocation {
        std::vector<std::byte> memory;
        CUdeviceptr base;
    };
    struct Mapping {
        CUdeviceptr data;
        std::size_t bytes;
        bool mapped = false;
    };
    CUcontext current = owner_context;
    std::vector<CUcontext> context_stack;
    std::unordered_map<CUdeviceptr, Allocation> allocations;
    std::unordered_map<std::uintptr_t, Mapping> registrations;
    std::unordered_map<CUevent, bool> events;
    std::string failure;
    int selected_device = 3;
    int verified_device = -1;
    bool dmabuf = true;
    bool supported = true;
    bool backend_available = true;
    std::size_t created = 0, freed = 0, opened = 0, closed = 0;
    std::size_t pinned = 0, unpinned = 0, mapped = 0, unmapped = 0;
    std::size_t exported_fds = 0, closed_fds = 0;
    std::size_t copied = 0, waits = 0, context_waits = 0, events_created = 0, events_destroyed = 0;
    CUdeviceptr sync_allocation = 0;
    bool defer_copy = false, copy_entered = false, release_copy = false;
    std::mutex gate_mutex;
    std::condition_variable gate;

    void fail(const char* operation) {
        if (failure == operation) {
            failure.clear();
            throw std::runtime_error(operation);
        }
    }
    int release_failure(const char* operation) noexcept {
        if (failure != operation) return 0;
        failure.clear();
        return 1;
    }
    CUcontext current_context() override { return current; }
    void push_context(CUcontext context) override {
        fail("bind");
        context_stack.push_back(current);
        current = context;
    }
    int pop_context() noexcept override {
        current = context_stack.back();
        context_stack.pop_back();
        return 0;
    }
    int current_device() override {
        fail("device");
        return selected_device;
    }
    void* open() override {
        fail("open");
        if (!backend_available) throw mmltk::frameworks::gpu::GdrTransportUnavailable("GDR backends unsupported");
        ++opened;
        return this;
    }
    bool uses_dmabuf(void*) override {
        fail("backend");
        return dmabuf;
    }
    void require_device_support(bool, int device) override {
        verified_device = device;
        fail("support");
        if (!supported) throw mmltk::frameworks::gpu::GdrTransportUnavailable("selected device unsupported");
    }
    int close(void*) noexcept override {
        if (auto status = release_failure("close")) return status;
        ++closed;
        return 0;
    }
    CUdeviceptr allocate(std::size_t bytes) override {
        fail("allocate");
        Allocation allocation{.memory = std::vector<std::byte>(bytes + 2 * page), .base = 0};
        const auto raw = reinterpret_cast<std::uintptr_t>(allocation.memory.data());
        allocation.base = ((raw + page - 1) & ~(page - 1)) + 17;
        const auto base = allocation.base;
        allocations.emplace(base, std::move(allocation));
        ++created;
        return base;
    }
    void sync_memops(CUdeviceptr allocation) override {
        sync_allocation = allocation;
        fail("sync_memops");
    }
    int free(CUdeviceptr allocation) noexcept override {
        if (auto status = release_failure("free")) return status;
        if (allocations.erase(allocation) != 1) std::terminate();
        ++freed;
        return 0;
    }
    std::uintptr_t pin(void*, CUdeviceptr data, std::size_t bytes) override {
        fail("pin");
        const auto id = ++next_mapping;
        registrations.emplace(id, Mapping{data, bytes});
        ++pinned;
        if (dmabuf) ++exported_fds;
        return id;
    }
    int unpin(void*, std::uintptr_t id) noexcept override {
        if (auto status = release_failure("unpin")) return status;
        if (registrations.erase(id) != 1) std::terminate();
        ++unpinned;
        if (dmabuf) ++closed_fds;
        return 0;
    }
    void* map(void*, std::uintptr_t id, std::size_t) override {
        fail("map");
        auto& entry = registrations.at(id);
        entry.mapped = true;
        ++mapped;
        return reinterpret_cast<void*>(entry.data - 4096);
    }
    detail::GdrMappingInfo info(void*, std::uintptr_t id) override {
        auto& entry = registrations.at(id);
        fail(entry.mapped ? "mapped_info" : "pinned_info");
        return {.base = entry.data - 4096, .bytes = entry.bytes + 4096, .page_size = 4096, .mapping_type = 1, .mapped = entry.mapped};
    }
    int unmap(void*, std::uintptr_t id, void*, std::size_t) noexcept override {
        if (auto status = release_failure("unmap")) return status;
        registrations.at(id).mapped = false;
        ++unmapped;
        return 0;
    }
    void copy(std::uintptr_t, void* destination, const void* source, std::size_t bytes) override {
        fail("copy");
        {
            std::unique_lock lock(gate_mutex);
            if (defer_copy) {
                copy_entered = true;
                gate.notify_all();
                gate.wait(lock, [&] { return release_copy; });
            }
        }
        std::memcpy(destination, source, bytes);
        ++copied;
    }
    CUevent create_event() override {
        fail("event");
        const auto event = reinterpret_cast<CUevent>(++next_event);
        events.emplace(event, false);
        ++events_created;
        return event;
    }
    void record_event(CUevent event, CUstream) override {
        fail("record");
        events.at(event) = true;
    }
    void wait_event(CUevent event) override {
        fail("wait");
        events.at(event) = false;
        ++waits;
    }
    int destroy_event(CUevent event) noexcept override {
        if (auto status = release_failure("destroy_event")) return status;
        if (events.erase(event) != 1) std::terminate();
        ++events_destroyed;
        return 0;
    }
    void synchronize_context() override {
        fail("synchronize");
        ++context_waits;
    }

   private:
    std::uintptr_t next_mapping = 0;
    std::uintptr_t next_event = 0;
};

void check_no_live_resources(const FakeGdrBackend& api) {
    // CLEANUP-IGNORE: Physical GDR balance invariants are unrelated to reflected browser identity comparisons.
    CHECK(api.created == api.freed);
    CHECK(api.opened == api.closed);
    CHECK(api.pinned == api.unpinned);
    CHECK(api.exported_fds == api.closed_fds);
    CHECK(api.mapped == api.unmapped);
    CHECK(api.events_created == api.events_destroyed);
    CHECK(api.context_stack.empty());
}

template <std::size_t Size>
void check_consumed_values(const std::array<std::byte, Size>& values, const std::array<unsigned char, Size>& result) {
    for (std::size_t i = 0; i < result.size(); ++i)
        CHECK(result[i] == (std::to_integer<unsigned char>(values[i]) ^ 90));
}

TEST_CASE("Mapped storage owns alignment offsets tails and persistent capacity", "[frameworks][gpu][gdr]") {
    auto api = std::make_shared<FakeGdrBackend>();
    {
        GdrMappedBuffer buffer(owner_context, 2, api);
        buffer.ensure_bytes(71);
        CHECK(api->verified_device == api->selected_device);
        CHECK(buffer.uses_dmabuf());
        CHECK(api->allocations.contains(api->sync_allocation));
        std::array<std::byte, 71> values{};
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i] = static_cast<std::byte>(i);
        REQUIRE(buffer.write(3, values));
        auto lease = buffer.borrow();
        const auto data = lease.device_data();
        CHECK(data % page == 0);
        CHECK(data != api->sync_allocation);
        CHECK(std::memcmp(reinterpret_cast<void*>(data + 3), values.data(), values.size()) == 0);
        CHECK_THROWS_AS(buffer.write(0, values), std::logic_error);
        CHECK_THROWS_AS(buffer.close(), std::logic_error);
        lease.record_consumed(first_stream);
        lease.record_consumed(second_stream);
        lease = {};
        REQUIRE(buffer.write(page - values.size(), values));
        CHECK(api->waits == 2);
        CHECK(std::memcmp(reinterpret_cast<void*>(data + page - values.size()), values.data(), values.size()) == 0);
        buffer.ensure_bytes(32);
        CHECK(api->created == 1);
        CHECK(api->pinned == 1);
        CHECK(api->events_created == 2);
        CHECK_THROWS_AS(buffer.write(page, values), std::out_of_range);
        CHECK_THROWS_AS(buffer.ensure_bytes(std::numeric_limits<std::size_t>::max()), std::bad_alloc);
    }
    check_no_live_resources(*api);
}

TEST_CASE("Mapped construction failures release every acquired physical resource", "[frameworks][gpu][gdr]") {
    for (const auto* failure :
         {"bind", "device", "open", "backend", "support", "allocate", "sync_memops", "pin", "pinned_info", "map", "mapped_info", "event"}) {
        CAPTURE(failure);
        auto api = std::make_shared<FakeGdrBackend>();
        GdrMappedBuffer buffer(owner_context, 2, api);
        api->failure = failure;
        bool runtime_failure = false;
        try {
            buffer.ensure_bytes(64);
        } catch (const mmltk::frameworks::gpu::GdrTransportUnavailable&) {
            FAIL("resource failure misclassified as unavailable transport");
        } catch (const std::runtime_error&) { runtime_failure = true; }
        CHECK(runtime_failure);
        CHECK(buffer.capacity_bytes() == 0);
        check_no_live_resources(*api);
        buffer.ensure_bytes(64);
        CHECK(buffer.capacity_bytes() == page);
    }
}

TEST_CASE("GDR fallback selection preserves operation failures across unsupported backends", "[frameworks][gpu][gdr]") {
    CHECK(gdr_backend_selection_error(EIO, ENOTSUP) == EIO);
    CHECK(gdr_backend_selection_error(ENOTSUP, ENOTSUP) == ENOTSUP);
    for (const int failure : {EIO, ENOMEM, EACCES, EPERM, EMFILE, EINVAL}) {
        CAPTURE(failure);
        CHECK(gdr_backend_selection_error(failure, ENOTSUP) == failure);
        CHECK(gdr_backend_selection_error(ENOTSUP, failure) == failure);
        CHECK(gdr_backend_selection_error(failure, EIO) == failure);
        CHECK(gdr_backend_selection_error(failure, 0) == 0);
        CHECK(gdr_backend_selection_error(0, failure) == 0);
    }
}

TEST_CASE("Verified GDR backend support failure retains its typed classification", "[frameworks][gpu][gdr]") {
    auto api = std::make_shared<FakeGdrBackend>();
    GdrMappedBuffer buffer(owner_context, 1, api);
    api->backend_available = false;
    CHECK_THROWS_AS(buffer.ensure_bytes(64), mmltk::frameworks::gpu::GdrTransportUnavailable);
    CHECK(api->opened == 0U);
    api->backend_available = true;
    api->supported = false;
    CHECK_THROWS_AS(buffer.ensure_bytes(64), mmltk::frameworks::gpu::GdrTransportUnavailable);
    CHECK(api->created == 0U);
    CHECK(api->opened == api->closed);
    CHECK(api->context_stack.empty());
    api->supported = true;
    buffer.ensure_bytes(64);
    CHECK(buffer.capacity_bytes() == page);
}

TEST_CASE("Mapped growth preserves old leases and failed growth preserves current storage", "[frameworks][gpu][gdr]") {
    auto api = std::make_shared<FakeGdrBackend>();
    GdrMappedBuffer buffer(owner_context, 1, api);
    buffer.ensure_bytes(64);
    auto old = buffer.borrow();
    const auto old_pointer = old.device_data();
    api->failure = "map";
    CHECK_THROWS(buffer.ensure_bytes(page + 1));
    CHECK(buffer.capacity_bytes() == page);
    CHECK(old.device_data() == old_pointer);
    CHECK(api->freed == 1);
    buffer.ensure_bytes(page + 1);
    CHECK(buffer.capacity_bytes() == 2 * page);
    CHECK(old.device_data() == old_pointer);
    CHECK(api->freed == 1);
    CHECK_THROWS_AS(buffer.close(), std::logic_error);
    old.record_consumed(first_stream);
    old = {};
    CHECK(api->waits == 1);
    CHECK(api->freed == 2);
    buffer.close();
    CHECK(api->freed == 3);
}

TEST_CASE("Mapped selected device context and consumer failure paths are explicit", "[frameworks][gpu][gdr]") {
    auto api = std::make_shared<FakeGdrBackend>();
    CHECK_THROWS_AS(GdrMappedBuffer(reinterpret_cast<CUcontext>(2), 1, api), std::invalid_argument);
    GdrMappedBuffer buffer(owner_context, 1, api);
    api->supported = false;
    CHECK_THROWS(buffer.ensure_bytes(64));
    CHECK(api->verified_device == 3);
    CHECK(api->created == 0);
    api->supported = true;
    api->current = reinterpret_cast<CUcontext>(2);
    buffer.ensure_bytes(64);
    CHECK(api->current == reinterpret_cast<CUcontext>(2));
    auto lease = buffer.borrow();
    lease.record_consumed(first_stream);
    CHECK_THROWS(lease.record_consumed(second_stream));
    lease = {};
    std::array<std::byte, 3> bytes{};
    REQUIRE(buffer.write(0, bytes));
    CHECK(api->context_waits == 1);
    auto unrecorded = buffer.borrow();
    unrecorded = {};
    REQUIRE(buffer.write(0, bytes));
    CHECK(api->context_waits == 2);
    auto failed_record = buffer.borrow();
    api->failure = "record";
    CHECK_THROWS(failed_record.record_consumed(first_stream));
    failed_record = {};
    REQUIRE(buffer.write(0, bytes));
    CHECK(api->context_waits == 3);
    api->failure = "unpin";
    CHECK_THROWS(buffer.close());
    CHECK(api->freed == 0);
    buffer.close();
    CHECK(api->freed == 1);
}

TEST_CASE("Failed GPU settlement prevents mapped overwrite until completion succeeds", "[frameworks][gpu][gdr]") {
    auto api = std::make_shared<FakeGdrBackend>();
    GdrMappedBuffer buffer(owner_context, 1, api);
    buffer.ensure_bytes(1);
    std::array<std::byte, 3> bytes{};
    auto lease = buffer.borrow();
    lease.record_consumed(first_stream);
    lease = {};
    api->failure = "wait";
    CHECK_THROWS(buffer.write(0, bytes));
    CHECK(api->copied == 0);
    CHECK(api->freed == 0);
    REQUIRE(buffer.write(0, bytes));
    CHECK(api->waits == 1);
    lease = buffer.borrow();
    lease = {};
    api->failure = "synchronize";
    CHECK_THROWS(buffer.write(0, bytes));
    CHECK(api->copied == 1);
    REQUIRE(buffer.write(0, bytes));
    CHECK(api->context_waits == 1);
    CHECK(api->context_stack.empty());
}

TEST_CASE("Independent mapped owners register and copy while another CPU writer is blocked", "[frameworks][gpu][gdr]") {
    auto first = std::make_shared<FakeGdrBackend>();
    auto second = std::make_shared<FakeGdrBackend>();
    GdrMappedBuffer existing(owner_context, 1, first);
    existing.ensure_bytes(1);
    first->defer_copy = true;
    std::array<std::byte, 3> bytes{};
    auto copying = std::async(std::launch::async, [&] { return existing.write(0, bytes); });
    struct CopyGate final {
        FakeGdrBackend& backend;
        ~CopyGate() {
            std::lock_guard lock(backend.gate_mutex);
            backend.release_copy = true;
            backend.gate.notify_all();
        }
    };
    {
        CopyGate release{*first};
        {
            std::unique_lock lock(first->gate_mutex);
            first->gate.wait(lock, [&] { return first->copy_entered; });
        }
        GdrMappedBuffer registering(owner_context, 1, second);
        registering.ensure_bytes(1);
        REQUIRE(registering.write(0, bytes));
        registering.close();
        CHECK(second->created == second->freed);
        CHECK(second->copied == 1);
        CHECK(first->copied == 0);
    }
    CHECK(copying.get());
}

TEST_CASE("Mapped retirement retries retain dependent resources and zero capacity stays empty", "[frameworks][gpu][gdr]") {
    for (const auto* boundary : {"unmap", "unpin", "free", "destroy_event", "close"}) {
        CAPTURE(boundary);
        auto api = std::make_shared<FakeGdrBackend>();
        api->dmabuf = false;
        GdrMappedBuffer buffer(owner_context, 1, api);
        buffer.ensure_bytes(0);
        CHECK(api->opened == 0);
        CHECK_THROWS_AS(buffer.borrow(), std::logic_error);
        buffer.ensure_bytes(1);
        CHECK_FALSE(buffer.uses_dmabuf());
        api->failure = boundary;
        CHECK_THROWS(buffer.close());
        CHECK_THROWS_AS(buffer.borrow(), std::logic_error);
        CHECK_THROWS_AS(buffer.ensure_bytes(page + 1), std::logic_error);
        buffer.close();
        CHECK(api->opened == api->closed);
        CHECK(api->created == api->freed);
        CHECK(api->pinned == api->unpinned);
        CHECK(api->mapped == api->unmapped);
        CHECK(api->events_created == api->events_destroyed);
        CHECK(api->exported_fds == 0);
    }
}

TEST_CASE("Cancellation cannot relinquish an active mapped CPU writer", "[frameworks][gpu][gdr]") {
    auto api = std::make_shared<FakeGdrBackend>();
    GdrMappedBuffer buffer(owner_context, 1, api);
    buffer.ensure_bytes(64);
    std::array<std::byte, 3> bytes{};
    std::stop_source cancellation;
    cancellation.request_stop();
    CHECK_FALSE(buffer.write(0, bytes, cancellation.get_token()));
    CHECK(api->copied == 0);
    cancellation = std::stop_source{};
    api->defer_copy = true;
    auto writing = std::async(std::launch::async, [&] { return buffer.write(0, bytes, cancellation.get_token()); });
    {
        std::unique_lock lock(api->gate_mutex);
        api->gate.wait(lock, [&] { return api->copy_entered; });
        cancellation.request_stop();
        CHECK(api->freed == 0);
        api->release_copy = true;
        api->gate.notify_all();
    }
    CHECK(writing.get());
    CHECK(api->copied == 1);
    buffer.close();
    CHECK(api->freed == 1);
}
}  // namespace

namespace {
class HardwareGdrContext final {
   public:
    explicit HardwareGdrContext(CUdevice device) {
        if (cuCtxCreate(&context, nullptr, CU_CTX_SCHED_AUTO, device) != CUDA_SUCCESS)
            throw std::runtime_error("create isolated GDR hardware context");
    }
    ~HardwareGdrContext() { (void)cuCtxDestroy(context); }
    CUcontext context{};
};
struct HardwareConsumer final {
    CUmodule module{};
    CUfunction function{};
    CUstream stream{};
    CUdeviceptr output{};
    HardwareConsumer() {
        try {
            // Functional consumption, not timing: each GPU thread reads the
            // newly CPU-written mapping and changes one byte in separate output.
            constexpr char kernel[] = R"ptx(
.version 7.0
.target sm_52
.address_size 64
.visible .entry consume(.param .u64 source, .param .u64 destination, .param .u32 length) {
 .reg .pred %outside;
 .reg .b32 %block, %width, %thread, %index, %count, %value;
 .reg .b64 %src, %dst, %offset;
 ld.param.u64 %src, [source];
 ld.param.u64 %dst, [destination];
 ld.param.u32 %count, [length];
 mov.u32 %block, %ctaid.x;
 mov.u32 %width, %ntid.x;
 mov.u32 %thread, %tid.x;
 mad.lo.u32 %index, %block, %width, %thread;
 setp.ge.u32 %outside, %index, %count;
 @%outside bra done;
 cvt.u64.u32 %offset, %index;
 add.u64 %src, %src, %offset;
 add.u64 %dst, %dst, %offset;
 ld.global.u8 %value, [%src];
 xor.b32 %value, %value, 90;
 st.global.u8 [%dst], %value;
 done: ret;
}
)ptx";
            if (cuModuleLoadData(&module, kernel) != CUDA_SUCCESS || cuModuleGetFunction(&function, module, "consume") != CUDA_SUCCESS ||
                cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING) != CUDA_SUCCESS || cuMemAlloc(&output, page) != CUDA_SUCCESS)
                throw std::runtime_error("initialize GDR hardware consumer");
        } catch (...) {
            release();
            throw;
        }
    }
    ~HardwareConsumer() { release(); }
    void release() noexcept {
        if (stream) (void)cuStreamSynchronize(stream);
        if (output) {
            (void)cuMemFree(output);
            output = 0;
        }
        if (stream) {
            (void)cuStreamDestroy(stream);
            stream = nullptr;
        }
        if (module) {
            (void)cuModuleUnload(module);
            module = nullptr;
        }
    }
    void submit(CUdeviceptr source, unsigned length) {
        void* arguments[]{&source, &output, &length};
        if (cuLaunchKernel(function, (length + 127) / 128, 1, 1, 128, 1, 1, 0, stream, arguments, nullptr) != CUDA_SUCCESS)
            throw std::runtime_error("launch GDR hardware consumer");
    }
};

CUdevice hardware_device() {
    if (cuInit(0) != CUDA_SUCCESS) SKIP("CUDA unavailable; GDR hardware behavior unverified");
    int count{};
    REQUIRE(cuDeviceGetCount(&count) == CUDA_SUCCESS);
    if (!count) SKIP("No CUDA-visible GPU; GDR hardware behavior unverified");
    // Run the same test once per requested visible ordinal through the wrapper.
    int ordinal = 0;
    if (const char* selected = std::getenv("MMLTK_GDR_TEST_DEVICE")) ordinal = std::stoi(selected);
    CUdevice device{};
    REQUIRE(cuDeviceGet(&device, ordinal) == CUDA_SUCCESS);
    return device;
}
void require_hardware_backend() {
    const auto api = detail::gdr_buffer_backend();
    void* handle{};
    try {
        handle = api->open();
    } catch (const std::runtime_error& error) {
        SKIP(std::string("GDR backend unavailable; hardware transfer unverified: ") + error.what());
    }
    bool dmabuf{};
    try {
        dmabuf = api->uses_dmabuf(handle);
        api->require_device_support(dmabuf, api->current_device());
    } catch (const std::runtime_error& error) {
        (void)api->close(handle);
        SKIP(std::string("Selected GPU lacks required support; hardware transfer unverified: ") + error.what());
    }
    REQUIRE(api->close(handle) == 0);
    if (const char* expected = std::getenv("MMLTK_GDR_TEST_BACKEND")) {
        const std::string backend(expected);
        REQUIRE((backend == "gdrdrv" || backend == "dmabuf"));
        if (dmabuf != (backend == "dmabuf")) SKIP("Requested GDR backend is unavailable; that backend remains unverified");
    }
    INFO("GDR hardware backend: " << (dmabuf ? "dmabuf" : "gdrdrv"));
}

std::vector<int> mapped_descriptors() {
    std::vector<int> result;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        std::error_code error;
        const auto target = std::filesystem::read_symlink(entry.path(), error).string();
        if (!error && (target.find("dmabuf") != std::string::npos || target == "/dev/gdrdrv"))
            result.push_back(std::stoi(entry.path().filename().string()));
    }
    return result;
}

TEST_CASE("GDR CPU writes precede real CUDA consumption on an isolated context", "[frameworks][gpu][gdr][hardware]") {
    HardwareGdrContext owner(hardware_device());
    require_hardware_backend();
    const auto descriptors_before = mapped_descriptors();
    GdrMappedBuffer buffer(owner.context);
    buffer.ensure_bytes(page);
    auto descriptors = mapped_descriptors();
    std::erase_if(descriptors, [&](int fd) { return std::ranges::find(descriptors_before, fd) != descriptors_before.end(); });
    REQUIRE(descriptors.size() == 1);
    for (int fd : descriptors)
        CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    HardwareConsumer consumer;
    std::array<std::byte, 71> values{};
    std::array<unsigned char, 71> result{};
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = static_cast<std::byte>(i + 1);
    for (const std::size_t offset :
         {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{15}, std::size_t{31}, page - values.size()}) {
        CAPTURE(offset);
        REQUIRE(buffer.write(offset, values));
        auto lease = buffer.borrow();
        consumer.submit(lease.device_data() + offset, static_cast<unsigned>(values.size()));
        lease.record_consumed(consumer.stream);
        lease = {};
        // Overwriting exercises event settlement before CPU stores and then
        // validates the output produced from the preceding mapped contents.
        REQUIRE(buffer.write(offset, values));
        REQUIRE(cuMemcpyDtoH(result.data(), consumer.output, result.size()) == CUDA_SUCCESS);
        check_consumed_values(values, result);
    }
    REQUIRE(buffer.write(0, values));
    auto prior = buffer.borrow();
    const auto prior_data = prior.device_data();
    buffer.ensure_bytes(page + 1);
    consumer.submit(prior_data, static_cast<unsigned>(values.size()));
    prior.record_consumed(consumer.stream);
    prior = {};
    REQUIRE(cuMemcpyDtoH(result.data(), consumer.output, result.size()) == CUDA_SUCCESS);
    check_consumed_values(values, result);
    for (int fd : descriptors) {
        errno = 0;
        CHECK(fcntl(fd, F_GETFD) == -1);
        CHECK(errno == EBADF);
    }
    buffer.close();
}

TEST_CASE("Independent GDR handles copy while another isolated owner registers and retires", "[frameworks][gpu][gdr][hardware]") {
    const auto device = hardware_device();
    {
        HardwareGdrContext probe(device);
        require_hardware_backend();
    }
    std::promise<void> start;
    auto ready = start.get_future().share();
    auto worker = [&](bool replace) {
        HardwareGdrContext owner(device);
        GdrMappedBuffer stable(owner.context);
        stable.ensure_bytes(page);
        HardwareConsumer consumer;
        std::vector<std::byte> values(page, std::byte{0x3c});
        std::vector<unsigned char> result(page);
        ready.wait();
        for (unsigned iteration = 0; iteration != 8; ++iteration) {
            std::unique_ptr<GdrMappedBuffer> transient;
            if (replace) {
                transient = std::make_unique<GdrMappedBuffer>(owner.context);
                transient->ensure_bytes(page);
            }
            auto& buffer = transient ? *transient : stable;
            if (!buffer.write(0, values)) throw std::runtime_error("unexpected hardware cancellation");
            auto lease = buffer.borrow();
            consumer.submit(lease.device_data(), static_cast<unsigned>(values.size()));
            lease.record_consumed(consumer.stream);
            lease = {};
            if (!buffer.write(0, values)) throw std::runtime_error("unexpected hardware cancellation");
            if (cuMemcpyDtoH(result.data(), consumer.output, result.size()) != CUDA_SUCCESS)
                throw std::runtime_error("GDR concurrent readback failed");
            if (std::ranges::any_of(result, [](auto value) { return value != (0x3c ^ 90); }))
                throw std::runtime_error("GDR concurrent CUDA consumer observed incorrect bytes");
        }
        stable.close();
    };
    auto copies = std::async(std::launch::async, worker, false);
    auto registrations = std::async(std::launch::async, worker, true);
    start.set_value();
    REQUIRE_NOTHROW(copies.get());
    REQUIRE_NOTHROW(registrations.get());
}
}  // namespace
