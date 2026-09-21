#pragma once
#include <cuda.h>
#include <cstddef>
#include <memory>
#include <span>
#include <stop_token>
#include <stdexcept>
namespace mmltk::frameworks::gpu {
class GdrTransportUnavailable final : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
namespace detail {
class GdrBufferBackend;
}
// Construct on the actual GPU owner's current context. That context must outlive
// the buffer and every read lease. Each buffer is an independent writable slot.
// The owning system serializes buffer control calls and joins CPU writers before
// shutdown. Read leases may outlive growth and move between owner workers.
// CPU writes return only after GDRCopy's stores/fences; submit CUDA work afterward.
class GdrMappedBuffer final {
 struct Storage;
 struct Lifetime;

public:
 class ReadLease final {
 public:
  ReadLease() noexcept = default;
  ~ReadLease() noexcept;
  ReadLease(ReadLease&&) noexcept;
  ReadLease& operator=(ReadLease&&) noexcept;
  ReadLease(const ReadLease&) = delete;
  ReadLease& operator=(const ReadLease&) = delete;
  [[nodiscard]] CUdeviceptr device_data() const noexcept;
  [[nodiscard]] std::size_t capacity_bytes() const noexcept;
  // Each consuming stream belongs to the allocation owner context. A
  // foreign receiver must finish its own copy before releasing the lease.
  // Record after the last submission on each consuming stream, on an
  // ordinary owner thread. Retain this lease through later consumers such
  // as backward. An unrecorded lease conservatively settles its context.
  void record_consumed(CUstream);

 private:
  friend class GdrMappedBuffer;
  explicit ReadLease(std::shared_ptr<Storage>);
  void reset() noexcept;
  std::shared_ptr<Storage> storage_;
  bool recorded_ = false;
 };
 explicit GdrMappedBuffer(CUcontext owner, std::size_t consumer_stream_capacity = 1);
 GdrMappedBuffer(CUcontext owner, std::size_t consumer_stream_capacity, std::shared_ptr<detail::GdrBufferBackend>);
 ~GdrMappedBuffer() noexcept;
 GdrMappedBuffer(const GdrMappedBuffer&) = delete;
 GdrMappedBuffer& operator=(const GdrMappedBuffer&) = delete;
 // Growth is transactional. Old leases keep old mappings until CPU ownership
 // and recorded GPU consumption both finish. No copy of old contents occurs.
 void ensure_bytes(std::size_t);
 [[nodiscard]] std::size_t capacity_bytes() const noexcept;
 [[nodiscard]] bool uses_dmabuf() const noexcept;
 // False means cancelled before writing. Cancellation during the synchronous
 // copy cannot release ownership early; completion still fences all stores.
 [[nodiscard]] bool write(std::size_t offset, std::span<const std::byte>, std::stop_token = {});
 [[nodiscard]] ReadLease borrow();
 // Explicit owner shutdown; fails if a CPU lease still exists. Destruction
 // retains lease-owned mappings, while this method proves complete release.
 void close();

private:
 void require_usable() const;
 CUcontext context_;
 std::size_t consumer_capacity_;
 std::shared_ptr<detail::GdrBufferBackend> backend_;
 std::shared_ptr<Lifetime> lifetime_;
 std::shared_ptr<Storage> storage_;
 bool closing_ = false;
};
}  // namespace mmltk::frameworks::gpu
