#include "train_process_runtime.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fastloader::gui {

namespace {

using json = nlohmann::json;

constexpr std::chrono::milliseconds kTrainPollInterval{20};
constexpr std::chrono::milliseconds kProgressPollInterval{100};

std::string format_decimal(double value, int precision) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(precision);
    stream << value;
    return stream.str();
}

std::optional<TrainArtifactProgress> read_train_artifact_progress(const std::filesystem::path& output_dir) {
    auto read_json_object = [](const std::filesystem::path& path) -> std::optional<json> {
        std::ifstream stream(path);
        if (!stream.is_open()) {
            return std::nullopt;
        }
        json parsed = json::parse(stream, nullptr, false);
        if (!parsed.is_object()) {
            return std::nullopt;
        }
        return parsed;
    };
    auto read_last_json_line = [](const std::filesystem::path& path) -> std::optional<json> {
        std::ifstream stream(path);
        if (!stream.is_open()) {
            return std::nullopt;
        }
        std::optional<json> last_entry;
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty()) {
                continue;
            }
            json parsed = json::parse(line, nullptr, false);
            if (parsed.is_object()) {
                last_entry = std::move(parsed);
            }
        }
        return last_entry;
    };
    auto assign_json_value = [](const json& object, const char* key, auto& target) {
        const auto found = object.find(key);
        if (found == object.end() || found->is_null()) {
            return;
        }
        try {
            target = found->get<std::decay_t<decltype(target)>>();
        } catch (const std::exception&) {
        }
    };
    auto assign_val_metrics = [&](const json& object, TrainArtifactProgress& progress) {
        const auto val = object.find("val");
        if (val == object.end() || !val->is_object()) {
            return;
        }
        assign_json_value(*val, "loss", progress.val_loss);
        if (const auto bbox = val->find("bbox"); bbox != val->end() && bbox->is_object()) {
            assign_json_value(*bbox, "ap", progress.val_bbox_ap);
        }
        if (const auto mask = val->find("mask"); mask != val->end() && mask->is_object()) {
            assign_json_value(*mask, "ap", progress.val_mask_ap);
        }
    };
    auto merge_progress = [&](const json& object, TrainArtifactProgress& progress) {
        assign_json_value(object, "phase", progress.phase);
        assign_json_value(object, "epoch", progress.epoch);
        assign_json_value(object, "total_epochs", progress.total_epochs);
        assign_json_value(object, "completed_batches", progress.completed_batches);
        assign_json_value(object, "total_batches", progress.total_batches);
        assign_json_value(object, "completed_waves", progress.completed_waves);
        assign_json_value(object, "optimizer_steps", progress.optimizer_steps);
        assign_json_value(object, "steps_per_epoch", progress.steps_per_epoch);
        assign_json_value(object, "train_lanes", progress.train_lanes);
        assign_json_value(object, "train_loss", progress.train_loss);
        assign_json_value(object, "class_loss", progress.class_loss);
        assign_json_value(object, "box_loss", progress.box_loss);
        assign_json_value(object, "step_loss", progress.step_loss);
        assign_json_value(object, "step_class_loss", progress.step_class_loss);
        assign_json_value(object, "step_box_loss", progress.step_box_loss);
        assign_json_value(object, "batches_per_second", progress.batches_per_second);
        assign_json_value(object, "images_per_second", progress.images_per_second);
        assign_json_value(object, "elapsed_seconds", progress.elapsed_seconds);
        assign_json_value(object, "val_loss", progress.val_loss);
        assign_json_value(object, "checkpoint_path", progress.checkpoint_path);
        assign_val_metrics(object, progress);
    };

    std::optional<TrainArtifactProgress> progress;
    if (const auto progress_json = read_json_object(output_dir / "progress.json"); progress_json.has_value()) {
        progress.emplace();
        merge_progress(*progress_json, *progress);
    }
    if (const auto log_json = read_last_json_line(output_dir / "log.txt"); log_json.has_value()) {
        if (!progress.has_value()) {
            progress.emplace();
        }
        merge_progress(*log_json, *progress);
    }
    if (!progress.has_value()) {
        return std::nullopt;
    }
    if (const auto results_json = read_json_object(output_dir / "results.json"); results_json.has_value()) {
        if (progress->checkpoint_path.empty()) {
            progress->checkpoint_path =
                results_json->value("best_checkpoint", results_json->value("checkpoint", ""));
        }
    }
    return progress;
}

