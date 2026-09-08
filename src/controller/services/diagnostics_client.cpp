#include "src/controller/services/diagnostics_client.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <utility>

#include "src/common/io/noexcept_io.h"

namespace mmltk::controller::services {
struct DiagnosticsClient::State final {
    struct Record final {
        std::array<char, kRecordCapacity> bytes{};
        std::size_t size = 0U;
    };

    explicit State(mmltk::common::io::ScopedFd adopted, DiagnosticsExecutionPolicy next_policy) noexcept;

    void signal_locked() noexcept;
    void discard_locked() noexcept;
    [[nodiscard]] bool flush_locked() noexcept;
    void fail_writer_locked() noexcept;
    void finish_writer_locked() noexcept;
    void publish_terminal_locked() noexcept;
    [[nodiscard]] bool wait_for_writer_event(bool output_pending) noexcept;
    void run() noexcept;
    void close(DiagnosticsCloseMode mode) noexcept;

    mmltk::common::io::ScopedFd descriptor;
    mmltk::common::io::ScopedFd wake;
    mmltk::common::io::ScopedFd terminal_wake;
    const DiagnosticsExecutionPolicy policy;
    std::array<Record, kQueueCapacity> records{};
    std::mutex mutex;
    std::condition_variable drained;
    std::size_t head = 0U;
    std::size_t size = 0U;
    bool write_active = false;
    bool closing = false;
    bool discard = false;
    bool failed = false;
    std::atomic<DiagnosticsTerminal> terminal{DiagnosticsTerminal::Pending};
    std::atomic<bool> enabled{true};
    std::atomic<std::uint64_t> accepted{0U};
    std::atomic<std::uint64_t> flushed{0U};
    std::atomic<std::uint64_t> dropped{0U};
    std::atomic<std::uint64_t> write_failures{0U};
};

namespace {

[[nodiscard]] bool valid_json_object(const std::string_view json) noexcept {
    if (json.empty() || json.size() > DiagnosticsClient::kRecordCapacity || json.find_first_of("\r\n") != std::string_view::npos)
        return false;
    const std::size_t first = json.find_first_not_of(" \t");
    const std::size_t last = json.find_last_not_of(" \t");
    if (first == std::string_view::npos || json[first] != '{' || json[last] != '}') return false;
    try {
        return nlohmann::json::accept(json);
    } catch (...) { return false; }
}

[[nodiscard]] mmltk::common::io::ScopedFd open_diagnostics_file(const std::filesystem::path& path) noexcept {
    if (path.empty()) return {};
    return mmltk::common::io::ScopedFd{::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NONBLOCK, 0600)};
}

[[nodiscard]] bool make_nonblocking(const int descriptor) noexcept {
    int flags = -1;
    do {
        flags = ::fcntl(descriptor, F_GETFL);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0) return false;
    int result = -1;
    do {
        result = ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

}  // namespace

DiagnosticsClient::State::State(mmltk::common::io::ScopedFd adopted, const DiagnosticsExecutionPolicy next_policy) noexcept
    : descriptor(std::move(adopted)), policy(next_policy) {}

void DiagnosticsClient::State::signal_locked() noexcept {
    const std::uint64_t one = 1U;
    while (::write(wake.get(), &one, sizeof(one)) < 0) {
        if (errno == EINTR) continue;
        return;
    }
}
void DiagnosticsClient::State::discard_locked() noexcept {
    dropped.fetch_add(size, std::memory_order_relaxed);
    head = 0U;
    size = 0U;
    write_active = false;
}
bool DiagnosticsClient::State::flush_locked() noexcept {
    while (size != 0U) {
        const Record& record = records[head];
        const bool wrote = mmltk::common::io::try_write_all_noexcept(descriptor.get(), {record.bytes.data(), record.size}) &&
                           mmltk::common::io::try_write_all_noexcept(descriptor.get(), "\n");
        if (!wrote) {
            write_failures.fetch_add(1U, std::memory_order_relaxed);
            failed = true;
            enabled.store(false, std::memory_order_release);
            discard_locked();
            descriptor.reset();
            drained.notify_all();
            return false;
        }
        head = (head + 1U) % kQueueCapacity;
        --size;
        flushed.fetch_add(1U, std::memory_order_relaxed);
    }
    drained.notify_all();
    return true;
}

void DiagnosticsClient::State::fail_writer_locked() noexcept {
    write_failures.fetch_add(1U, std::memory_order_relaxed);
    failed = true;
    enabled.store(false, std::memory_order_release);
    discard_locked();
    descriptor.reset();
    drained.notify_all();
}

void DiagnosticsClient::State::finish_writer_locked() noexcept {
    descriptor.reset();
    enabled.store(false, std::memory_order_release);
    drained.notify_all();
}
void DiagnosticsClient::State::publish_terminal_locked() noexcept {
    if (terminal.load(std::memory_order_relaxed) != DiagnosticsTerminal::Pending) return;
    // This owns the final transition.  All mutable I/O state settles before
    // the immutable terminal fact is visible; writer execution only releases
    // its self-retained State after this point.
    discard_locked();
    descriptor.reset();
    enabled.store(false, std::memory_order_release);
    terminal.store(failed ? DiagnosticsTerminal::Failed : DiagnosticsTerminal::Drained, std::memory_order_release);
    drained.notify_all();
    const std::uint64_t one = 1U;
    while (::write(terminal_wake.get(), &one, sizeof(one)) < 0) {
        if (errno == EINTR) continue;
        break;
    }
}

bool DiagnosticsClient::State::wait_for_writer_event(const bool output_pending) noexcept {
    std::array<pollfd, 2U> events{{
        {.fd = wake.get(), .events = POLLIN, .revents = 0},
        {.fd = output_pending ? descriptor.get() : -1, .events = POLLOUT, .revents = 0},
    }};
    const nfds_t event_count = output_pending ? nfds_t{2U} : nfds_t{1U};
    int ready = -1;
    do {
        ready = ::poll(events.data(), event_count, -1);
    } while (ready < 0 && errno == EINTR);
    if (ready < 0 || (events[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return false;
    if ((events[0].revents & POLLIN) == 0) return true;

    std::uint64_t ignored = 0U;
    ssize_t consumed = -1;
    do {
        consumed = ::read(wake.get(), &ignored, sizeof(ignored));
    } while (consumed < 0 && errno == EINTR);
    return consumed == static_cast<ssize_t>(sizeof(ignored)) || (consumed < 0 && errno == EAGAIN);
}

void DiagnosticsClient::State::run() noexcept {
    sigset_t blocked_signals{};
    if (::sigemptyset(&blocked_signals) != 0 || ::sigaddset(&blocked_signals, SIGPIPE) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &blocked_signals, nullptr) != 0) {
        std::lock_guard lock(mutex);
        fail_writer_locked();
        publish_terminal_locked();
        return;
    }
    std::size_t output_offset = 0U;
    for (;;) {
        Record* active = nullptr;
        {
            std::lock_guard lock(mutex);
            if (discard) {
                discard_locked();
                finish_writer_locked();
                publish_terminal_locked();
                return;
            }
            if (!write_active && size != 0U) {
                write_active = true;
                output_offset = 0U;
            }
            if (write_active) {
                active = &records[head];
            } else if (closing || failed) {
                finish_writer_locked();
                publish_terminal_locked();
                return;
            }
        }

        if (active == nullptr) {
            if (wait_for_writer_event(false)) continue;
            std::lock_guard lock(mutex);
            fail_writer_locked();
            publish_terminal_locked();
            return;
        }

        const bool writing_record = output_offset < active->size;
        const char* const bytes = writing_record ? active->bytes.data() + output_offset : "\n";
        const std::size_t remaining = writing_record ? active->size - output_offset : 1U;
        ssize_t written = -1;
        do {
            written = ::write(descriptor.get(), bytes, remaining);
        } while (written < 0 && errno == EINTR);
        if (written > 0) {
            output_offset += static_cast<std::size_t>(written);
            if (output_offset != active->size + 1U) continue;

            std::lock_guard lock(mutex);
            head = (head + 1U) % kQueueCapacity;
            --size;
            write_active = false;
            flushed.fetch_add(1U, std::memory_order_relaxed);
            drained.notify_all();
            if (discard) {
                discard_locked();
                finish_writer_locked();
                publish_terminal_locked();
                return;
            }
            if (closing && size == 0U) {
                finish_writer_locked();
                publish_terminal_locked();
                return;
            }
            continue;
        }

        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            {
                std::lock_guard lock(mutex);
                if (discard) continue;
                if (closing) {
                    fail_writer_locked();
                    publish_terminal_locked();
                    return;
                }
            }
            if (wait_for_writer_event(true)) continue;
        }

        std::lock_guard lock(mutex);
        fail_writer_locked();
        publish_terminal_locked();
        return;
    }
}
void DiagnosticsClient::State::close(const DiagnosticsCloseMode mode) noexcept {
    {
        std::lock_guard lock(mutex);
        if (!closing) {
            closing = true;
            discard = mode == DiagnosticsCloseMode::Discard;
            enabled.store(false, std::memory_order_release);
        }
        if (policy == DiagnosticsExecutionPolicy::CallerDriven) {
            if (discard)
                discard_locked();
            else
                static_cast<void>(flush_locked());
            finish_writer_locked();
            publish_terminal_locked();
            return;
        }
        signal_locked();
    }
}

DiagnosticsClient::DiagnosticsClient(const std::filesystem::path& path) noexcept {
    initialize(open_diagnostics_file(path), DiagnosticsExecutionPolicy::BackgroundWriter);
}

DiagnosticsClient::DiagnosticsClient(mmltk::common::io::ScopedFd descriptor) noexcept {
    initialize(std::move(descriptor), DiagnosticsExecutionPolicy::BackgroundWriter);
}

DiagnosticsClient::DiagnosticsClient(mmltk::common::io::ScopedFd descriptor, const DiagnosticsExecutionPolicy policy) noexcept {
    initialize(std::move(descriptor), policy);
}

void DiagnosticsClient::initialize(mmltk::common::io::ScopedFd descriptor, const DiagnosticsExecutionPolicy policy) noexcept {
    if (descriptor.get() < 0) return;
    // Every descriptor admitted by this sink has the same nonblocking I/O
    // contract. CallerDriven uses the synchronous owner path, so retaining a
    // blocking descriptor would violate the same teardown guarantee.
    if (!make_nonblocking(descriptor.get())) return;
    try {
        state_ = std::make_shared<State>(std::move(descriptor), policy);
    } catch (...) { return; }
    const int terminal_wake = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (terminal_wake < 0) {
        state_.reset();
        return;
    }
    state_->terminal_wake.reset(terminal_wake);
    if (policy == DiagnosticsExecutionPolicy::BackgroundWriter) {
        const int wake = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
        if (wake < 0) {
            state_.reset();
            return;
        }
        state_->wake.reset(wake);
        try {
            // The execution's capture is its sole lifetime owner after the
            // client releases its handle.  Detaching at creation makes every
            // client teardown path nonblocking and prevents std::thread's
            // joinable-destructor termination contract.
            std::thread([state = state_]() noexcept { state->run(); }).detach();
        } catch (...) {
            std::lock_guard lock(state_->mutex);
            state_->failed = true;
            state_->publish_terminal_locked();
        }
    }
}

DiagnosticsClient::~DiagnosticsClient() noexcept { close(DiagnosticsCloseMode::Discard); }
DiagnosticsClient::DiagnosticsClient(DiagnosticsClient&& other) noexcept : state_(std::move(other.state_)) {}
DiagnosticsClient& DiagnosticsClient::operator=(DiagnosticsClient&& other) noexcept {
    if (this == &other) return *this;
    close(DiagnosticsCloseMode::Discard);
    state_ = std::move(other.state_);
    return *this;
}
DiagnosticsClient DiagnosticsClient::from_environment() noexcept {
    const char* const path = std::getenv("MMLTK_GUI_TRACE_FILE");
    return path != nullptr && *path != '\0' ? DiagnosticsClient{std::filesystem::path{path}} : DiagnosticsClient{};
}
bool DiagnosticsClient::enabled() const noexcept { return state_ != nullptr && state_->enabled.load(std::memory_order_acquire); }
DiagnosticsProducer DiagnosticsClient::producer() const noexcept { return DiagnosticsProducer{state_}; }
DiagnosticsCounters DiagnosticsClient::counters() const noexcept {
    if (state_ == nullptr) return {};
    return {.accepted = state_->accepted.load(),
            .flushed = state_->flushed.load(),
            .dropped = state_->dropped.load(),
            .write_failures = state_->write_failures.load()};
}
int DiagnosticsClient::terminal_fd() const noexcept { return state_ == nullptr ? -1 : state_->terminal_wake.get(); }
void DiagnosticsClient::consume_terminal_wake() const noexcept {
    if (state_ == nullptr || terminal() == DiagnosticsTerminal::Pending) return;
    std::uint64_t ignored = 0U;
    while (::read(state_->terminal_wake.get(), &ignored, sizeof(ignored)) < 0 && errno == EINTR) {}
}
DiagnosticsTerminal DiagnosticsClient::terminal() const noexcept {
    return state_ == nullptr ? DiagnosticsTerminal::Drained : state_->terminal.load(std::memory_order_acquire);
}
void DiagnosticsClient::flush() noexcept {
    if (state_ == nullptr) return;
    if (state_->policy == DiagnosticsExecutionPolicy::CallerDriven) {
        std::lock_guard lock(state_->mutex);
        if (!state_->flush_locked()) state_->publish_terminal_locked();
        return;
    }
    std::unique_lock lock(state_->mutex);
    if (state_->closing || state_->failed || state_->size == 0U) return;
    state_->signal_locked();
    state_->drained.wait(lock, [&] { return state_->size == 0U || state_->failed; });
}
void DiagnosticsClient::close(const DiagnosticsCloseMode mode) noexcept {
    if (state_ != nullptr) state_->close(mode);
}
void DiagnosticsClient::wait_closed() noexcept {
    if (state_ == nullptr) return;
    std::unique_lock lock{state_->mutex};
    state_->drained.wait(lock, [this] { return state_->terminal.load(std::memory_order_acquire) != DiagnosticsTerminal::Pending; });
}

DiagnosticsProducer::Operation DiagnosticsProducer::acquire() const noexcept { return Operation{state_.lock()}; }
bool DiagnosticsProducer::enabled() const noexcept { return acquire().enabled(); }
DiagnosticsProducer::DiagnosticsProducer(std::weak_ptr<DiagnosticsClient::State> state) noexcept : state_(std::move(state)) {}
DiagnosticsProducer::Operation::Operation(std::shared_ptr<DiagnosticsClient::State> state) noexcept : state_(std::move(state)) {}
bool DiagnosticsProducer::Operation::enabled() const noexcept {
    return state_ != nullptr && state_->enabled.load(std::memory_order_acquire);
}
DiagnosticSubmitResult DiagnosticsProducer::Operation::submit(const DiagnosticRecord record) const noexcept { return submit(record, true); }
DiagnosticSubmitResult DiagnosticsProducer::Operation::try_submit(const DiagnosticRecord record) const noexcept {
    return submit(record, false);
}
std::mutex& DiagnosticsClient::queue_mutex_for_test() noexcept { return state_->mutex; }

DiagnosticSubmitResult DiagnosticsProducer::Operation::submit(const DiagnosticRecord record, const bool wait_for_capacity,
                                                              const bool validate) const noexcept {
    if (state_ == nullptr || !state_->enabled.load(std::memory_order_acquire)) return DiagnosticSubmitResult::Disabled;
    if (record.json.size() > DiagnosticsClient::kRecordCapacity) {
        state_->dropped.fetch_add(1U, std::memory_order_relaxed);
        return DiagnosticSubmitResult::RecordTooLarge;
    }
    if (validate && !valid_json_object(record.json)) {
        state_->dropped.fetch_add(1U, std::memory_order_relaxed);
        return DiagnosticSubmitResult::InvalidJson;
    }
    std::unique_lock lock(state_->mutex, std::defer_lock);
    if (wait_for_capacity) {
        lock.lock();
    } else if (!lock.try_lock()) {
        state_->dropped.fetch_add(1U, std::memory_order_relaxed);
        return DiagnosticSubmitResult::Contended;
    }
    if (state_->closing || state_->failed || state_->descriptor.get() < 0) return DiagnosticSubmitResult::Closed;
    if (state_->size == DiagnosticsClient::kQueueCapacity) {
        if (!wait_for_capacity || state_->policy == DiagnosticsExecutionPolicy::CallerDriven) {
            state_->dropped.fetch_add(1U, std::memory_order_relaxed);
            return DiagnosticSubmitResult::Capacity;
        }
        state_->drained.wait(lock, [&] {
            return state_->size != DiagnosticsClient::kQueueCapacity || state_->closing || state_->failed || state_->descriptor.get() < 0;
        });
        if (state_->closing || state_->failed || state_->descriptor.get() < 0) return DiagnosticSubmitResult::Closed;
    }
    auto& target = state_->records[(state_->head + state_->size) % DiagnosticsClient::kQueueCapacity];
    std::memcpy(target.bytes.data(), record.json.data(), record.json.size());
    target.size = record.json.size();
    ++state_->size;
    state_->accepted.fetch_add(1U, std::memory_order_relaxed);
    if (state_->policy == DiagnosticsExecutionPolicy::BackgroundWriter) state_->signal_locked();
    return DiagnosticSubmitResult::Accepted;
}

}  // namespace mmltk::controller::services
