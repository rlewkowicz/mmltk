#include "src/controller/services/train_process_client.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <inplace_vector>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/common/io/event_fd.h"
#include "src/common/io/scoped_fd.h"
#include "src/controller/services/train_command.h"
#include "src/frameworks/process/subprocess_utils.h"

namespace mmltk::controller::services {
namespace {

constexpr std::size_t kProgressDocumentLimit = std::size_t{64U} * 1024U;
constexpr std::size_t kProgressEdgeReadBudget = std::size_t{16U} * 1024U;

[[nodiscard]] std::string bounded_error(std::string value) { return mmltk::controller::contracts::bounded_compute_error(std::move(value)); }

void validate_progress_fields(const std::string& status, const std::string& checkpoint) {
    if (!mmltk::controller::contracts::valid_compute_text(status, mmltk::controller::contracts::kComputeStatusCapacity)) {
        throw std::runtime_error("train progress status exceeds fixed capacity");
    }
    if (!mmltk::controller::contracts::valid_compute_text(checkpoint, mmltk::controller::contracts::kComputePathCapacity)) {
        throw std::runtime_error("train checkpoint path exceeds fixed capacity");
    }
}

[[nodiscard]] int event_descriptor() {
    const int descriptor = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "failed to create train control eventfd");
    return descriptor;
}

[[nodiscard]] int timer_descriptor() {
    const int descriptor = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "failed to create train escalation timerfd");
    return descriptor;
}

[[nodiscard]] int lifecycle_descriptor(const int timer) {
    const int descriptor = ::epoll_create1(EPOLL_CLOEXEC);
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "failed to create train lifecycle epoll fd");
    epoll_event event{.events = EPOLLIN, .data = {.u64 = 0U}};
    if (::epoll_ctl(descriptor, EPOLL_CTL_ADD, timer, &event) == 0) return descriptor;
    const int error = errno;
    ::close(descriptor);
    throw std::system_error(error, std::generic_category(), "failed to register train escalation timer");
}

void signal_group(const pid_t group, const int signal) noexcept {
    if (group > 0 && ::kill(-group, signal) != 0 && errno != ESRCH) return;
}

void arm_escalation(const int descriptor, const std::chrono::milliseconds delay_value) {
    const auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(delay_value).count();
    const itimerspec timer{.it_interval = {}, .it_value = {.tv_sec = delay / 1'000'000'000LL, .tv_nsec = delay % 1'000'000'000LL}};
    if (::timerfd_settime(descriptor, 0, &timer, nullptr) != 0) {
        throw std::system_error(errno, std::generic_category(), "failed to arm train escalation timerfd");
    }
}

[[nodiscard]] int progress_descriptor(const std::filesystem::path& directory, int& watch) {
    const int descriptor = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "failed to create train progress inotify fd");
    watch = ::inotify_add_watch(descriptor, directory.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF);
    if (watch >= 0) return descriptor;
    const int error = errno;
    ::close(descriptor);
    throw std::system_error(error, std::generic_category(), "failed to register train progress watch");
}

[[nodiscard]] std::string bounded_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return {};
    if (size > kProgressDocumentLimit) throw std::runtime_error("train progress document exceeds its fixed admission limit");
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    result.resize(static_cast<std::size_t>(input.gcount()));
    return result;
}

template <class T>
void read_json_value(const nlohmann::json& object, const char* key, T& value) {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) return;
    try {
        value = found->get<T>();
    } catch (const nlohmann::json::exception&) {}
}

[[nodiscard]] bool consume_progress_edges(const int descriptor, const int watch) {
    std::array<char, 4096U> buffer{};
    std::size_t consumed = 0U;
    bool relevant = false;
    while (consumed < kProgressEdgeReadBudget) {
        const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return relevant;
            throw std::system_error(errno, std::generic_category(), "failed to consume train progress edge");
        }
        if (count == 0) return relevant;
        consumed += static_cast<std::size_t>(count);
        for (std::size_t offset = 0U; offset < static_cast<std::size_t>(count);) {
            const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
            if (event->wd == watch && event->len != 0U && (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) != 0U) {
                const std::string_view name{event->name};
                relevant = relevant || name == "progress.json" || name == "results.json" || name == "log.txt";
            }
            offset += sizeof(inotify_event) + event->len;
        }
    }
    return relevant;
}

}  // namespace