void trim_output_tail(std::string& output_tail, std::size_t max_size = 65536) {
    if (output_tail.size() > max_size) {
        output_tail.erase(0, output_tail.size() - max_size);
    }
}

void append_console_output(std::string& tail, const std::string_view chunk, std::size_t max_size = 65536) {
    size_t index = 0;
    while (index < chunk.size()) {
        const char ch = chunk[index];
        if (ch == '\r') {
            const size_t newline = tail.rfind('\n');
            if (newline == std::string::npos) {
                tail.clear();
            } else {
                tail.erase(newline + 1);
            }
            ++index;
            continue;
        }
        if (ch == '\b') {
            if (!tail.empty()) {
                tail.pop_back();
            }
            ++index;
            continue;
        }
        if (ch == '\033') {
            ++index;
            if (index < chunk.size() && chunk[index] == '[') {
                ++index;
                while (index < chunk.size()) {
                    const unsigned char code = static_cast<unsigned char>(chunk[index++]);
                    if (code >= 0x40 && code <= 0x7E) {
                        break;
                    }
                }
            }
            continue;
        }
        tail.push_back(ch);
        ++index;
    }
    trim_output_tail(tail, max_size);
}

std::string drain_nonblocking_fd(const int fd) {
    std::string drained;
    if (fd < 0) {
        return drained;
    }
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t bytes_read = ::read(fd, buffer.data(), buffer.size());
        if (bytes_read > 0) {
            drained.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            continue;
        }
        if (bytes_read == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        throw std::runtime_error(std::string("failed to read train subprocess output: ") + std::strerror(errno));
    }
    return drained;
}

std::string summarize_train_process(const LocalTrainSessionState& state) {
    if (state.progress.has_value()) {
        std::ostringstream stream;
        stream << "train completed: last_epoch=" << (state.progress->epoch + 1)
               << " train_loss=" << format_decimal(state.progress->train_loss, 4)
               << " val_bbox_ap=" << format_decimal(state.progress->val_bbox_ap, 4)
               << " val_mask_ap=" << format_decimal(state.progress->val_mask_ap, 4);
        if (!state.progress->checkpoint_path.empty()) {
            stream << " checkpoint=" << state.progress->checkpoint_path;
        }
        return stream.str();
    }
    return "train completed";
}

