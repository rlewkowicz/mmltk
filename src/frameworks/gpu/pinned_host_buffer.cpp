#include "src/frameworks/gpu/pinned_host_buffer.h"
#include <stdexcept>
#include <utility>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include "src/common/io/scoped_fd.h"
#include "src/common/system/numa_memory.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"

namespace mmltk::frameworks::gpu {
namespace {
void check(CUresult error, const char* operation) {
    if (error != CUDA_SUCCESS) throw std::runtime_error(std::string(operation) + ": CUDA status " + std::to_string(error));
}
class Context final {
   public:
    explicit Context(CUcontext context) { check(cuCtxPushCurrent(context), "bind registered host owner"); }
    ~Context() {
        CUcontext previous{};
        (void)cuCtxPopCurrent(&previous);
    }
};
}  // namespace
struct PinnedHostBuffer::State {
    State(CUcontext owner, int node, bool shared) : context(owner), memory(node), portable(shared) {}
    CUcontext context;
    mmltk::common::system::NumaMemory memory;
    bool portable;
    bool registered = false;
};
struct PinnedHostBuffer::Retention {
    TerminalCudaRetirementOwner terminal{1};
    TerminalCudaRetirementLease lease = ReserveTerminalCudaLease(terminal);
    mmltk::common::io::ScopedFd trace;
    Retention() {
        if (const auto* path = std::getenv("MMLTK_NUMA_TRANSFER_TRACE_FILE"); path && *path)
            trace.reset(::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
    }
};
PinnedHostBuffer::PinnedHostBuffer(CUcontext context, const mmltk::common::system::ExecutionPlacement& placement, bool portable,
                                   Register registration)
    : registration_(registration),
      placement_(placement),
      retention_(std::make_unique<Retention>()),
      state_(std::make_shared<State>(context, placement.numa_node, portable)) {
    if (!context || !registration || placement.cpus.empty())
        throw std::invalid_argument("pinned host buffer requires an owning context and placement");
}
PinnedHostBuffer::~PinnedHostBuffer() noexcept {
    if (!state_->registered) return;
    CUresult error = CUDA_ERROR_UNKNOWN;
    try {
        Context context(state_->context);
        error = cuCtxSynchronize();
    } catch (...) {}
    if (error == CUDA_SUCCESS) error = ReleaseSettled();
    if (error != CUDA_SUCCESS) std::move(retention_->lease).Install(TerminalCudaCustody::Share(std::move(state_)), cudaErrorUnknown);
}
std::unique_ptr<PinnedHostBuffer> PinnedHostBuffer::ForCurrentDevice(bool portable) {
    CUcontext context{};
    CUdevice device{};
    check(cuCtxGetCurrent(&context), "resolve local host allocation context");
    check(cuCtxGetDevice(&device), "resolve local host allocation device");
    const int node = mmltk::common::system::bound_memory_node(mmltk::common::system::capture_memory_policy());
    const auto execution = resolve_device_execution(device, mmltk::common::system::NumaTopology::Capture(), node);
    return std::make_unique<PinnedHostBuffer>(context, execution.placement, portable);
}
void PinnedHostBuffer::ensure_bytes(std::size_t bytes) {
    if (bytes <= capacity_bytes()) return;
    // The candidate owns its own physical retirement lease before registration.
    // Failure at any subsequent boundary leaves both allocations under RAII custody.
    PinnedHostBuffer replacement(state_->context, placement_, state_->portable, registration_);
    replacement.state_->memory.ensure_bytes(bytes);
    Context context(state_->context);
    check(registration_(replacement.data(), replacement.capacity_bytes(), state_->portable ? CU_MEMHOSTREGISTER_PORTABLE : 0),
          "register strictly local host pages");
    replacement.state_->registered = true;
    replacement.log("registered", bytes);
    if (state_->registered) check(cuCtxSynchronize(), "settle pinned host growth");
    check(ReleaseSettled(), "release pinned host growth");
    state_.swap(replacement.state_);
}
void PinnedHostBuffer::log(const char* event, std::size_t active_bytes) const noexcept {
    if (retention_->trace.get() < 0) return;
    char record[384];
    const int size = std::snprintf(record, sizeof(record),
                                   "{\"event\":\"%s\",\"owner_context\":%llu,\"node\":%d,\"allocation\":%llu,\"capacity_bytes\":%zu,"
                                   "\"active_bytes\":%zu,\"portable\":%s,\"pages\":\"owned-local-verified\"}\n",
                                   event, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(state_->context)), node(),
                                   static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(data())), capacity_bytes(),
                                   active_bytes, state_->portable ? "true" : "false");
    if (size > 0 && static_cast<std::size_t>(size) < sizeof(record)) {
        const auto written = ::write(retention_->trace.get(), record, static_cast<std::size_t>(size));
        (void)written;
    }
}
CUresult PinnedHostBuffer::ReleaseSettled() noexcept {
    if (!state_->registered) {
        state_->memory.reset();
        return CUDA_SUCCESS;
    }
    try {
        Context context(state_->context);
        const auto status = cuMemHostUnregister(state_->memory.data());
        if (status != CUDA_SUCCESS) return status;
        state_->registered = false;
        log("unregistered");
        state_->memory.reset();
        return CUDA_SUCCESS;
    } catch (...) { return CUDA_ERROR_UNKNOWN; }
}
void* PinnedHostBuffer::data() const noexcept { return state_->memory.data(); }
std::size_t PinnedHostBuffer::capacity_bytes() const noexcept { return state_->memory.capacity_bytes(); }
int PinnedHostBuffer::node() const noexcept { return state_->memory.node(); }
}  // namespace mmltk::frameworks::gpu