bool TrainProcessClient::State::tracks(const pid_t candidate) const noexcept {
    return std::ranges::any_of(group_members, [candidate](const GroupMember& member) { return member.pid == candidate; });
}
void TrainProcessClient::State::refresh_group_members() {
    if (group <= 0) {
        group_members.clear();
        group_tracking = false;
        group_quiesced = true;
        return;
    }
    std::unique_ptr<DIR, int (*)(DIR*)> directory{::opendir("/proc"), &::closedir};
    if (!directory) throw std::system_error(errno, std::generic_category(), "failed to inspect train process group");
    bool found = false;
    while (const dirent* entry = ::readdir(directory.get())) {
        char* end = nullptr;
        errno = 0;
        const long encoded = std::strtol(entry->d_name, &end, 10);
        if (errno != 0 || end == entry->d_name || *end != '\0' || encoded <= 0 || encoded > std::numeric_limits<pid_t>::max()) continue;
        const pid_t candidate = static_cast<pid_t>(encoded);
        std::array<char, 64U> path{};
        if (const int length = std::snprintf(path.data(), path.size(), "/proc/%ld/stat", encoded);
            length <= 0 || static_cast<std::size_t>(length) >= path.size())
            continue;
        mmltk::common::io::ScopedFd stat_fd(::open(path.data(), O_RDONLY | O_CLOEXEC));
        if (stat_fd.get() < 0) continue;
        std::array<char, 4096U> stat{};
        const ssize_t count = ::read(stat_fd.get(), stat.data(), stat.size() - 1U);
        if (count <= 0) continue;
        stat[static_cast<std::size_t>(count)] = '\0';
        const char* closing = std::strrchr(stat.data(), ')');
        if (!closing || closing[1] == '\0' || closing[2] == 'Z') continue;
        char* cursor = nullptr;
        errno = 0;
        static_cast<void>(std::strtol(closing + 3, &cursor, 10));
        if (cursor == closing + 3 || errno != 0) continue;
        const char* group_begin = cursor;
        errno = 0;
        const long candidate_group = std::strtol(group_begin, &cursor, 10);
        if (cursor == group_begin || errno != 0 || candidate_group != group) continue;
        found = true;
        if (tracks(candidate)) continue;
        if (group_members.size() == group_members.capacity()) throw std::runtime_error("train process group exceeds fixed member capacity");
        const int descriptor = static_cast<int>(::syscall(SYS_pidfd_open, candidate, 0U));
        if (descriptor < 0) {
            if (errno == ESRCH) continue;
            throw std::system_error(errno, std::generic_category(), "failed to watch train group member");
        }
        mmltk::common::io::ScopedFd member(descriptor);
        epoll_event event{.events = EPOLLIN, .data = {.u64 = static_cast<std::uint64_t>(candidate)}};
        if (::epoll_ctl(lifecycle_fd.get(), EPOLL_CTL_ADD, member.get(), &event) != 0)
            throw std::system_error(errno, std::generic_category(), "failed to register train group member");
        group_members.push_back(GroupMember{.pid = candidate, .pidfd = std::move(member)});
    }
    group_tracking = found || !group_members.empty();
    group_quiesced = !group_tracking;
}
bool TrainProcessClient::State::consume_lifecycle() {
    std::array<epoll_event, 64U> events{};
    bool escalated = false;
    for (;;) {
        const int count = ::epoll_wait(lifecycle_fd.get(), events.data(), static_cast<int>(events.size()), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::system_error(errno, std::generic_category(), "failed to consume train lifecycle readiness");
        if (count == 0) break;
        for (int i = 0; i < count; ++i) {
            const auto id = events[static_cast<std::size_t>(i)].data.u64;
            if (id == 0U) {
                mmltk::common::io::drain_event_fd(escalation_fd.get());
                if (!kill_sent) signal_group(group, SIGKILL);
                kill_sent = true;
                escalated = true;
                continue;
            }
            const auto found =
                std::ranges::find_if(group_members, [id](const GroupMember& member) { return member.pid == static_cast<pid_t>(id); });
            if (found != group_members.end()) group_members.erase(found);
        }
    }
    if (group_tracking && group_members.empty()) refresh_group_members();
    return escalated;
}

TrainProcessClient::TrainProcessClient() noexcept = default;
TrainProcessClient::TrainProcessClient(State state) noexcept : state_(std::move(state)) {}
TrainProcessClient::~TrainProcessClient() noexcept { force_reap(); }
TrainProcessClient::TrainProcessClient(TrainProcessClient&&) noexcept = default;

TrainProcessClient TrainProcessClient::launch(const mmltk::backend::models::rfdetr::TrainRequest& request,
                                              const std::filesystem::path& cli_path, const std::string_view fallback_preset_name,
                                              const TrainProcessOptions options) {
    if (options.escalation_delay <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("train escalation delay must be positive");
    }
    std::filesystem::create_directories(request.output_dir);
    std::error_code cleanup_error;
    std::filesystem::remove(request.output_dir / "progress.json", cleanup_error);
    std::filesystem::remove(request.output_dir / "results.json", cleanup_error);
    std::filesystem::remove(request.output_dir / "log.txt", cleanup_error);
    const auto arguments = build_train_command_arguments(request, fallback_preset_name);
    std::vector<std::string> argv_storage;
    argv_storage.reserve(arguments.size() + 1U);
    argv_storage.push_back(cli_path.string());
    argv_storage.insert(argv_storage.end(), arguments.begin(), arguments.end());
    const mmltk::frameworks::process::ArgvBuffer argv(std::move(argv_storage));
    auto child = mmltk::frameworks::process::spawn_captured_child_process("local training", [&](const int output, const int setup) {
        mmltk::frameworks::process::prepare_captured_output_child(output, setup);
        ::execv(argv.program(), argv.data());
        mmltk::frameworks::process::fail_child_setup(setup, mmltk::frameworks::process::ChildSetupStage::Exec);
    });
    try {
        const pid_t child_pid = child.pid();
        if (::setpgid(child_pid, child_pid) != 0 && errno != EACCES && errno != ESRCH) {
            throw std::system_error(errno, std::generic_category(), "failed to establish train process group");
        }
        State state;
        state.pid = child_pid;
        state.group = child_pid;
        state.pidfd.reset(child.release_pidfd());
        state.stdout_fd.reset(child.release_stdout_fd());
        state.setup_fd.reset(child.release_setup_error_fd());
        state.output_directory = request.output_dir;
        state.progress_fd.reset(progress_descriptor(state.output_directory, state.progress_watch));
        state.control_fd.reset(event_descriptor());
        state.escalation_fd.reset(timer_descriptor());
        state.lifecycle_fd.reset(lifecycle_descriptor(state.escalation_fd.get()));
        state.escalation_delay = options.escalation_delay;
        state.refresh_group_members();
        static_cast<void>(child.release_pid());
        return TrainProcessClient{std::move(state)};
    } catch (...) {
        const pid_t pid = child.pid();
        signal_group(pid, SIGKILL);
        child.close();
        throw;
    }
}

bool TrainProcessClient::active() const noexcept { return state_ && !state_->terminal_consumed; }
std::int32_t TrainProcessClient::process_group_id() const noexcept { return state_ ? state_->group : -1; }
int TrainProcessClient::stdout_fd() const noexcept { return state_ ? state_->stdout_fd.get() : -1; }
int TrainProcessClient::pid_fd() const noexcept { return state_ ? state_->pidfd.get() : -1; }
int TrainProcessClient::setup_error_fd() const noexcept { return state_ ? state_->setup_fd.get() : -1; }
int TrainProcessClient::progress_fd() const noexcept { return state_ ? state_->progress_fd.get() : -1; }
int TrainProcessClient::control_fd() const noexcept { return state_ ? state_->control_fd.get() : -1; }
int TrainProcessClient::escalation_fd() const noexcept { return state_ ? state_->lifecycle_fd.get() : -1; }

bool TrainProcessClient::request_stop(const bool force) noexcept {
    if (!active()) return false;
    state_->stop_requested = true;
    state_->force_requested = state_->force_requested || force;
    return mmltk::common::io::signal_event_fd(state_->control_fd.get());
}

bool TrainProcessClient::consume_stop_request() {
    if (!active()) return false;
    mmltk::common::io::drain_event_fd(state_->control_fd.get());
    if (state_->force_requested) {
        if (!state_->kill_sent) signal_group(state_->group, SIGKILL);
        state_->kill_sent = true;
    } else if (!state_->term_sent) {
        signal_group(state_->group, SIGTERM);
        state_->term_sent = true;
    }
    arm_escalation(state_->escalation_fd.get(), state_->escalation_delay);
    return true;
}

bool TrainProcessClient::consume_escalation() {
    if (!active()) return false;
    return state_->consume_lifecycle();
}

void TrainProcessClient::consume_output(std::string& output, const std::size_t budget) {
    if (!state_ || state_->stdout_fd.get() < 0 || budget == 0U) return;
    std::array<char, 4096U> bytes{};
    std::size_t read = 0U;
    while (read < budget) {
        const ssize_t count = ::read(state_->stdout_fd.get(), bytes.data(), std::min(bytes.size(), budget - read));
        if (count > 0) {
            output.append(bytes.data(), static_cast<std::size_t>(count));
            read += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            state_->stdout_fd.reset();
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        throw std::system_error(errno, std::generic_category(), "failed to read train output");
    }
}

std::optional<TrainProcessProgress> TrainProcessClient::consume_progress() {
    if (!active() || !consume_progress_edges(state_->progress_fd.get(), state_->progress_watch)) return std::nullopt;
    if (state_->progress_sequence == std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("train progress sequence exhausted");
    const auto progress = nlohmann::json::parse(bounded_file(state_->output_directory / "progress.json"), nullptr, false);
    const auto result = nlohmann::json::parse(bounded_file(state_->output_directory / "results.json"), nullptr, false);
    if (!progress.is_object() && !result.is_object()) return std::nullopt;
    std::string status = "training";
    std::uint64_t completed = 0U;
    std::uint64_t total = 0U;
    std::int64_t epoch = -1;
    std::uint64_t total_epochs = 0U;
    if (progress.is_object()) {
        read_json_value(progress, "phase", status);
        read_json_value(progress, "completed_batches", completed);
        read_json_value(progress, "total_batches", total);
        read_json_value(progress, "epoch", epoch);
        read_json_value(progress, "total_epochs", total_epochs);
    }
    if (total == 0U && total_epochs != 0U) {
        total = total_epochs;
        completed = epoch < 0 ? 0U : std::min<std::uint64_t>(static_cast<std::uint64_t>(epoch) + 1U, total);
    }
    if (total != 0U) completed = std::min(completed, total);
    std::string checkpoint;
    if (progress.is_object()) read_json_value(progress, "checkpoint_path", checkpoint);
    if (checkpoint.empty() && result.is_object()) {
        read_json_value(result, "best_checkpoint", checkpoint);
        if (checkpoint.empty()) read_json_value(result, "checkpoint", checkpoint);
    }
    validate_progress_fields(status, checkpoint);
    ++state_->progress_sequence;
    return TrainProcessProgress{
        .progress = {.sequence = state_->progress_sequence, .completed = completed, .total = total, .status = std::move(status)},
        .checkpoint_path = std::move(checkpoint)};
}

std::optional<TrainProcessExit> TrainProcessClient::consume_exit() {
    if (!active()) return std::nullopt;
    int status = 0;
    if (!state_->reaped) {
        pid_t result;
        do {
            result = ::waitpid(state_->pid, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == 0) return std::nullopt;
        if (result != state_->pid) throw std::system_error(errno, std::generic_category(), "failed to reap train process");
        state_->reaped = true;
        state_->wait_status = status;
        state_->pid = -1;
        state_->pidfd.reset();
    }
    static_cast<void>(state_->consume_lifecycle());
    if (!state_->group_tracking) {
        signal_group(state_->group, SIGKILL);
        state_->kill_sent = true;
        state_->refresh_group_members();
    }
    if (!state_->group_quiesced) return std::nullopt;
    const auto setup = mmltk::frameworks::process::read_child_setup_failure(state_->setup_fd.get());
    state_->setup_fd.reset();
    std::optional<TrainProcessProgress> final_progress;
    if (state_->progress_sequence != std::numeric_limits<std::uint64_t>::max()) {
        const auto progress = nlohmann::json::parse(bounded_file(state_->output_directory / "progress.json"), nullptr, false);
        if (progress.is_object()) {
            std::string status_text = "training";
            std::uint64_t completed = 0U;
            std::uint64_t total = 0U;
            std::string checkpoint;
            read_json_value(progress, "phase", status_text);
            read_json_value(progress, "completed_batches", completed);
            read_json_value(progress, "total_batches", total);
            read_json_value(progress, "checkpoint_path", checkpoint);
            if (total != 0U) completed = std::min(completed, total);
            validate_progress_fields(status_text, checkpoint);
            final_progress = TrainProcessProgress{.progress = {.sequence = ++state_->progress_sequence,
                                                               .completed = completed,
                                                               .total = total,
                                                               .status = std::move(status_text)},
                                                  .checkpoint_path = std::move(checkpoint)};
        }
    }
    TrainProcessExit exit{.outcome = TrainProcessExitOutcome::Failed,
                          .wait_status = state_->wait_status,
                          .setup_failure = setup.has_value(),
                          .final_progress = std::move(final_progress),
                          .error = {}};
    if (setup) exit.error = bounded_error(mmltk::frameworks::process::format_child_setup_failure(*setup, "local training"));
    if (setup)
        exit.outcome = TrainProcessExitOutcome::Failed;
    else if (state_->stop_requested)
        exit.outcome = TrainProcessExitOutcome::Cancelled;
    else if (WIFEXITED(state_->wait_status) && WEXITSTATUS(state_->wait_status) == 0)
        exit.outcome = TrainProcessExitOutcome::Succeeded;
    else {
        exit.outcome = TrainProcessExitOutcome::Failed;
        exit.error = bounded_error("local training exited unsuccessfully");
    }
    state_->terminal_consumed = true;
    state_->group = -1;
    state_->progress_fd.reset();
    state_->control_fd.reset();
    state_->escalation_fd.reset();
    state_->lifecycle_fd.reset();
    return exit;
}

TrainProcessRunResult TrainProcessClient::Run(TrainProcessStopToken token, const TrainProcessProgressObserver progress) {
    if (!active()) throw std::logic_error("cannot run an inactive train process");
    mmltk::common::io::ScopedFd stop_fd(token.release());
    std::string output;
    for (;;) {
        if (const auto update = consume_progress()) progress(*update);
        if (auto terminal = consume_exit()) {
            if (terminal->final_progress) progress(*terminal->final_progress);
            return {.terminal = std::move(*terminal), .output = std::move(output)};
        }

        std::array<pollfd, 6U> ready{{
            {.fd = stdout_fd(), .events = POLLIN, .revents = 0},
            {.fd = pid_fd(), .events = POLLIN, .revents = 0},
            {.fd = progress_fd(), .events = POLLIN, .revents = 0},
            {.fd = control_fd(), .events = POLLIN, .revents = 0},
            {.fd = escalation_fd(), .events = POLLIN, .revents = 0},
            {.fd = stop_fd.get(), .events = POLLIN, .revents = 0},
        }};
        int count;
        do {
            count = ::poll(ready.data(), static_cast<nfds_t>(ready.size()), -1);
        } while (count < 0 && errno == EINTR);
        if (count < 0) throw std::system_error(errno, std::generic_category(), "failed to wait for train process readiness");
        for (const pollfd& descriptor : ready) {
            if ((descriptor.revents & POLLNVAL) != 0) { throw std::runtime_error("train readiness source became invalid"); }
        }
        if ((ready[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            const std::size_t retained = output.size();
            if (retained < kTrainProcessReadBudget) consume_output(output, kTrainProcessReadBudget - retained);
            std::string discarded;
            consume_output(discarded, kTrainProcessReadBudget);
        }
        if ((ready[3].revents & POLLIN) != 0) static_cast<void>(consume_stop_request());
        if ((ready[4].revents & POLLIN) != 0) static_cast<void>(consume_escalation());
        if ((ready[5].revents & POLLIN) != 0) {
            mmltk::common::io::drain_event_fd(stop_fd.get());
            static_cast<void>(request_stop(false));
        }
    }
}

void TrainProcessClient::force_reap() noexcept {
    if (!active()) return;
    signal_group(state_->group, SIGKILL);
    if (!state_->reaped && state_->pid > 0) static_cast<void>(mmltk::frameworks::process::wait_child_process(state_->pid));
    try {
        state_->refresh_group_members();
        while (!state_->group_quiesced) {
            pollfd ready{.fd = state_->lifecycle_fd.get(), .events = POLLIN, .revents = 0};
            int result;
            do {
                result = ::poll(&ready, 1U, -1);
            } while (result < 0 && errno == EINTR);
            if (result <= 0) std::terminate();
            static_cast<void>(state_->consume_lifecycle());
        }
    } catch (...) { std::terminate(); }
    state_->terminal_consumed = true;
    state_->progress_fd.reset();
    state_.reset();
}

}  // namespace mmltk::controller::services