bool process_group_alive(int process_group_id) {
    if (process_group_id <= 0) {
        return false;
    }
    if (::kill(-process_group_id, 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

} // namespace

struct LocalTrainSession::Impl {
    mutable std::mutex mutex;
    std::thread worker;
    LocalTrainSessionState state;

    void start(const TrainCommandConfig& config,
               const std::filesystem::path& cli_path) {
        shutdown();
        {
            std::lock_guard<std::mutex> lock(mutex);
            state = {};
            state.running = true;
            state.label = "train.local";
            state.last_summary = "starting local training...";
            state.output_dir = config.output_dir;
        }
        worker = std::thread(&Impl::worker_main, this, config, cli_path);
    }

    void request_stop(bool force) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!state.running) {
            return;
        }
        state.stop_requested = true;
        state.force_kill_requested = state.force_kill_requested || force;
        state.last_error.clear();
        state.last_summary = force ? "force kill requested for local training"
                                   : "stop requested for local training";
    }

    void shutdown() {
        if (worker.joinable()) {
            request_stop(true);
            worker.join();
        }
        std::lock_guard<std::mutex> lock(mutex);
        state.running = false;
        state.pid = -1;
        state.process_group_id = -1;
    }

    LocalTrainSessionState snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        return state;
    }

    bool running() const {
        std::lock_guard<std::mutex> lock(mutex);
        return state.running;
    }

    void worker_main(TrainCommandConfig config,
                     std::filesystem::path cli_path) {
        int stdout_fd = -1;
        try {
            const std::vector<std::string> args = build_train_command_arguments(config);
            const std::filesystem::path output_dir = config.output_dir;
            std::error_code ignored_error;
            std::filesystem::remove(output_dir / "progress.json", ignored_error);
            std::filesystem::remove(output_dir / "log.txt", ignored_error);
            std::filesystem::remove(output_dir / "results.json", ignored_error);

            int output_pipe[2];
            if (::pipe(output_pipe) != 0) {
                throw std::runtime_error(std::string("failed to create local train pipe: ") + std::strerror(errno));
            }

            std::vector<std::string> argv_strings;
            argv_strings.reserve(args.size() + 1U);
            argv_strings.push_back(cli_path.string());
            argv_strings.insert(argv_strings.end(), args.begin(), args.end());

            std::vector<char*> argv;
            argv.reserve(argv_strings.size() + 1U);
            for (auto& arg : argv_strings) {
                argv.push_back(arg.data());
            }
            argv.push_back(nullptr);

            const pid_t child_pid = ::fork();
            if (child_pid < 0) {
                ::close(output_pipe[0]);
                ::close(output_pipe[1]);
                throw std::runtime_error(std::string("failed to fork local train subprocess: ") + std::strerror(errno));
            }
            if (child_pid == 0) {
                (void)::setpgid(0, 0);
                ::close(output_pipe[0]);
                if (::dup2(output_pipe[1], STDOUT_FILENO) < 0 || ::dup2(output_pipe[1], STDERR_FILENO) < 0) {
                    std::fprintf(stderr, "dup2 failed for local train subprocess: %s\n", std::strerror(errno));
                    std::_Exit(127);
                }
                ::close(output_pipe[1]);
                ::execv(argv.front(), argv.data());
                std::fprintf(stderr, "execv failed for local train subprocess: %s\n", std::strerror(errno));
                std::_Exit(127);
            }

            ::close(output_pipe[1]);
            if (::setpgid(child_pid, child_pid) != 0 && errno != EACCES && errno != ESRCH) {
                ::close(output_pipe[0]);
                (void)::kill(child_pid, SIGKILL);
                throw std::runtime_error(std::string("failed to group local train subprocess: ") + std::strerror(errno));
            }
            const int flags = ::fcntl(output_pipe[0], F_GETFL, 0);
            if (flags >= 0) {
                (void)::fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
            }
            stdout_fd = output_pipe[0];

            {
                std::lock_guard<std::mutex> lock(mutex);
                state.pid = static_cast<int>(child_pid);
                state.process_group_id = static_cast<int>(child_pid);
                state.exit_code = -1;
                state.output_tail.clear();
                state.progress.reset();
            }

            bool term_sent = false;
            bool kill_sent = false;
            auto next_progress_read = std::chrono::steady_clock::now();
            while (true) {
                bool stop_requested = false;
                bool force_kill_requested = false;
                int pid = -1;
                int process_group_id = -1;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    stop_requested = state.stop_requested;
                    force_kill_requested = state.force_kill_requested;
                    pid = state.pid;
                    process_group_id = state.process_group_id;
                }

                if (process_group_id > 0) {
                    if (force_kill_requested && !kill_sent) {
                        if (::kill(-process_group_id, SIGKILL) != 0 && errno != ESRCH) {
                            std::lock_guard<std::mutex> lock(mutex);
                            state.last_error =
                                std::string("failed to signal local train subprocess group: ") + std::strerror(errno);
                        }
                        kill_sent = true;
                    } else if (stop_requested && !term_sent) {
                        if (::kill(-process_group_id, SIGTERM) != 0 && errno != ESRCH) {
                            std::lock_guard<std::mutex> lock(mutex);
                            state.last_error =
                                std::string("failed to signal local train subprocess group: ") + std::strerror(errno);
                        }
                        term_sent = true;
                    }
                }

                const std::string output_chunk = drain_nonblocking_fd(stdout_fd);
                if (!output_chunk.empty()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    append_console_output(state.output_tail, output_chunk);
                }

                const auto now = std::chrono::steady_clock::now();
                if (now >= next_progress_read) {
                    if (const auto progress = read_train_artifact_progress(output_dir); progress.has_value()) {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.progress = progress;
                    }
                    next_progress_read = now + kProgressPollInterval;
                }

                int status = 0;
                const pid_t waited = pid > 0 ? ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG) : 0;
                if (waited == 0) {
                    std::this_thread::sleep_for(kTrainPollInterval);
                    continue;
                }
                if (waited < 0) {
                    if ((errno == ECHILD || errno == ESRCH) && process_group_alive(process_group_id)) {
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            state.pid = -1;
                        }
                        std::this_thread::sleep_for(kTrainPollInterval);
                        continue;
                    }
                    throw std::runtime_error(std::string("failed to wait for local train subprocess: ") + std::strerror(errno));
                }

                finalize_process(status, stdout_fd, process_group_id, output_dir);
                stdout_fd = -1;
                return;
            }
        } catch (const std::exception& error) {
            if (stdout_fd >= 0) {
                ::close(stdout_fd);
            }
            std::lock_guard<std::mutex> lock(mutex);
            state.running = false;
            state.pid = -1;
            state.process_group_id = -1;
            if (state.last_error.empty()) {
                state.last_error = error.what();
            } else {
                state.last_error += "\n";
                state.last_error += error.what();
            }
        }
    }

    void finalize_process(int status,
                          int stdout_fd,
                          int process_group_id,
                          const std::filesystem::path& output_dir) {
        const std::string output_chunk = drain_nonblocking_fd(stdout_fd);
        ::close(stdout_fd);
        const auto progress = read_train_artifact_progress(output_dir);
        std::lock_guard<std::mutex> lock(mutex);
        if (WIFEXITED(status)) {
            state.exit_code = WEXITSTATUS(status);
            state.last_error =
                state.exit_code == 0 ? std::string{} : "local train exited with code " + std::to_string(state.exit_code);
        } else if (WIFSIGNALED(status)) {
            state.exit_code = 128 + WTERMSIG(status);
            state.last_error = "local train terminated with signal " + std::to_string(WTERMSIG(status));
        } else {
            state.exit_code = 1;
            state.last_error = "local train terminated unexpectedly";
        }

        if (!output_chunk.empty()) {
            append_console_output(state.output_tail, output_chunk);
        }
        if (progress.has_value()) {
            state.progress = progress;
        }
        state.pid = -1;
        state.process_group_id = -1;
        state.running = false;

        if (state.stop_requested) {
            state.last_error.clear();
            state.last_summary = state.force_kill_requested ? "local train killed" : "local train stopped";
            return;
        }
        if ((state.exit_code == 0 ||
             (state.progress.has_value() && state.progress->phase == "complete")) &&
            state.last_error.empty()) {
            state.last_summary = summarize_train_process(state);
            return;
        }
        if (process_group_alive(process_group_id)) {
            if (!state.last_error.empty()) {
                state.last_error += "\n";
            }
            state.last_error += "local train worker group is still alive after the tracked subprocess exited";
            return;
        }
        if (!state.output_tail.empty()) {
            if (!state.last_error.empty()) {
                state.last_error += "\n";
            }
            state.last_error += state.output_tail;
        }
    }
};

LocalTrainSession::LocalTrainSession()
    : impl_(std::make_unique<Impl>()) {}

LocalTrainSession::~LocalTrainSession() {
    shutdown();
}

void LocalTrainSession::start(const TrainCommandConfig& config,
                              const std::filesystem::path& cli_path) {
    impl_->start(config, cli_path);
}

void LocalTrainSession::request_stop(bool force) {
    impl_->request_stop(force);
}

void LocalTrainSession::shutdown() {
    impl_->shutdown();
}

LocalTrainSessionState LocalTrainSession::snapshot() const {
    return impl_->snapshot();
}

bool LocalTrainSession::running() const {
    return impl_->running();
}

} // namespace fastloader::gui
