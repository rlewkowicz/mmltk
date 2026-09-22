#include "src/frameworks/gpu/gdr_mapped_buffer.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>
#include "src/common/io/scoped_fd.h"
#include "src/frameworks/gpu/detail/gdr_buffer_backend.h"
#include "src/frameworks/gpu/resource_owner_command_authority.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
namespace mmltk::frameworks::gpu {
namespace {
constexpr std::size_t mapping_alignment = 65536;
class Context final {
public:
 Context(detail::GdrBufferBackend& backend, CUcontext owner) : backend_(backend) { backend_.push_context(owner); }
 ~Context() {
  if (active_ && backend_.pop_context() != 0) (void)fail_current_resource_worker();
 }
 void finish() {
  active_ = false;
  const int result = backend_.pop_context();
  if (result != 0) {
   (void)fail_current_resource_worker();
   throw std::runtime_error("restore GDR caller context: status " + std::to_string(result));
  }
 }

private:
 detail::GdrBufferBackend& backend_;
 bool active_ = true;
};
std::size_t rounded_capacity(std::size_t bytes) {
 constexpr auto maximum = std::numeric_limits<std::size_t>::max();
 if (bytes > maximum - 2 * (mapping_alignment - 1)) throw std::bad_alloc();
 return (bytes + mapping_alignment - 1) & ~(mapping_alignment - 1);
}
void release_check(int result, const char* operation) {
 if (result != 0) throw std::runtime_error(std::string(operation) + ": status " + std::to_string(result));
}
}  // namespace
struct GdrMappedBuffer::Lifetime {
 std::atomic<std::size_t> allocations{0};
 std::atomic<bool> terminal{false};
};
struct GdrMappedBuffer::Storage {
 struct Physical final {
  struct Consumer final {
   CUevent event{};
   CUstream stream{};
   bool pending = false;
  };
  std::shared_ptr<detail::GdrBufferBackend> backend;
  CUcontext context;
  int device = -1;
  void* handle = nullptr;
  CUdeviceptr allocation = 0;
  CUdeviceptr device_pointer = 0;
  std::size_t allocated_bytes = 0;
  std::size_t capacity = 0;
  std::uintptr_t registration = 0;
  void* mapping = nullptr;
  std::byte* cpu_pointer = nullptr;
  std::size_t mapped_bytes = 0;
  int mapping_type = 0;
  bool dmabuf = false;
  bool backend_known = false;
  bool unrecorded = false;
  std::size_t pending_consumers = 0;
  std::vector<Consumer> consumers;
  mmltk::common::io::ScopedFd trace;
  Physical(std::shared_ptr<detail::GdrBufferBackend> api, CUcontext owner, std::size_t count) : backend(std::move(api)), context(owner), consumers(count) {
   if (const char* path = std::getenv("MMLTK_GDR_TRACE_FILE"); path && *path) trace.reset(::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
  }
  void log(const char* event, std::size_t bytes = 0) const noexcept {
   if (trace.get() < 0) return;
   char text[512];
   const int length = std::snprintf(text, sizeof(text),
    "{\"event\":\"%s\",\"device\":%d,\"context\":%llu,\"backend\":\"%s\",\"allocation\":%llu,"
    "\"allocated_bytes\":%zu,\"capacity\":%zu,\"mapped_bytes\":%zu,\"mapping_type\":%d,"
    "\"owned_export_fds\":%d,\"bytes\":%zu}\n",
    event, device, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(context)), backend_known ? (dmabuf ? "dmabuf" : "gdrdrv") : "unselected",
    static_cast<unsigned long long>(allocation), allocated_bytes, capacity, mapped_bytes, mapping_type, dmabuf && registration ? 1 : 0, bytes);
   if (length > 0 && static_cast<std::size_t>(length) < sizeof(text)) {
    const auto written = ::write(trace.get(), text, static_cast<std::size_t>(length));
    (void)written;
   }
  }
  void settle() {
   if (unrecorded) {
    backend->synchronize_context();
    unrecorded = false;
    for (auto& consumer : consumers) consumer.pending = false;
    pending_consumers = 0;
   } else {
    for (auto& consumer : consumers) {
     if (!consumer.pending) continue;
     backend->wait_event(consumer.event);
     consumer.pending = false;
     --pending_consumers;
    }
   }
  }
  void release() {
   if (!handle && !allocation) return;
   Context binding(*backend, context);
   settle();
   // Stop at the first failed physical release, retaining every dependent
   // resource. A retry resumes at this exact remaining owner boundary.
   if (mapping) {
    release_check(backend->unmap(handle, registration, mapping, mapped_bytes), "unmap GDR storage");
    mapping = nullptr;
    cpu_pointer = nullptr;
   }
   if (registration) {
    release_check(backend->unpin(handle, registration), "unpin GDR storage");
    registration = 0;
   }
   if (allocation) {
    release_check(backend->free(allocation), "free original GDR allocation");
    allocation = 0;
    device_pointer = 0;
   }
   for (auto& consumer : consumers) {
    if (!consumer.event) continue;
    release_check(backend->destroy_event(consumer.event), "destroy GDR event");
    consumer.event = nullptr;
   }
   if (handle) {
    release_check(backend->close(handle), "close GDR handle");
    handle = nullptr;
   }
   log("released");
   capacity = 0;
   binding.finish();
  }
 };
 TerminalCudaRetirementOwner terminal{1};
 TerminalCudaRetirementLease retirement = ReserveTerminalCudaLease(terminal);
 std::shared_ptr<Physical> physical;
 std::shared_ptr<Lifetime> lifetime;
 std::mutex mutex;
 std::size_t readers = 0;
 Storage(std::shared_ptr<detail::GdrBufferBackend> backend, CUcontext context, std::size_t count, std::shared_ptr<Lifetime> family)
     : physical(std::make_shared<Physical>(std::move(backend), context, count)), lifetime(std::move(family)) {
  lifetime->allocations.fetch_add(1, std::memory_order_relaxed);
 }
 ~Storage() noexcept {
  try {
   physical->release();
  } catch (...) {
   lifetime->terminal.store(true, std::memory_order_release);
   physical->log("retirement_failed");
   std::move(retirement).Install(TerminalCudaCustody::Share(std::move(physical)), cudaErrorUnknown);
  }
  lifetime->allocations.fetch_sub(1, std::memory_order_release);
 }
 void initialize(std::size_t bytes) {
  auto& p = *physical;
  Context binding(*p.backend, p.context);
  p.device = p.backend->current_device();
  try {
   p.handle = p.backend->open();
   p.dmabuf = p.backend->uses_dmabuf(p.handle);
   p.backend_known = true;
   p.backend->require_device_support(p.dmabuf, p.device);
   p.capacity = rounded_capacity(bytes);
   p.allocated_bytes = p.capacity + mapping_alignment - 1;
   p.allocation = p.backend->allocate(p.allocated_bytes);
   if (p.allocation > std::numeric_limits<CUdeviceptr>::max() - (mapping_alignment - 1)) throw std::bad_alloc();
   p.backend->sync_memops(p.allocation);
   p.device_pointer = (p.allocation + mapping_alignment - 1) & ~(static_cast<CUdeviceptr>(mapping_alignment) - 1);
   p.registration = p.backend->pin(p.handle, p.device_pointer, p.capacity);
   const auto before = p.backend->info(p.handle, p.registration);
   if (before.page_size == 0 || before.page_size > mapping_alignment || (before.page_size & (before.page_size - 1)) != 0 || before.base % before.page_size != 0 ||
       before.bytes % before.page_size != 0 || before.base > p.device_pointer || before.bytes < p.capacity || p.device_pointer - before.base > before.bytes - p.capacity ||
       before.base < p.allocation || before.bytes > p.allocated_bytes - (before.base - p.allocation))
    throw std::runtime_error("GDR registration exceeds its owning allocation");
   p.mapped_bytes = before.bytes;
   p.mapping = p.backend->map(p.handle, p.registration, p.mapped_bytes);
   const auto after = p.backend->info(p.handle, p.registration);
   if (!after.mapped || after.base != before.base || after.bytes != before.bytes || after.page_size != before.page_size) throw std::runtime_error("GDR mapping geometry changed during registration");
   p.mapping_type = after.mapping_type;
   p.cpu_pointer = static_cast<std::byte*>(p.mapping) + (p.device_pointer - after.base);
   for (auto& consumer : p.consumers) consumer.event = p.backend->create_event();
   p.log("allocated");
   binding.finish();
  } catch (...) {
   p.log("allocation_failed");
   throw;
  }
 }
};
GdrMappedBuffer::GdrMappedBuffer(CUcontext owner, std::size_t consumers) : GdrMappedBuffer(owner, consumers, detail::gdr_buffer_backend()) {}
GdrMappedBuffer::GdrMappedBuffer(CUcontext owner, std::size_t consumers, std::shared_ptr<detail::GdrBufferBackend> backend)
    : context_(owner), consumer_capacity_(consumers), backend_(std::move(backend)), lifetime_(std::make_shared<Lifetime>()) {
 if (!owner || !backend_ || consumers == 0) throw std::invalid_argument("GDR buffer requires an owner context and bounded consumers");
 if (backend_->current_context() != owner) throw std::invalid_argument("construct GDR buffer on its actual current owner context");
}
GdrMappedBuffer::~GdrMappedBuffer() noexcept = default;
void GdrMappedBuffer::require_usable() const {
 if (closing_) throw std::logic_error("GDR buffer is closing");
 if (lifetime_->terminal.load(std::memory_order_acquire)) throw std::runtime_error("GDR owner has terminal physical resources");
}
void GdrMappedBuffer::ensure_bytes(std::size_t bytes) {
 require_usable();
 if (bytes <= capacity_bytes()) return;
 (void)rounded_capacity(bytes);
 auto replacement = std::make_shared<Storage>(backend_, context_, consumer_capacity_, lifetime_);
 replacement->initialize(bytes);
 storage_.swap(replacement);
 replacement.reset();
 require_usable();
}
std::size_t GdrMappedBuffer::capacity_bytes() const noexcept { return storage_ ? storage_->physical->capacity : 0; }
bool GdrMappedBuffer::uses_dmabuf() const noexcept { return storage_ && storage_->physical->dmabuf; }
bool GdrMappedBuffer::write(std::size_t offset, std::span<const std::byte> bytes, std::stop_token stop) {
 require_usable();
 if (!storage_) throw std::logic_error("GDR buffer has no storage");
 std::lock_guard lock(storage_->mutex);
 auto& p = *storage_->physical;
 if (offset > p.capacity || bytes.size() > p.capacity - offset) throw std::out_of_range("GDR write exceeds capacity");
 if (storage_->readers) throw std::logic_error("GDR buffer is retained by a consumer");
 if (stop.stop_requested()) return false;
 if (p.unrecorded || p.pending_consumers) {
  Context binding(*p.backend, p.context);
  p.settle();
  binding.finish();
 }
 if (stop.stop_requested()) return false;
 // Established mappings require no CUDA context operations on the I/O
 // worker. Only a previous GPU consumer introduces a settlement boundary.
 if (!bytes.empty()) p.backend->copy(p.registration, p.cpu_pointer + offset, bytes.data(), bytes.size());
 p.log("copy_complete", bytes.size());
 return true;
}
GdrMappedBuffer::ReadLease GdrMappedBuffer::borrow() {
 require_usable();
 if (!storage_) throw std::logic_error("GDR buffer has no storage");
 return ReadLease(storage_);
}
void GdrMappedBuffer::close() {
 if (lifetime_->terminal.load(std::memory_order_acquire)) throw std::runtime_error("GDR owner has terminal physical resources");
 if (lifetime_->allocations.load(std::memory_order_acquire) > (storage_ ? 1U : 0U)) throw std::logic_error("GDR shutdown still has consumers of prior storage");
 if (!storage_) {
  closing_ = true;
  return;
 }
 {
  std::lock_guard lock(storage_->mutex);
  if (storage_->readers) throw std::logic_error("GDR shutdown still has CPU consumers");
  closing_ = true;
  storage_->physical->release();
 }
 storage_.reset();
}
GdrMappedBuffer::ReadLease::ReadLease(std::shared_ptr<Storage> storage) : storage_(std::move(storage)) {
 std::lock_guard lock(storage_->mutex);
 ++storage_->readers;
}
GdrMappedBuffer::ReadLease::~ReadLease() noexcept { reset(); }
GdrMappedBuffer::ReadLease::ReadLease(ReadLease&& other) noexcept : storage_(std::move(other.storage_)), recorded_(std::exchange(other.recorded_, false)) {}
GdrMappedBuffer::ReadLease& GdrMappedBuffer::ReadLease::operator=(ReadLease&& other) noexcept {
 if (this != &other) {
  reset();
  storage_ = std::move(other.storage_);
  recorded_ = std::exchange(other.recorded_, false);
 }
 return *this;
}
void GdrMappedBuffer::ReadLease::reset() noexcept {
 if (!storage_) return;
 {
  std::lock_guard lock(storage_->mutex);
  if (!recorded_) storage_->physical->unrecorded = true;
  --storage_->readers;
 }
 storage_.reset();
}
CUdeviceptr GdrMappedBuffer::ReadLease::device_data() const noexcept { return storage_ ? storage_->physical->device_pointer : 0; }
std::size_t GdrMappedBuffer::ReadLease::capacity_bytes() const noexcept { return storage_ ? storage_->physical->capacity : 0; }
void GdrMappedBuffer::ReadLease::record_consumed(CUstream stream) {
 if (!storage_) throw std::logic_error("empty GDR consumer lease");
 std::lock_guard lock(storage_->mutex);
 auto& p = *storage_->physical;
 try {
  Context binding(*p.backend, p.context);
  auto entry = std::find_if(p.consumers.begin(), p.consumers.end(), [stream](const auto& consumer) { return consumer.pending && consumer.stream == stream; });
  if (entry == p.consumers.end()) entry = std::find_if(p.consumers.begin(), p.consumers.end(), [](const auto& consumer) { return !consumer.pending; });
  if (entry == p.consumers.end()) throw std::runtime_error("GDR consumer stream capacity exhausted");
  p.backend->record_event(entry->event, stream);
  entry->stream = stream;
  if (!entry->pending) ++p.pending_consumers;
  entry->pending = true;
  recorded_ = true;
  binding.finish();
 } catch (...) {
  p.unrecorded = true;
  throw;
 }
}
}  // namespace mmltk::frameworks::gpu
