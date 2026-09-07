#pragma once
#include "src/backend/data/data_loading_options.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <optional>
#include "src/frameworks/gpu/device_execution.h"
#include <string>

#include "src/backend/data/compiled_dataset.h"
#include "src/common/concurrency/worker_pool.h"

namespace mmltk::backend::data {

// A system-local physical image loader. Policy owners assign bounded slots and
// retain them until their consumers finish. No image is copied between device
// buffers just to adapt a consumer's batch layout.
class CompiledImageStream final {
   public:
    class Buffer final {
       public:
        explicit Buffer(bool pinned, const mmltk::common::system::ExecutionPlacement* placement = nullptr, bool mapped = false);
        ~Buffer();
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        [[nodiscard]] void* data() const noexcept;
        [[nodiscard]] std::size_t capacity_bytes() const noexcept;
        [[nodiscard]] bool owns_allocation() const noexcept;

       private:
        friend class CompiledImageStream;
        void ensure_bytes(std::size_t bytes);
        void begin_write();
        void write(std::size_t offset, std::span<const std::byte>);
        void publish(void* stream);
        void consumed(void* stream);
        [[nodiscard]] int reset() noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    struct Config {
        std::size_t slots = 1;
        std::size_t workers = 1;
        int device = 0;
        std::string cpu_affinity{};
        bool prefault = false;
        DataLoadingOptions loading{};
        std::optional<mmltk::frameworks::gpu::DeviceExecution> execution{};
    };
    struct ReadObserver {
        void* context = nullptr;
        // Runs on an I/O worker before touching file-backed pixels. False
        // cancels the read, including an acceptance gate superseded by a view.
        bool (*before)(void*, std::size_t) = nullptr;
        void (*complete)(void*, std::size_t, std::exception_ptr, bool) noexcept = nullptr;
    };
    struct CompletionObserver {
        void* context = nullptr;
        void (*complete)(void*, std::size_t, std::exception_ptr) noexcept = nullptr;
    };

    explicit CompiledImageStream(Config config);
    ~CompiledImageStream();
    CompiledImageStream(const CompiledImageStream&) = delete;
    CompiledImageStream& operator=(const CompiledImageStream&) = delete;

    // Capture the actual owner context on its GPU worker, never during Explore model construction.
    void bind_current_context();
    [[nodiscard]] const mmltk::frameworks::gpu::DeviceExecution& execution() const noexcept;
    void prepare_host(std::size_t slot, std::size_t bytes);
    void prepare_metadata(std::size_t slot, std::size_t bytes);
    [[nodiscard]] const Buffer& metadata_storage(std::size_t slot);
    void prepare_images(std::size_t slot, std::size_t bytes);
    [[nodiscard]] std::span<const std::byte> host_images(std::size_t slot);
    void prepare_device(std::size_t index, std::size_t bytes);
    [[nodiscard]] const Buffer& host_storage(std::size_t slot) const;
    [[nodiscard]] const Buffer& device_storage(std::size_t index) const;
    [[nodiscard]] mmltk::common::concurrency::WorkerPool& workers() noexcept;
    // Copies the compact read list into reused slot storage. At most one read
    // job per slot is admitted. Visible work can cancel queued prefetch jobs.
    void submit(std::size_t slot, const CompiledDataset& source, std::span<const CompiledImageRead> reads, ReadObserver observer,
                CompletionObserver transfer = {nullptr, nullptr});
    void cancel_reads() noexcept;
    void cancel_read(std::size_t slot) noexcept;
    void wait_reads();
    [[nodiscard]] bool wait_read(std::size_t slot);
    void wait_transfer(std::size_t slot);
    void handoff(std::size_t slot, void* stream);
    void release(std::size_t slot, void* stream, CompletionObserver observer);
    // Borrow only the slot's completion event for work that consumes no input
    // image, including retained Explore tiles after a cancelled read.
    void fence(std::size_t slot, void* stream, CompletionObserver observer);
    void synchronize();
    // Terminal physical release, called before the owning CUDA context leaves scope.
    void close();
    [[nodiscard]] int reset_storage() noexcept;
    [[nodiscard]] bool owns_allocation() const noexcept;
    [[nodiscard]] bool owns_resources() const noexcept;

   private:
    struct Impl;
    void read_slot(std::size_t slot) noexcept;
    void upload(std::size_t slot, std::size_t bytes, void* stream);
    struct Retention;
    std::unique_ptr<Retention> retention_;
    std::shared_ptr<Impl> impl_;
};

}  // namespace mmltk::backend::data
