#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>

#include "src/common/io/scoped_fd.h"

namespace mmltk::controller::services {

struct DiagnosticRecord final {
    std::string_view json;
};

enum class DiagnosticSubmitResult : std::uint8_t {
    Accepted,
    Disabled,
    Capacity,
    RecordTooLarge,
    InvalidJson,
    Closed,
    Contended,
};

enum class DiagnosticsCloseMode : std::uint8_t { Flush, Discard };
enum class DiagnosticsExecutionPolicy : std::uint8_t { BackgroundWriter, CallerDriven };
enum class DiagnosticsTerminal : std::uint8_t { Pending, Drained, Failed };

struct DiagnosticsCounters final {
    std::uint64_t accepted = 0U;
    std::uint64_t flushed = 0U;
    std::uint64_t dropped = 0U;
    std::uint64_t write_failures = 0U;
};

class DiagnosticsProducer;

// Application-owned bounded JSONL sink.  A producer is a weak capability: it
// never keeps file I/O or the writer alive after application shutdown.
class DiagnosticsClient final {
   public:
    static constexpr std::size_t kRecordCapacity = 16U * 1024U;
    static constexpr std::size_t kQueueCapacity = 256U;

    DiagnosticsClient() noexcept = default;
    explicit DiagnosticsClient(const std::filesystem::path& path) noexcept;
    explicit DiagnosticsClient(mmltk::common::io::ScopedFd descriptor) noexcept;
    DiagnosticsClient(mmltk::common::io::ScopedFd descriptor, DiagnosticsExecutionPolicy policy) noexcept;
    ~DiagnosticsClient() noexcept;

    DiagnosticsClient(const DiagnosticsClient&) = delete;
    DiagnosticsClient& operator=(const DiagnosticsClient&) = delete;
    DiagnosticsClient(DiagnosticsClient&&) noexcept;
    DiagnosticsClient& operator=(DiagnosticsClient&&) noexcept;

    [[nodiscard]] static DiagnosticsClient from_environment() noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] DiagnosticsProducer producer() const noexcept;
    [[nodiscard]] DiagnosticsCounters counters() const noexcept;
    [[nodiscard]] int terminal_fd() const noexcept;
    void consume_terminal_wake() const noexcept;
    [[nodiscard]] DiagnosticsTerminal terminal() const noexcept;
    void flush() noexcept;
    void close(DiagnosticsCloseMode mode = DiagnosticsCloseMode::Flush) noexcept;
    void wait_closed() noexcept;

   private:
    [[nodiscard]] std::mutex& queue_mutex_for_test() noexcept;
    friend struct DiagnosticsClientTestAccess;
    void initialize(mmltk::common::io::ScopedFd descriptor, DiagnosticsExecutionPolicy policy) noexcept;

    struct State;
    std::shared_ptr<State> state_;
    friend class DiagnosticsProducer;
};

class DiagnosticsProducer final {
   public:
    class Operation final {
       public:
        Operation() noexcept = default;
        [[nodiscard]] bool enabled() const noexcept;
        [[nodiscard]] DiagnosticSubmitResult submit(DiagnosticRecord record) const noexcept;
        // Effect-only instrumentation must never wait for diagnostic capacity.
        [[nodiscard]] DiagnosticSubmitResult try_submit(DiagnosticRecord record) const noexcept;

       private:
        [[nodiscard]] DiagnosticSubmitResult submit(DiagnosticRecord record, bool wait_for_capacity, bool validate = true) const noexcept;
        [[nodiscard]] DiagnosticSubmitResult submit_terminal_encoded(DiagnosticRecord record) const noexcept;
        [[nodiscard]] DiagnosticSubmitResult try_submit_encoded(DiagnosticRecord record) const noexcept {
            return submit(record, false, false);
        }
        friend class RuntimeDiagnosticTarget;
        explicit Operation(std::shared_ptr<DiagnosticsClient::State> state) noexcept;
        std::shared_ptr<DiagnosticsClient::State> state_;
        friend class DiagnosticsProducer;
    };

    DiagnosticsProducer() noexcept = default;
    [[nodiscard]] Operation acquire() const noexcept;
    [[nodiscard]] bool enabled() const noexcept;

   private:
    explicit DiagnosticsProducer(std::weak_ptr<DiagnosticsClient::State> state) noexcept;
    std::weak_ptr<DiagnosticsClient::State> state_;
    friend class DiagnosticsClient;
};

}  // namespace mmltk::controller::services
