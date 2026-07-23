#include "browser_artifact_controller.h"

#include "compiled_file_utils.h"
#include "common_utils.h"
#include "cpu_affinity.h"
#include "dataset_compiler.h"
#include "mmltk/rfdetr/model_config.h"
#include "mmltk/rfdetr/preset_catalog.h"
#include "mmltk/rfdetr/weight_catalog.h"
#include "mmltk/runtime/async_runtime.h"
#include "mmltk_logging.h"
#include "subprocess_utils.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mmltk::gui {

namespace {

using OperationPhase = mmltk::browser::ArtifactOperationPhase;
using DatasetCompilePhase = mmltk::browser::DatasetCompilePhase;
using WeightStatus = mmltk::browser::WeightArtifactStatus;

class CompileEtaEstimator;

struct DatasetOperationState {
    OperationPhase phase = OperationPhase::Idle;
    DatasetCompilePhase compile_phase = DatasetCompilePhase::Idle;
    std::uint64_t generation = 0U;
    std::string source_dir;
    std::string output_dir;
    std::string preset_name;
    std::string active_split;
    std::size_t done = 0U;
    std::size_t total = 0U;
    std::uint64_t elapsed_ms = 0U;
    std::uint64_t remaining_ms = 0U;
    std::uint64_t dropped_instances = 0U;
    bool eta_ready = false;
    std::string error;
    std::vector<mmltk::CompiledDatasetInfo> splits;
    bool compatible = false;
    bool compiling = false;
    std::chrono::steady_clock::time_point started_at{};
    std::shared_ptr<mmltk::CompileTelemetry> compile_telemetry;
    std::unique_ptr<CompileEtaEstimator> eta;
    std::size_t completed_steps = 0U;
    std::uint64_t future_images = 0U;
    std::uint64_t completed_dropped_instances = 0U;
};

struct CompileEtaSnapshot {
    std::uint64_t elapsed_ms = 0U;
    std::uint64_t remaining_ms = 0U;
    bool ready = false;
};

class CompileEtaEstimator {
   public:
    using Clock = std::chrono::steady_clock;

    explicit CompileEtaEstimator(const Clock::time_point started_at)
        : started_at_(started_at), previous_sample_(started_at), last_work_(started_at) {}

    void begin_split(const Clock::time_point now = Clock::now()) noexcept {
        previous_sample_ = now;
        previous_label_done_ = 0U;
        previous_pixel_done_ = 0U;
    }

    [[nodiscard]] CompileEtaSnapshot sample(const mmltk::CompileProgress& progress, const std::uint64_t future_images,
                                            const Clock::time_point now = Clock::now()) noexcept {
        const double interval_seconds = std::chrono::duration<double>(now - previous_sample_).count();
        const std::size_t label_delta =
            progress.label_done >= previous_label_done_ ? progress.label_done - previous_label_done_ : 0U;
        const std::size_t pixel_delta =
            progress.pixel_done >= previous_pixel_done_ ? progress.pixel_done - previous_pixel_done_ : 0U;
        if (interval_seconds > 0.0) {
            update_rate(label_rate_, static_cast<double>(label_delta) / interval_seconds);
            update_rate(pixel_rate_, static_cast<double>(pixel_delta) / interval_seconds);
        }
        if (label_delta != 0U || pixel_delta != 0U) {
            if (!observation_started_) {
                observation_started_at_ = now;
                observation_started_ = true;
            }
            last_work_ = now;
        }
        previous_sample_ = now;
        previous_label_done_ = progress.label_done;
        previous_pixel_done_ = progress.pixel_done;

        CompileEtaSnapshot estimate;
        estimate.elapsed_ms = duration_ms(now - started_at_);
        const bool observation_ready = observation_started_ && now - observation_started_at_ >= kMinimumObservation;
        const bool stalled = now - last_work_ >= kStallWindow;
        const std::uint64_t label_remaining =
            progress.label_total - std::min(progress.label_done, progress.label_total);
        const std::uint64_t pixel_remaining =
            progress.pixel_total - std::min(progress.pixel_done, progress.pixel_total);
        if (!observation_ready || stalled || label_rate_ <= 0.0 || pixel_rate_ <= 0.0) {
            return estimate;
        }
        const double current_seconds = std::max(static_cast<double>(label_remaining) / label_rate_,
                                                static_cast<double>(pixel_remaining) / pixel_rate_);
        const double future_seconds =
            static_cast<double>(future_images) * std::max(1.0 / label_rate_, 1.0 / pixel_rate_);
        estimate.remaining_ms = seconds_ms(current_seconds + future_seconds);
        estimate.ready = true;
        return estimate;
    }

   private:
    static constexpr auto kMinimumObservation = std::chrono::seconds(2);
    static constexpr auto kStallWindow = std::chrono::seconds(2);
    static constexpr double kEwmaWeight = 0.25;
    static constexpr double kRateChangeBound = 4.0;

    static void update_rate(double& rate, const double observed_rate) noexcept {
        if (observed_rate <= 0.0) {
            return;
        }
        if (rate <= 0.0) {
            rate = observed_rate;
            return;
        }
        const double bounded = std::clamp(observed_rate, rate / kRateChangeBound, rate * kRateChangeBound);
        rate += kEwmaWeight * (bounded - rate);
    }

    template <typename Rep, typename Period>
    [[nodiscard]] static std::uint64_t duration_ms(const std::chrono::duration<Rep, Period> duration) noexcept {
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        return milliseconds > 0 ? static_cast<std::uint64_t>(milliseconds) : 0U;
    }

    [[nodiscard]] static std::uint64_t seconds_ms(const double seconds) noexcept {
        constexpr double kMaximumMilliseconds = static_cast<double>(std::numeric_limits<std::uint64_t>::max());
        return static_cast<std::uint64_t>(std::clamp(seconds * 1'000.0, 0.0, kMaximumMilliseconds));
    }

    Clock::time_point started_at_;
    Clock::time_point previous_sample_;
    Clock::time_point last_work_;
    Clock::time_point observation_started_at_{};
    std::size_t previous_label_done_ = 0U;
    std::size_t previous_pixel_done_ = 0U;
    double label_rate_ = 0.0;
    double pixel_rate_ = 0.0;
    bool observation_started_ = false;
};

[[nodiscard]] DatasetCompilePhase browser_compile_phase(const mmltk::CompileProgressPhase phase) noexcept {
    switch (phase) {
        case mmltk::CompileProgressPhase::kPlanning:
            return DatasetCompilePhase::Planning;
        case mmltk::CompileProgressPhase::kLabels:
            return DatasetCompilePhase::Labels;
        case mmltk::CompileProgressPhase::kPixels:
            return DatasetCompilePhase::Pixels;
        case mmltk::CompileProgressPhase::kSyncing:
            return DatasetCompilePhase::Syncing;
        case mmltk::CompileProgressPhase::kPublishing:
            return DatasetCompilePhase::Publishing;
    }
    return DatasetCompilePhase::Idle;
}

struct WeightOperationState {
    OperationPhase phase = OperationPhase::Idle;
    WeightStatus status = WeightStatus::Idle;
    std::uint64_t generation = 0U;
    std::string preset_name;
    std::string path;
    std::string error;
    std::uint64_t downloaded_bytes = 0U;
    std::uint64_t total_bytes = 0U;
    std::uint32_t attempt = 0U;
    std::chrono::steady_clock::time_point retry_deadline{};
    bool resumable = false;
};

struct DatasetInspectionResult {
    std::vector<mmltk::CompiledDatasetInfo> splits;
    bool compatible = false;
    std::string error;
};

struct PendingDatasetInspection {
    std::array<std::string, 3U> paths;
    std::string preset_name;
    std::uint32_t resolution = 0U;
};

class StagingDirectoryCleanup {
   public:
    explicit StagingDirectoryCleanup(std::filesystem::path path) : path_(std::move(path)) {}
    ~StagingDirectoryCleanup() {
        if (!published_) {
            std::error_code ignored;
            (void)mmltk::remove_tree_no_follow(path_, ignored);
        }
    }

    void published() noexcept {
        published_ = true;
    }

   private:
    std::filesystem::path path_;
    bool published_ = false;
};

[[nodiscard]] std::filesystem::path create_staging_directory(const std::filesystem::path& output) {
    const std::filesystem::path parent =
        output.parent_path().empty() ? std::filesystem::path{"."} : output.parent_path();
    std::filesystem::create_directories(parent);
    std::string path_template = output.string() + ".tmp.XXXXXX";
    std::vector<char> writable_path(path_template.begin(), path_template.end());
    writable_path.push_back('\0');
    if (::mkdtemp(writable_path.data()) == nullptr) {
        throw mmltk::errno_error("mkdtemp failed", path_template);
    }
    return std::filesystem::path(writable_path.data());
}

[[nodiscard]] bool path_is_within(const std::filesystem::path& candidate, const std::filesystem::path& parent) {
    auto candidate_it = candidate.begin();
    for (auto parent_it = parent.begin(); parent_it != parent.end(); ++parent_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *candidate_it != *parent_it) {
            return false;
        }
    }
    return true;
}

void validate_nonoverlapping_directories(const std::filesystem::path& source, const std::filesystem::path& output) {
    const std::filesystem::path normalized_source = std::filesystem::weakly_canonical(source);
    const std::filesystem::path normalized_output = std::filesystem::weakly_canonical(output);
    if (path_is_within(normalized_source, normalized_output) || path_is_within(normalized_output, normalized_source)) {
        throw std::runtime_error("raw dataset source and compiled output directories must not overlap: " +
                                 normalized_source.string() + " and " + normalized_output.string());
    }
}

[[nodiscard]] DatasetInspectionResult inspect_dataset_paths(const std::array<std::string, 3U>& paths,
                                                            const std::string_view preset_name,
                                                            const std::uint32_t resolution) {
    const mmltk::rfdetr::PresetCatalogEntry* preset = mmltk::rfdetr::find_preset_catalog_entry(preset_name);
    if (preset == nullptr) {
        throw std::runtime_error("unknown RF-DETR preset: " + std::string(preset_name));
    }
    DatasetInspectionResult result;
    if (paths[0].empty() || paths[1].empty()) {
        result.error = "training requires both train and validation compiled dataset paths";
        return result;
    }
    constexpr std::array<std::string_view, 3U> kSplitNames{"train", "val", "test"};
    for (std::size_t index = 0U; index < paths.size(); ++index) {
        const std::string& path = paths[index];
        if (!path.empty()) {
            try {
                result.splits.push_back(mmltk::inspect_compiled_dataset(path));
            } catch (const std::exception& error) {
                result.error = std::string(kSplitNames[index]) + " split " + path + " is invalid: " + error.what();
                return result;
            }
        }
    }
    const std::vector<std::string>& classes = result.splits.front().class_names;
    std::size_t inspected_index = 0U;
    for (std::size_t path_index = 0U; path_index < paths.size(); ++path_index) {
        if (paths[path_index].empty()) {
            continue;
        }
        const mmltk::CompiledDatasetInfo& split = result.splits[inspected_index++];
        const bool invalid_shape = split.width != split.height || split.channels != 3U;
        const bool mismatched_resolution = resolution > 0U && split.width != resolution;
        if (invalid_shape || mismatched_resolution) {
            result.error = std::string(kSplitNames[path_index]) + " split " + split.path.string() + " is " +
                           std::to_string(split.width) + "x" + std::to_string(split.height) + "x" +
                           std::to_string(split.channels) + "; " + std::string(preset->display_name) +
                           (resolution > 0U ? " is configured for " + std::to_string(resolution) + "x" +
                                                  std::to_string(resolution) + "x3"
                                            : " requires square RGB input");
            return result;
        }
        if (split.class_names != classes) {
            result.error = std::string(kSplitNames[path_index]) + " split " + split.path.string() +
                           " has a different class catalog than train split " + result.splits.front().path.string();
            return result;
        }
    }
    result.compatible = true;
    return result;
}

enum class ChildStage : std::uint8_t {
    Redirect,
    PreserveDescriptor,
    Exec,
};

[[nodiscard]] const char* child_stage_name(const ChildStage stage) noexcept {
    switch (stage) {
        case ChildStage::Redirect:
            return "output redirect";
        case ChildStage::PreserveDescriptor:
            return "descriptor preservation";
        case ChildStage::Exec:
            return "exec";
    }
    return "unknown setup";
}

[[nodiscard]] std::string run_program(const std::vector<std::string>& arguments,
                                      const std::atomic<bool>* cancel_requested = nullptr,
                                      const std::chrono::milliseconds timeout = std::chrono::milliseconds{0},
                                      const int preserved_fd = -1) {
    if (arguments.empty()) {
        throw std::invalid_argument("cannot run an empty process argument list");
    }
    const auto captured = subprocess::run_captured_child_process<ChildStage>(
        arguments.front(), "artifact process output read failed: ",
        [&](const int output_fd, const int setup_fd) {
            if (::dup2(output_fd, STDOUT_FILENO) < 0 || ::dup2(output_fd, STDERR_FILENO) < 0) {
                (void)subprocess::write_child_setup_failure(setup_fd, ChildStage::Redirect);
                std::_Exit(127);
            }
            if (preserved_fd >= 0) {
                const int descriptor_flags = ::fcntl(preserved_fd, F_GETFD);
                if (descriptor_flags < 0 || ::fcntl(preserved_fd, F_SETFD, descriptor_flags & ~FD_CLOEXEC) != 0) {
                    (void)subprocess::write_child_setup_failure(setup_fd, ChildStage::PreserveDescriptor);
                    std::_Exit(127);
                }
            }
            std::vector<char*> argv;
            argv.reserve(arguments.size() + 1U);
            for (const std::string& argument : arguments) {
                argv.push_back(const_cast<char*>(argument.c_str()));
            }
            argv.push_back(nullptr);
            ::execvp(argv.front(), argv.data());
            (void)subprocess::write_child_setup_failure(setup_fd, ChildStage::Exec);
            std::_Exit(127);
        },
        std::chrono::milliseconds{20},
        cancel_requested != nullptr
            ? std::function<bool()>{[cancel_requested] { return cancel_requested->load(std::memory_order_relaxed); }}
            : std::function<bool()>{},
        timeout);
    if (captured.setup_failure.has_value()) {
        throw std::runtime_error(
            subprocess::format_child_setup_failure(*captured.setup_failure, child_stage_name, arguments.front()));
    }
    if (!WIFEXITED(captured.status) || WEXITSTATUS(captured.status) != 0) {
        std::string output_tail = captured.output;
        subprocess::trim_output_tail(output_tail, 4096U);
        throw std::runtime_error(arguments.front() + " failed: " + output_tail);
    }
    return captured.output;
}

[[nodiscard]] std::string file_md5(const std::filesystem::path& path,
                                   const std::atomic<bool>* cancel_requested = nullptr) {
    const std::string output = run_program({"md5sum", "--", path.string()}, cancel_requested);
    if (output.size() < 32U) {
        throw std::runtime_error("md5sum returned an invalid digest for " + path.string());
    }
    std::string digest = output.substr(0U, 32U);
    std::ranges::transform(digest, digest.begin(),
                           [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return digest;
}

[[nodiscard]] std::string leased_file_md5(const int fd, const std::filesystem::path& path,
                                          const std::atomic<bool>* cancel_requested = nullptr) {
    const std::filesystem::path descriptor_path = "/proc/self/fd/" + std::to_string(fd);
    const std::string output =
        run_program({"md5sum", "--", descriptor_path.string()}, cancel_requested, std::chrono::milliseconds{0}, fd);
    if (output.size() < 32U) {
        throw std::runtime_error("md5sum returned an invalid digest for " + path.string());
    }
    std::string digest = output.substr(0U, 32U);
    std::ranges::transform(digest, digest.begin(),
                           [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return digest;
}

[[nodiscard]] std::filesystem::path repository_cache_root() {
    if (const char* root = std::getenv("MMLTK_REPO_ROOT"); root != nullptr && *root != '\0') {
        return std::filesystem::path(root) / ".cache" / "mmltk" / "weights";
    }
    return std::filesystem::current_path() / ".cache" / "mmltk" / "weights";
}

struct PartialDownloadMetadata {
    static constexpr std::uint32_t kSchemaVersion = 1U;

    std::string url;
    std::string expected_md5;
    std::string etag;
    std::string last_modified;
    std::uint64_t total_bytes = 0U;
    bool accept_ranges = false;
};

enum class WeightFailureKind : std::uint8_t {
    NoConnection,
    CannotDownload,
    Checksum,
    Filesystem,
    Http,
    Incompatible,
    Cancelled,
};

class WeightTransferError final : public std::runtime_error {
   public:
    WeightTransferError(const WeightFailureKind kind, const std::string& message, const bool discard_partial = false)
        : std::runtime_error(message), kind_(kind), discard_partial_(discard_partial) {}

    [[nodiscard]] WeightFailureKind kind() const noexcept {
        return kind_;
    }
    [[nodiscard]] bool discard_partial() const noexcept {
        return discard_partial_;
    }

   private:
    WeightFailureKind kind_;
    bool discard_partial_;
};

[[nodiscard]] bool weight_failure_is_retryable(const WeightFailureKind kind) noexcept {
    switch (kind) {
        case WeightFailureKind::NoConnection:
        case WeightFailureKind::CannotDownload:
        case WeightFailureKind::Checksum:
        case WeightFailureKind::Http:
            return true;
        case WeightFailureKind::Filesystem:
        case WeightFailureKind::Incompatible:
        case WeightFailureKind::Cancelled:
            return false;
    }
    return false;
}

[[nodiscard]] WeightStatus weight_status_for_failure(const WeightFailureKind kind) noexcept {
    switch (kind) {
        case WeightFailureKind::NoConnection:
            return WeightStatus::NoConnection;
        case WeightFailureKind::CannotDownload:
            return WeightStatus::CannotDownload;
        case WeightFailureKind::Checksum:
            return WeightStatus::ChecksumError;
        case WeightFailureKind::Filesystem:
            return WeightStatus::FilesystemError;
        case WeightFailureKind::Http:
            return WeightStatus::HttpError;
        case WeightFailureKind::Incompatible:
            return WeightStatus::Incompatible;
        case WeightFailureKind::Cancelled:
            return WeightStatus::Cancelled;
    }
    return WeightStatus::CannotDownload;
}

[[nodiscard]] std::string_view weight_status_name(const WeightStatus status) noexcept {
    switch (status) {
        case WeightStatus::Idle:
            return "idle";
        case WeightStatus::Verifying:
            return "verifying";
        case WeightStatus::Downloading:
            return "downloading";
        case WeightStatus::RetryWaiting:
            return "retry_waiting";
        case WeightStatus::Ready:
            return "ready";
        case WeightStatus::NoConnection:
            return "no_connection";
        case WeightStatus::CannotDownload:
            return "cannot_download";
        case WeightStatus::ChecksumError:
            return "checksum_error";
        case WeightStatus::FilesystemError:
            return "filesystem_error";
        case WeightStatus::HttpError:
            return "http_error";
        case WeightStatus::Incompatible:
            return "incompatible";
        case WeightStatus::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

[[nodiscard]] std::uint64_t regular_file_size(const int fd, const std::filesystem::path& path) {
    struct stat status {};
    if (::fstat(fd, &status) != 0) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  mmltk::errno_error("cannot inspect partial weight descriptor", path.string()).what());
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  "partial weight path is not a regular file: " + path.string());
    }
    return static_cast<std::uint64_t>(status.st_size);
}

void require_leased_path_identity(const int fd, const std::filesystem::path& path) {
    struct stat descriptor_status {};
    struct stat path_status {};
    if (::fstat(fd, &descriptor_status) != 0) {
        throw WeightTransferError(
            WeightFailureKind::Filesystem,
            mmltk::errno_error("cannot inspect leased partial weight descriptor", path.string()).what());
    }
    if (::lstat(path.c_str(), &path_status) != 0) {
        throw WeightTransferError(
            WeightFailureKind::Filesystem,
            mmltk::errno_error("cannot inspect leased partial weight path", path.string()).what());
    }
    if (!S_ISREG(descriptor_status.st_mode) || !S_ISREG(path_status.st_mode) ||
        descriptor_status.st_dev != path_status.st_dev || descriptor_status.st_ino != path_status.st_ino) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  "leased partial weight path changed during acquisition: " + path.string());
    }
}

void reset_partial_descriptor(const int fd, const std::filesystem::path& path) {
    if (::ftruncate(fd, 0) != 0 || ::lseek(fd, 0, SEEK_SET) < 0) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  mmltk::errno_error("cannot reset partial weight download", path.string()).what());
    }
}

class PartialFileLease final {
   public:
    PartialFileLease(std::filesystem::path path, const std::atomic<bool>& cancelled) : path_(std::move(path)) {
        for (;;) {
            const int raw_fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
            if (raw_fd < 0) {
                throw WeightTransferError(
                    WeightFailureKind::Filesystem,
                    mmltk::errno_error("cannot open partial weight download", path_.string()).what());
            }
            mmltk::UniqueFd candidate(raw_fd);
            while (::flock(candidate.get(), LOCK_EX | LOCK_NB) != 0) {
                if (errno != EWOULDBLOCK && errno != EAGAIN) {
                    throw WeightTransferError(
                        WeightFailureKind::Filesystem,
                        mmltk::errno_error("cannot lease partial weight download", path_.string()).what());
                }
                if (cancelled.load(std::memory_order_relaxed)) {
                    throw WeightTransferError(WeightFailureKind::Cancelled, "weight acquisition cancelled");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }

            struct stat descriptor_status {};
            struct stat path_status {};
            if (::fstat(candidate.get(), &descriptor_status) != 0) {
                throw WeightTransferError(
                    WeightFailureKind::Filesystem,
                    mmltk::errno_error("cannot inspect leased partial weight descriptor", path_.string()).what());
            }
            if (::lstat(path_.c_str(), &path_status) != 0) {
                if (errno == ENOENT) {
                    (void)::flock(candidate.get(), LOCK_UN);
                    continue;
                }
                throw WeightTransferError(
                    WeightFailureKind::Filesystem,
                    mmltk::errno_error("cannot inspect leased partial weight path", path_.string()).what());
            }
            if (descriptor_status.st_dev != path_status.st_dev || descriptor_status.st_ino != path_status.st_ino) {
                (void)::flock(candidate.get(), LOCK_UN);
                continue;
            }
            if (!S_ISREG(descriptor_status.st_mode)) {
                throw WeightTransferError(WeightFailureKind::Filesystem,
                                          "partial weight path is not a regular file: " + path_.string());
            }
            fd_ = std::move(candidate);
            return;
        }
    }

    ~PartialFileLease() {
        if (fd_.get() >= 0) {
            (void)::flock(fd_.get(), LOCK_UN);
        }
    }

    PartialFileLease(const PartialFileLease&) = delete;
    PartialFileLease& operator=(const PartialFileLease&) = delete;

    [[nodiscard]] int fd() const noexcept {
        return fd_.get();
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

   private:
    std::filesystem::path path_;
    mmltk::UniqueFd fd_;
};

[[nodiscard]] bool remove_legacy_lockfile(const std::filesystem::path& path, const std::atomic<bool>& cancelled) {
    const int raw_fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (raw_fd < 0) {
        if (errno == ENOENT) {
            return false;
        }
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  mmltk::errno_error("cannot open legacy weight lock", path.string()).what());
    }
    const mmltk::UniqueFd fd(raw_fd);
    while (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            throw WeightTransferError(WeightFailureKind::Filesystem,
                                      mmltk::errno_error("cannot acquire legacy weight lock", path.string()).what());
        }
        if (cancelled.load(std::memory_order_relaxed)) {
            throw WeightTransferError(WeightFailureKind::Cancelled, "weight acquisition cancelled");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  mmltk::errno_error("cannot remove legacy weight lock", path.string()).what());
    }
    try {
        mmltk::sync_parent_directory(path);
    } catch (const std::exception& error) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  "cannot sync legacy weight lock cleanup: " + std::string(error.what()));
    }
    return true;
}

[[nodiscard]] bool remove_path_checked(const std::filesystem::path& path, const std::string_view action) {
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    if (error) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  std::string(action) + ": " + path.string() + ": " + error.message());
    }
    return removed;
}

void sync_cleanup_directory(const std::filesystem::path& path, const std::string_view action) {
    try {
        mmltk::sync_parent_directory(path);
    } catch (const std::exception& error) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  std::string(action) + ": " + std::string(error.what()));
    }
}

void remove_resume_metadata(const std::filesystem::path& metadata_path) {
    const bool removed_staging =
        remove_path_checked(metadata_path.string() + ".next", "cannot remove staged weight resume metadata");
    const bool removed_metadata = remove_path_checked(metadata_path, "cannot remove weight resume metadata");
    if (removed_staging || removed_metadata) {
        sync_cleanup_directory(metadata_path, "cannot sync weight resume metadata cleanup");
    }
}

void discard_partial_state(const int fd, const std::filesystem::path& part_path,
                           const std::filesystem::path& metadata_path) {
    reset_partial_descriptor(fd, part_path);
    remove_resume_metadata(metadata_path);
}

void remove_transient_state(const std::filesystem::path& part_path, const std::filesystem::path& metadata_path) {
    const bool removed_partial = remove_path_checked(part_path, "cannot remove completed partial weight file");
    const bool removed_staging =
        remove_path_checked(metadata_path.string() + ".next", "cannot remove staged weight resume metadata");
    const bool removed_metadata = remove_path_checked(metadata_path, "cannot remove weight resume metadata");
    if (removed_partial || removed_staging || removed_metadata) {
        sync_cleanup_directory(part_path, "cannot sync completed weight cleanup");
    }
}

[[nodiscard]] std::optional<PartialDownloadMetadata> load_partial_metadata(const std::filesystem::path& metadata_path,
                                                                           const std::string_view url,
                                                                           const std::string_view expected_md5) {
    try {
        std::ifstream input(metadata_path);
        if (!input) {
            return std::nullopt;
        }
        const nlohmann::json json = nlohmann::json::parse(input);
        PartialDownloadMetadata metadata;
        const std::uint32_t schema_version = json.value("schema_version", std::uint32_t{0U});
        metadata.url = json.value("url", std::string{});
        metadata.expected_md5 = json.value("expected_md5", std::string{});
        metadata.etag = json.value("etag", std::string{});
        metadata.last_modified = json.value("last_modified", std::string{});
        metadata.total_bytes = json.value("total_bytes", std::uint64_t{0U});
        metadata.accept_ranges = json.value("accept_ranges", false);
        if (schema_version != PartialDownloadMetadata::kSchemaVersion || metadata.url != url ||
            metadata.expected_md5 != expected_md5 || metadata.total_bytes == 0U || !metadata.accept_ranges ||
            (metadata.etag.empty() && metadata.last_modified.empty())) {
            return std::nullopt;
        }
        return metadata;
    } catch (...) {
        return std::nullopt;
    }
}

void store_partial_metadata(const std::filesystem::path& metadata_path, const PartialDownloadMetadata& metadata) {
    const std::filesystem::path staging_path = metadata_path.string() + ".next";
    const std::string encoded = nlohmann::json{
        {"schema_version", PartialDownloadMetadata::kSchemaVersion},
        {"url", metadata.url},
        {"expected_md5", metadata.expected_md5},
        {"etag", metadata.etag},
        {"last_modified", metadata.last_modified},
        {"total_bytes", metadata.total_bytes},
        {"accept_ranges",
         metadata.accept_ranges}}.dump();
    const int raw_fd = ::open(staging_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (raw_fd < 0) {
        throw WeightTransferError(
            WeightFailureKind::Filesystem,
            mmltk::errno_error("cannot write resumable download metadata", staging_path.string()).what());
    }
    const mmltk::UniqueFd fd(raw_fd);
    std::size_t written = 0U;
    while (written < encoded.size()) {
        const ssize_t result = ::write(fd.get(), encoded.data() + written, encoded.size() - written);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            throw WeightTransferError(
                WeightFailureKind::Filesystem,
                mmltk::errno_error("cannot write resumable download metadata", staging_path.string()).what());
        }
        written += static_cast<std::size_t>(result);
    }
    if (::fsync(fd.get()) != 0) {
        throw WeightTransferError(
            WeightFailureKind::Filesystem,
            mmltk::errno_error("cannot flush resumable download metadata", staging_path.string()).what());
    }
    std::error_code error;
    std::filesystem::rename(staging_path, metadata_path, error);
    if (error) {
        const std::string detail = error.message();
        std::error_code ignored;
        std::filesystem::remove(staging_path, ignored);
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  "cannot publish resumable download metadata: " + detail);
    }
    try {
        mmltk::sync_parent_directory(metadata_path);
    } catch (const std::exception& error_detail) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  "cannot sync resumable download metadata: " + std::string(error_detail.what()));
    }
}

class CurlGlobalState final {
   public:
    CurlGlobalState() {
        const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result != CURLE_OK) {
            throw WeightTransferError(WeightFailureKind::CannotDownload,
                                      std::string("libcurl initialization failed: ") + curl_easy_strerror(result));
        }
    }
    ~CurlGlobalState() {
        curl_global_cleanup();
    }
};

void ensure_curl_initialized() {
    static const CurlGlobalState state;
    (void)state;
}

[[nodiscard]] std::string_view trim_http_value(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() &&
           (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return value;
}

[[nodiscard]] bool equals_case_insensitive(const std::string_view value, const std::string_view expected) {
    return value.size() == expected.size() &&
           std::equal(expected.begin(), expected.end(), value.begin(), [](const char left, const char right) {
               return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
           });
}

[[nodiscard]] bool starts_with_case_insensitive(const std::string_view value, const std::string_view prefix) {
    return value.size() >= prefix.size() && equals_case_insensitive(value.substr(0U, prefix.size()), prefix);
}

[[nodiscard]] bool parse_uint64(const std::string_view value, std::uint64_t& output) noexcept {
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), output);
    return conversion.ec == std::errc{} && conversion.ptr == value.data() + value.size();
}

struct CurlDownloadContext {
    int fd = -1;
    const std::atomic<bool>* cancelled = nullptr;
    const std::filesystem::path* part_path = nullptr;
    const std::filesystem::path* metadata_path = nullptr;
    std::string_view url;
    std::string_view expected_md5;
    std::function<void(std::uint64_t, std::uint64_t, bool)> publish_progress;
    std::optional<PartialDownloadMetadata> prior_metadata;
    std::optional<PartialDownloadMetadata> active_metadata;
    std::uint64_t resume_offset = 0U;
    std::uint64_t progress_base = 0U;
    std::uint64_t reported_total = 0U;
    std::uint64_t content_range_start = 0U;
    std::uint64_t content_range_end = 0U;
    std::uint64_t last_reported_done = 0U;
    long response_code = 0L;
    std::string etag;
    std::string last_modified;
    std::string callback_error;
    bool accept_ranges = false;
    bool content_range_valid = false;
    bool content_range_unsatisfied = false;
    bool body_accepted = false;
    bool response_restarted = false;
    bool range_not_satisfiable = false;
    bool callback_failed = false;
    bool callback_discard_partial = false;
    WeightFailureKind callback_failure_kind = WeightFailureKind::CannotDownload;
    std::chrono::steady_clock::time_point last_progress_publish{};
};

void record_callback_failure(CurlDownloadContext& context, const WeightFailureKind kind, const std::string_view message,
                             const bool discard_partial = false) noexcept {
    context.callback_discard_partial = context.callback_discard_partial || discard_partial;
    if (context.callback_failed) {
        return;
    }
    context.callback_failed = true;
    context.callback_failure_kind = kind;
    try {
        context.callback_error.assign(message);
    } catch (...) {
        context.callback_error.clear();
    }
}

size_t curl_write_callback(char* data, const size_t size, const size_t count, void* userdata) noexcept {
    auto& context = *static_cast<CurlDownloadContext*>(userdata);
    try {
        if (count != 0U && size > std::numeric_limits<std::size_t>::max() / count) {
            record_callback_failure(context, WeightFailureKind::Filesystem,
                                    "libcurl weight write callback size overflow");
            return 0U;
        }
        const size_t bytes = size * count;
        if (!context.body_accepted) {
            return bytes;
        }
        size_t written = 0U;
        while (written < bytes) {
            const ssize_t result = ::write(context.fd, data + written, bytes - written);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                const std::runtime_error error =
                    mmltk::errno_error("cannot write partial weight download", context.part_path->string());
                record_callback_failure(context, WeightFailureKind::Filesystem, error.what());
                return 0U;
            }
            written += static_cast<size_t>(result);
        }
        return bytes;
    } catch (const std::exception& error) {
        record_callback_failure(context, WeightFailureKind::Filesystem, error.what());
        return 0U;
    } catch (...) {
        record_callback_failure(context, WeightFailureKind::Filesystem,
                                "unknown partial weight write callback failure");
        return 0U;
    }
}

void parse_http_status(CurlDownloadContext& context, const std::string_view line) {
    const size_t first_space = line.find(' ');
    if (first_space == std::string_view::npos) {
        return;
    }
    long status = 0L;
    const std::string_view number = line.substr(first_space + 1U, 3U);
    const auto conversion = std::from_chars(number.data(), number.data() + number.size(), status);
    if (conversion.ec != std::errc{}) {
        return;
    }
    context.response_code = status;
    context.etag.clear();
    context.last_modified.clear();
    context.accept_ranges = false;
    context.reported_total = 0U;
    context.content_range_start = 0U;
    context.content_range_end = 0U;
    context.content_range_valid = false;
    context.content_range_unsatisfied = false;
    context.body_accepted = false;
}

void parse_content_range(CurlDownloadContext& context, std::string_view value) {
    value = trim_http_value(value);
    if (!starts_with_case_insensitive(value, "bytes ")) {
        return;
    }
    value.remove_prefix(6U);
    const size_t slash = value.find('/');
    if (slash == std::string_view::npos || slash + 1U >= value.size()) {
        return;
    }
    std::uint64_t total = 0U;
    if (!parse_uint64(trim_http_value(value.substr(slash + 1U)), total) || total == 0U) {
        return;
    }
    const std::string_view range = trim_http_value(value.substr(0U, slash));
    context.reported_total = total;
    if (range == "*") {
        context.content_range_unsatisfied = true;
        return;
    }
    const size_t dash = range.find('-');
    if (dash == std::string_view::npos || dash == 0U || dash + 1U >= range.size()) {
        return;
    }
    std::uint64_t start = 0U;
    std::uint64_t end = 0U;
    if (parse_uint64(range.substr(0U, dash), start) && parse_uint64(range.substr(dash + 1U), end) && start <= end &&
        end < total) {
        context.content_range_start = start;
        context.content_range_end = end;
        context.content_range_valid = true;
    }
}

void persist_active_resume_metadata(CurlDownloadContext& context) {
    if (!context.body_accepted || context.reported_total == 0U || !context.accept_ranges) {
        context.active_metadata.reset();
        remove_resume_metadata(*context.metadata_path);
        return;
    }
    if (context.response_code == 206L && context.etag.empty() && context.last_modified.empty() &&
        context.prior_metadata.has_value()) {
        context.etag = context.prior_metadata->etag;
        context.last_modified = context.prior_metadata->last_modified;
    }
    if (context.etag.empty() && context.last_modified.empty()) {
        context.active_metadata.reset();
        remove_resume_metadata(*context.metadata_path);
        return;
    }
    PartialDownloadMetadata metadata{
        .url = std::string(context.url),
        .expected_md5 = std::string(context.expected_md5),
        .etag = context.etag,
        .last_modified = context.last_modified,
        .total_bytes = context.reported_total,
        .accept_ranges = true,
    };
    store_partial_metadata(*context.metadata_path, metadata);
    context.active_metadata = std::move(metadata);
}

size_t curl_header_callback(char* data, const size_t size, const size_t count, void* userdata) noexcept {
    auto& context = *static_cast<CurlDownloadContext*>(userdata);
    try {
        if (count != 0U && size > std::numeric_limits<std::size_t>::max() / count) {
            record_callback_failure(context, WeightFailureKind::Http, "libcurl weight header callback size overflow");
            return 0U;
        }
        const size_t bytes = size * count;
        const std::string_view line(data, bytes);
        if (starts_with_case_insensitive(line, "HTTP/")) {
            parse_http_status(context, line);
            return bytes;
        }
        if (line == "\r\n" || line == "\n") {
            if (context.resume_offset > 0U && context.response_code == 200L) {
                reset_partial_descriptor(context.fd, *context.part_path);
                context.progress_base = 0U;
                context.response_restarted = true;
            } else if (context.response_code == 206L) {
                if (context.resume_offset == 0U || !context.content_range_valid ||
                    context.content_range_start != context.resume_offset || !context.prior_metadata.has_value() ||
                    context.reported_total != context.prior_metadata->total_bytes) {
                    record_callback_failure(context, WeightFailureKind::Http,
                                            "server returned an inconsistent partial weight range", true);
                    return 0U;
                }
                context.reported_total = std::max(context.reported_total, context.content_range_end + 1U);
            } else if (context.resume_offset > 0U && context.response_code == 416L) {
                context.range_not_satisfiable = true;
                return bytes;
            }
            context.body_accepted = context.response_code == 200L || context.response_code == 206L;
            if (context.body_accepted) {
                persist_active_resume_metadata(context);
                context.publish_progress(context.progress_base, context.reported_total,
                                         context.progress_base > 0U && !context.response_restarted);
                context.last_progress_publish = std::chrono::steady_clock::now();
                context.last_reported_done = context.progress_base;
            }
            return bytes;
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return bytes;
        }
        const std::string_view name = line.substr(0U, colon);
        const std::string_view value = trim_http_value(line.substr(colon + 1U));
        if (equals_case_insensitive(name, "etag")) {
            context.etag.assign(value);
        } else if (equals_case_insensitive(name, "last-modified")) {
            context.last_modified.assign(value);
        } else if (equals_case_insensitive(name, "accept-ranges")) {
            context.accept_ranges = equals_case_insensitive(value, "bytes");
        } else if (equals_case_insensitive(name, "content-range")) {
            context.accept_ranges = true;
            parse_content_range(context, value);
        } else if (equals_case_insensitive(name, "content-length") && context.response_code == 200L) {
            std::uint64_t total = 0U;
            if (parse_uint64(value, total)) {
                context.reported_total = total;
            }
        }
        return bytes;
    } catch (const WeightTransferError& error) {
        record_callback_failure(context, error.kind(), error.what());
        return 0U;
    } catch (const std::exception& error) {
        record_callback_failure(context, WeightFailureKind::CannotDownload, error.what());
        return 0U;
    } catch (...) {
        record_callback_failure(context, WeightFailureKind::CannotDownload,
                                "unknown weight response header callback failure");
        return 0U;
    }
}

int curl_progress_callback(void* userdata, const curl_off_t download_total, const curl_off_t downloaded, curl_off_t,
                           curl_off_t) noexcept {
    auto& context = *static_cast<CurlDownloadContext*>(userdata);
    if (context.cancelled->load(std::memory_order_relaxed)) {
        return 1;
    }
    if (!context.body_accepted) {
        return 0;
    }
    try {
        const std::uint64_t transfer_done = static_cast<std::uint64_t>(std::max<curl_off_t>(0, downloaded));
        const std::uint64_t transfer_total = static_cast<std::uint64_t>(std::max<curl_off_t>(0, download_total));
        const std::uint64_t done = transfer_done > std::numeric_limits<std::uint64_t>::max() - context.progress_base
                                       ? std::numeric_limits<std::uint64_t>::max()
                                       : context.progress_base + transfer_done;
        const std::uint64_t fallback_total =
            transfer_total > std::numeric_limits<std::uint64_t>::max() - context.progress_base
                ? std::numeric_limits<std::uint64_t>::max()
                : context.progress_base + transfer_total;
        const std::uint64_t total = context.reported_total > 0U ? context.reported_total : fallback_total;
        const auto now = std::chrono::steady_clock::now();
        const bool crossed_completion = total > 0U && done >= total && context.last_reported_done < total;
        if (done >= context.last_reported_done &&
            (now - context.last_progress_publish >= std::chrono::milliseconds{100} || crossed_completion)) {
            context.publish_progress(done, total, context.resume_offset > 0U && !context.response_restarted);
            context.last_progress_publish = now;
            context.last_reported_done = done;
        }
    } catch (const std::exception& error) {
        record_callback_failure(context, WeightFailureKind::CannotDownload, error.what());
        return 1;
    } catch (...) {
        record_callback_failure(context, WeightFailureKind::CannotDownload, "unknown weight progress callback failure");
        return 1;
    }
    return 0;
}

template <typename Value>
void set_curl_option(CURL* curl, const CURLoption option, Value value, const std::string_view name) {
    const CURLcode result = curl_easy_setopt(curl, option, value);
    if (result != CURLE_OK) {
        throw WeightTransferError(WeightFailureKind::CannotDownload,
                                  "cannot configure libcurl " + std::string(name) + ": " + curl_easy_strerror(result));
    }
}

[[nodiscard]] WeightFailureKind curl_failure_kind(const CURLcode result) noexcept {
    switch (result) {
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
            return WeightFailureKind::NoConnection;
        default:
            return WeightFailureKind::CannotDownload;
    }
}

struct DownloadAttemptResult {
    std::uint64_t downloaded_bytes = 0U;
    std::uint64_t total_bytes = 0U;
    bool resumable = false;
    bool resumed = false;
    bool restarted = false;
    long response_code = 0L;
};

template <typename TraceFn>
[[nodiscard]] DownloadAttemptResult download_weight_attempt(
    const int partial_fd, const std::filesystem::path& part_path, const std::filesystem::path& metadata_path,
    const std::string_view url, const std::string_view expected_md5, const std::atomic<bool>& cancelled,
    const std::function<void(std::uint64_t, std::uint64_t, bool)>& publish_progress, TraceFn&& trace) {
    ensure_curl_initialized();
    std::uint64_t part_size = regular_file_size(partial_fd, part_path);
    std::optional<PartialDownloadMetadata> metadata = load_partial_metadata(metadata_path, url, expected_md5);
    if (!metadata.has_value() && part_size > 0U) {
        discard_partial_state(partial_fd, part_path, metadata_path);
        part_size = 0U;
    } else if (metadata.has_value() && part_size > metadata->total_bytes) {
        discard_partial_state(partial_fd, part_path, metadata_path);
        part_size = 0U;
        metadata.reset();
    }
    if (part_size == 0U) {
        reset_partial_descriptor(partial_fd, part_path);
    } else if (part_size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
               ::lseek(partial_fd, static_cast<off_t>(part_size), SEEK_SET) < 0) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  mmltk::errno_error("cannot seek partial weight download", part_path.string()).what());
    }

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) {
        throw WeightTransferError(WeightFailureKind::CannotDownload, "cannot create libcurl transfer handle");
    }
    CurlDownloadContext context;
    context.fd = partial_fd;
    context.cancelled = &cancelled;
    context.part_path = &part_path;
    context.metadata_path = &metadata_path;
    context.url = url;
    context.expected_md5 = expected_md5;
    context.publish_progress = publish_progress;
    context.prior_metadata = metadata;
    context.resume_offset = part_size;
    context.progress_base = part_size;
    context.reported_total = metadata.has_value() ? metadata->total_bytes : 0U;
    context.last_reported_done = part_size;
    char error_buffer[CURL_ERROR_SIZE]{};
    const std::string url_string(url);
    set_curl_option(curl.get(), CURLOPT_ERRORBUFFER, error_buffer, "error buffer");
    set_curl_option(curl.get(), CURLOPT_URL, url_string.c_str(), "URL");
    set_curl_option(curl.get(), CURLOPT_FOLLOWLOCATION, 1L, "redirect following");
    set_curl_option(curl.get(), CURLOPT_MAXREDIRS, 10L, "redirect limit");
    set_curl_option(curl.get(), CURLOPT_NOSIGNAL, 1L, "signal suppression");
    set_curl_option(curl.get(), CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L, "proxy header suppression");
    set_curl_option(curl.get(), CURLOPT_CONNECTTIMEOUT, 30L, "connect timeout");
    set_curl_option(curl.get(), CURLOPT_TIMEOUT, 3600L, "transfer timeout");
    set_curl_option(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1024L, "low-speed limit");
    set_curl_option(curl.get(), CURLOPT_LOW_SPEED_TIME, 30L, "low-speed timeout");
    set_curl_option(curl.get(), CURLOPT_USERAGENT, "mmltk/1 weight-acquisition", "user agent");
    set_curl_option(curl.get(), CURLOPT_WRITEFUNCTION, curl_write_callback, "write callback");
    set_curl_option(curl.get(), CURLOPT_WRITEDATA, &context, "write callback data");
    set_curl_option(curl.get(), CURLOPT_HEADERFUNCTION, curl_header_callback, "header callback");
    set_curl_option(curl.get(), CURLOPT_HEADERDATA, &context, "header callback data");
    set_curl_option(curl.get(), CURLOPT_NOPROGRESS, 0L, "progress enablement");
    set_curl_option(curl.get(), CURLOPT_XFERINFOFUNCTION, curl_progress_callback, "progress callback");
    set_curl_option(curl.get(), CURLOPT_XFERINFODATA, &context, "progress callback data");

    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, &curl_slist_free_all);
    std::string range;
    std::string if_range;
    if (part_size > 0U && metadata.has_value()) {
        range = std::to_string(part_size) + "-";
        set_curl_option(curl.get(), CURLOPT_RANGE, range.c_str(), "resume range");
        if_range = "If-Range: " + (!metadata->etag.empty() ? metadata->etag : metadata->last_modified);
        curl_slist* appended = curl_slist_append(nullptr, if_range.c_str());
        if (appended == nullptr) {
            throw WeightTransferError(WeightFailureKind::CannotDownload, "cannot allocate libcurl resume headers");
        }
        headers.reset(appended);
        set_curl_option(curl.get(), CURLOPT_HTTPHEADER, headers.get(), "resume headers");
    }
    const CURLcode result = curl_easy_perform(curl.get());
    long response_code = context.response_code;
    const CURLcode response_info = curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response_code);
    if (response_info != CURLE_OK) {
        throw WeightTransferError(
            WeightFailureKind::CannotDownload,
            std::string("cannot read libcurl response code: ") + curl_easy_strerror(response_info));
    }
    char* effective_url = nullptr;
    (void)curl_easy_getinfo(curl.get(), CURLINFO_EFFECTIVE_URL, &effective_url);
    const std::uint64_t downloaded_size = regular_file_size(partial_fd, part_path);
    const bool resumable = downloaded_size > 0U && context.active_metadata.has_value() &&
                           downloaded_size <= context.active_metadata->total_bytes;
    trace("weight.response", [&] {
        return nlohmann::json{{"response_code", response_code},
                              {"effective_url", effective_url != nullptr ? effective_url : ""},
                              {"resume_offset", part_size},
                              {"downloaded_bytes", downloaded_size},
                              {"total_bytes", context.reported_total},
                              {"accept_ranges", context.accept_ranges},
                              {"content_range_valid", context.content_range_valid},
                              {"response_restarted", context.response_restarted},
                              {"resumable", resumable},
                              {"curl_result", static_cast<int>(result)}};
    });
    if (context.callback_failed) {
        throw WeightTransferError(
            context.callback_failure_kind,
            context.callback_error.empty() ? "weight transfer callback failed" : context.callback_error,
            context.callback_discard_partial);
    }
    if (result != CURLE_OK) {
        if (cancelled.load(std::memory_order_relaxed)) {
            throw WeightTransferError(WeightFailureKind::Cancelled, "weight acquisition cancelled");
        }
        const std::string detail = error_buffer[0] != '\0' ? std::string(error_buffer) : curl_easy_strerror(result);
        throw WeightTransferError(curl_failure_kind(result), "weight download failed: " + detail);
    }
    if (context.range_not_satisfiable) {
        const std::uint64_t complete_total =
            context.content_range_unsatisfied && context.reported_total > 0U
                ? context.reported_total
                : (context.prior_metadata.has_value() ? context.prior_metadata->total_bytes : 0U);
        if (complete_total > 0U && downloaded_size == complete_total) {
            if (::fsync(partial_fd) != 0) {
                throw WeightTransferError(
                    WeightFailureKind::Filesystem,
                    mmltk::errno_error("cannot flush complete partial weight download", part_path.string()).what());
            }
            publish_progress(downloaded_size, complete_total, false);
            return DownloadAttemptResult{downloaded_size, complete_total, false, true, false, response_code};
        }
        throw WeightTransferError(WeightFailureKind::Http,
                                  "server rejected an incomplete partial weight range; restarting", true);
    }
    if (!(response_code == 200L || response_code == 206L)) {
        throw WeightTransferError(WeightFailureKind::Http,
                                  "weight download returned HTTP " + std::to_string(response_code));
    }
    if (context.reported_total > 0U && downloaded_size != context.reported_total) {
        if (downloaded_size > context.reported_total) {
            throw WeightTransferError(WeightFailureKind::Http, "weight download exceeded the declared response size",
                                      true);
        }
        throw WeightTransferError(WeightFailureKind::CannotDownload,
                                  "weight download ended before the declared response size");
    }
    if (::fsync(partial_fd) != 0) {
        throw WeightTransferError(
            WeightFailureKind::Filesystem,
            mmltk::errno_error("cannot flush partial weight download", part_path.string()).what());
    }
    const std::uint64_t total = context.reported_total > 0U ? context.reported_total : downloaded_size;
    publish_progress(downloaded_size, total, resumable);
    return DownloadAttemptResult{
        downloaded_size, total, resumable, part_size > 0U && !context.response_restarted, context.response_restarted,
        response_code};
}

struct CanonicalWeightResult {
    std::string path;
    std::uint64_t downloaded_bytes = 0U;
    std::uint64_t total_bytes = 0U;
    std::uint32_t attempt = 0U;
};

template <typename TraceFn, typename UpdateFn>
[[nodiscard]] CanonicalWeightResult acquire_canonical_weight(const std::string_view preset_name,
                                                             const std::atomic<bool>& cancelled, TraceFn&& trace,
                                                             UpdateFn&& update) {
    const auto* preset = mmltk::rfdetr::find_preset_catalog_entry(preset_name);
    if (preset == nullptr) {
        throw WeightTransferError(WeightFailureKind::Incompatible,
                                  "unknown RF-DETR preset: " + std::string(preset_name));
    }
    const auto* asset = mmltk::rfdetr::find_weight_asset(preset->canonical_weight_filename);
    if (asset == nullptr) {
        throw WeightTransferError(WeightFailureKind::CannotDownload,
                                  "RF-DETR preset has no canonical weight asset: " + std::string(preset_name));
    }
    const std::filesystem::path cache_root = repository_cache_root();
    try {
        std::filesystem::create_directories(cache_root);
    } catch (const std::filesystem::filesystem_error& error) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  "cannot create weight cache directory: " + std::string(error.what()));
    }
    const std::filesystem::path destination = canonical_weight_cache_path(preset_name);
    const std::filesystem::path part_path = destination.string() + ".part";
    const std::filesystem::path metadata_path = destination.string() + ".part.json";
    const std::filesystem::path legacy_lock_path = destination.string() + ".lock";
    const auto cancellable_wait = [&](const std::chrono::milliseconds delay) {
        const auto deadline = std::chrono::steady_clock::now() + delay;
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancelled.load(std::memory_order_relaxed)) {
                throw WeightTransferError(WeightFailureKind::Cancelled, "weight acquisition cancelled");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    };
    trace("weight.verification_delay_start",
          [&] { return nlohmann::json{{"preset_name", std::string(preset_name)}, {"delay_ms", 2000U}}; });
    cancellable_wait(std::chrono::seconds{2});
    trace("weight.verification_delay_complete",
          [&] { return nlohmann::json{{"preset_name", std::string(preset_name)}}; });
    trace("weight.partial_lease_wait",
          [&] { return nlohmann::json{{"preset_name", std::string(preset_name)}, {"part_path", part_path.string()}}; });
    const bool removed_legacy_lock = remove_legacy_lockfile(legacy_lock_path, cancelled);
    PartialFileLease partial_lease(part_path, cancelled);
    trace("weight.partial_lease_acquired", [&] {
        return nlohmann::json{{"preset_name", std::string(preset_name)},
                              {"part_path", part_path.string()},
                              {"removed_legacy_lock", removed_legacy_lock}};
    });

    std::error_code cleanup_error;
    const bool removed_staging_metadata = std::filesystem::remove(metadata_path.string() + ".next", cleanup_error);
    if (cleanup_error) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  "cannot remove abandoned weight metadata staging file: " + cleanup_error.message());
    }

    std::error_code final_status_error;
    const std::filesystem::file_status final_status = std::filesystem::symlink_status(destination, final_status_error);
    if (final_status_error && final_status_error != std::errc::no_such_file_or_directory) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  "cannot inspect cached RF-DETR weights: " + final_status_error.message());
    }
    const bool final_exists = !final_status_error && final_status.type() != std::filesystem::file_type::not_found;
    if (final_exists && final_status.type() != std::filesystem::file_type::regular) {
        throw WeightTransferError(WeightFailureKind::Filesystem,
                                  "cached RF-DETR weight path is not a regular file: " + destination.string());
    }
    if (final_exists) {
        trace("weight.cache_verification_start", [&] {
            std::error_code size_error;
            const std::uint64_t size_bytes = std::filesystem::file_size(destination, size_error);
            return nlohmann::json{{"preset_name", std::string(preset_name)},
                                  {"path", destination.string()},
                                  {"size_bytes", size_error ? 0U : size_bytes},
                                  {"size_error", size_error ? size_error.message() : std::string{}}};
        });
        std::string digest;
        try {
            digest = file_md5(destination, &cancelled);
        } catch (const std::exception& error) {
            if (cancelled.load(std::memory_order_relaxed)) {
                throw WeightTransferError(WeightFailureKind::Cancelled, "weight acquisition cancelled");
            }
            throw WeightTransferError(WeightFailureKind::Checksum,
                                      "cannot verify cached RF-DETR weights: " + std::string(error.what()));
        }
        trace("weight.cache_check", [&] {
            return nlohmann::json{{"preset_name", std::string(preset_name)}, {"valid", digest == asset->md5_hash}};
        });
        if (digest == asset->md5_hash) {
            std::error_code size_error;
            const std::uint64_t size_bytes = std::filesystem::file_size(destination, size_error);
            if (size_error) {
                throw WeightTransferError(WeightFailureKind::Filesystem,
                                          "cannot size cached RF-DETR weights: " + size_error.message());
            }
            remove_transient_state(part_path, metadata_path);
            trace("weight.cache_reconcile", [&] {
                return nlohmann::json{{"preset_name", std::string(preset_name)},
                                      {"action", "use_final"},
                                      {"size_bytes", size_bytes},
                                      {"removed_staging_metadata", removed_staging_metadata}};
            });
            return CanonicalWeightResult{destination.string(), size_bytes, size_bytes, 0U};
        }
        std::error_code remove_error;
        std::filesystem::remove(destination, remove_error);
        if (remove_error) {
            throw WeightTransferError(WeightFailureKind::Filesystem,
                                      "cannot remove invalid cached RF-DETR weights: " + remove_error.message());
        }
        trace("weight.cache_reconcile", [&] {
            return nlohmann::json{{"preset_name", std::string(preset_name)}, {"action", "remove_invalid_final"}};
        });
    }
    if (cancelled.load(std::memory_order_relaxed)) {
        throw WeightTransferError(WeightFailureKind::Cancelled, "weight acquisition cancelled");
    }

    std::uint64_t partial_bytes = regular_file_size(partial_lease.fd(), part_path);
    std::optional<PartialDownloadMetadata> partial_metadata =
        load_partial_metadata(metadata_path, asset->download_url, asset->md5_hash);
    std::string_view reconcile_action = "start_empty";
    if (partial_bytes == 0U) {
        if (partial_metadata.has_value()) {
            remove_resume_metadata(metadata_path);
            partial_metadata.reset();
            reconcile_action = "remove_orphan_metadata";
        }
    } else if (!partial_metadata.has_value() || partial_bytes > partial_metadata->total_bytes) {
        discard_partial_state(partial_lease.fd(), part_path, metadata_path);
        partial_bytes = 0U;
        partial_metadata.reset();
        reconcile_action = "discard_invalid_partial";
    } else if (partial_bytes == partial_metadata->total_bytes) {
        if (::fsync(partial_lease.fd()) != 0) {
            throw WeightTransferError(
                WeightFailureKind::Filesystem,
                mmltk::errno_error("cannot flush complete partial RF-DETR weights", part_path.string()).what());
        }
        require_leased_path_identity(partial_lease.fd(), part_path);
        std::string digest;
        try {
            digest = leased_file_md5(partial_lease.fd(), part_path, &cancelled);
        } catch (const std::exception& error) {
            if (cancelled.load(std::memory_order_relaxed)) {
                throw WeightTransferError(WeightFailureKind::Cancelled, "weight acquisition cancelled");
            }
            throw WeightTransferError(WeightFailureKind::Checksum,
                                      "cannot verify complete partial RF-DETR weights: " + std::string(error.what()));
        }
        if (digest == asset->md5_hash) {
            require_leased_path_identity(partial_lease.fd(), part_path);
            try {
                mmltk::publish_staged_path_atomically(part_path, destination);
            } catch (const std::exception& error) {
                throw WeightTransferError(
                    WeightFailureKind::Filesystem,
                    "cannot publish complete partial RF-DETR weights: " + std::string(error.what()));
            }
            remove_resume_metadata(metadata_path);
            trace("weight.cache_reconcile", [&] {
                return nlohmann::json{{"preset_name", std::string(preset_name)},
                                      {"action", "publish_complete_partial"},
                                      {"size_bytes", partial_bytes}};
            });
            return CanonicalWeightResult{destination.string(), partial_bytes, partial_bytes, 0U};
        }
        discard_partial_state(partial_lease.fd(), part_path, metadata_path);
        partial_bytes = 0U;
        partial_metadata.reset();
        reconcile_action = "discard_corrupt_complete_partial";
    } else {
        reconcile_action = "resume_partial";
    }
    trace("weight.cache_reconcile", [&] {
        return nlohmann::json{{"preset_name", std::string(preset_name)},
                              {"action", std::string(reconcile_action)},
                              {"partial_bytes", partial_bytes},
                              {"total_bytes", partial_metadata.has_value() ? partial_metadata->total_bytes : 0U},
                              {"resumable", partial_bytes > 0U && partial_metadata.has_value()},
                              {"removed_staging_metadata", removed_staging_metadata}};
    });

    constexpr std::array retry_delays{std::chrono::seconds{3}, std::chrono::seconds{15}};
    for (std::uint32_t attempt = 1U; attempt <= 3U; ++attempt) {
        partial_bytes = regular_file_size(partial_lease.fd(), part_path);
        partial_metadata = load_partial_metadata(metadata_path, asset->download_url, asset->md5_hash);
        const std::uint64_t partial_total = partial_metadata.has_value() ? partial_metadata->total_bytes : 0U;
        const bool starts_resumable =
            partial_bytes > 0U && partial_metadata.has_value() && partial_bytes < partial_metadata->total_bytes;
        update(WeightStatus::Downloading, partial_bytes, partial_total, attempt, starts_resumable, std::string{},
               std::chrono::steady_clock::time_point{});
        trace("weight.download_start", [&] {
            return nlohmann::json{{"preset_name", std::string(preset_name)},
                                  {"destination", destination.string()},
                                  {"attempt", attempt},
                                  {"resume_offset", partial_bytes},
                                  {"total_bytes", partial_total},
                                  {"resumable", starts_resumable}};
        });
        bool published = false;
        try {
            const DownloadAttemptResult download = download_weight_attempt(
                partial_lease.fd(), part_path, metadata_path, asset->download_url, asset->md5_hash, cancelled,
                [&](const std::uint64_t done, const std::uint64_t total, const bool resumable) {
                    update(WeightStatus::Downloading, done, total, attempt, resumable, std::string{},
                           std::chrono::steady_clock::time_point{});
                },
                trace);
            require_leased_path_identity(partial_lease.fd(), part_path);
            std::string digest;
            try {
                digest = leased_file_md5(partial_lease.fd(), part_path, &cancelled);
            } catch (const std::exception& error) {
                if (cancelled.load(std::memory_order_relaxed)) {
                    throw WeightTransferError(WeightFailureKind::Cancelled, "weight acquisition cancelled");
                }
                throw WeightTransferError(WeightFailureKind::Checksum,
                                          "cannot verify downloaded RF-DETR weights: " + std::string(error.what()));
            }
            trace("weight.checksum", [&] {
                return nlohmann::json{{"preset_name", std::string(preset_name)}, {"valid", digest == asset->md5_hash}};
            });
            if (digest != asset->md5_hash) {
                throw WeightTransferError(WeightFailureKind::Checksum,
                                          "downloaded weight checksum mismatch for " + std::string(preset_name), true);
            }
            require_leased_path_identity(partial_lease.fd(), part_path);
            try {
                mmltk::publish_staged_path_atomically(part_path, destination);
                published = true;
            } catch (const std::exception& error) {
                throw WeightTransferError(WeightFailureKind::Filesystem,
                                          "cannot publish RF-DETR weights: " + std::string(error.what()));
            }
            remove_resume_metadata(metadata_path);
            trace("weight.publish", [&] {
                return nlohmann::json{{"preset_name", std::string(preset_name)},
                                      {"path", destination.string()},
                                      {"downloaded_bytes", download.downloaded_bytes},
                                      {"total_bytes", download.total_bytes},
                                      {"attempt", attempt},
                                      {"resumed", download.resumed},
                                      {"restarted", download.restarted},
                                      {"response_code", download.response_code}};
            });
            return CanonicalWeightResult{destination.string(), download.downloaded_bytes, download.total_bytes,
                                         attempt};
        } catch (const WeightTransferError& error) {
            if (published) {
                throw;
            }
            if (error.discard_partial()) {
                discard_partial_state(partial_lease.fd(), part_path, metadata_path);
            }
            partial_bytes = regular_file_size(partial_lease.fd(), part_path);
            partial_metadata = load_partial_metadata(metadata_path, asset->download_url, asset->md5_hash);
            std::uint64_t retained_total = partial_metadata.has_value() ? partial_metadata->total_bytes : 0U;
            const bool can_resume =
                partial_bytes > 0U && partial_metadata.has_value() && partial_bytes <= partial_metadata->total_bytes;
            if (!can_resume) {
                discard_partial_state(partial_lease.fd(), part_path, metadata_path);
                partial_bytes = 0U;
                retained_total = 0U;
            }
            trace("weight.download_attempt_failed", [&] {
                return nlohmann::json{{"preset_name", std::string(preset_name)},
                                      {"attempt", attempt},
                                      {"failure_kind", weight_status_name(weight_status_for_failure(error.kind()))},
                                      {"error", error.what()},
                                      {"retained_bytes", partial_bytes},
                                      {"total_bytes", retained_total},
                                      {"resumable", can_resume},
                                      {"retryable", weight_failure_is_retryable(error.kind())}};
            });
            if (!weight_failure_is_retryable(error.kind()) || attempt == 3U) {
                throw;
            }
            const auto deadline = std::chrono::steady_clock::now() + retry_delays[attempt - 1U];
            update(WeightStatus::RetryWaiting, partial_bytes, retained_total, attempt, can_resume, error.what(),
                   deadline);
            cancellable_wait(std::chrono::duration_cast<std::chrono::milliseconds>(retry_delays[attempt - 1U]));
        }
    }
    throw WeightTransferError(WeightFailureKind::CannotDownload, "weight acquisition exhausted retries");
}

}  

std::string canonical_weight_cache_path(const std::string_view preset_name) {
    const auto* preset = mmltk::rfdetr::find_preset_catalog_entry(preset_name);
    if (preset == nullptr) {
        throw std::runtime_error("unknown RF-DETR preset: " + std::string(preset_name));
    }
    return std::filesystem::absolute(repository_cache_root() / std::string(preset->canonical_weight_filename))
        .lexically_normal()
        .string();
}

struct BrowserArtifactController::Impl {
    Impl(mmltk::runtime::BackgroundExecutor& executor_in, mmltk::runtime::UiCallbackQueue& ui_callbacks_in)
        : executor(executor_in), ui_callbacks(ui_callbacks_in), compile_worker_cpus(mmltk::allowed_cpu_set()) {}

    mmltk::runtime::BackgroundExecutor& executor;
    mmltk::runtime::UiCallbackQueue& ui_callbacks;
    std::vector<int> compile_worker_cpus;
    mutable std::mutex mutex;
    DatasetOperationState dataset;
    WeightOperationState weight;
    std::optional<PendingDatasetInspection> pending_dataset_inspection;
    std::shared_ptr<std::atomic<bool>> dataset_cancel = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> weight_cancel = std::make_shared<std::atomic<bool>>(false);
    bool shutting_down = false;
    BrowserArtifactController::TraceSink trace_sink;
    std::atomic<bool> trace_enabled{false};

    [[nodiscard]] BrowserArtifactController::TraceSink trace_sink_copy() const {
        if (!trace_enabled.load(std::memory_order_acquire)) {
            return {};
        }
        std::lock_guard lock(mutex);
        return trace_sink;
    }

    template <typename FieldsFn>
    void trace(std::string_view event, FieldsFn&& fields_fn) const {
        BrowserArtifactController::TraceSink sink = trace_sink_copy();
        if (sink) {
            sink(event, fields_fn());
        }
    }
};

BrowserArtifactController::BrowserArtifactController(mmltk::runtime::BackgroundExecutor& executor,
                                                     mmltk::runtime::UiCallbackQueue& ui_callbacks)
    : impl_(std::make_shared<Impl>(executor, ui_callbacks)) {}

BrowserArtifactController::~BrowserArtifactController() {
    set_trace_sink({});
    shutdown();
}

void BrowserArtifactController::set_trace_sink(TraceSink sink) {
    if (!sink) {
        impl_->trace_enabled.store(false, std::memory_order_release);
    }
    std::lock_guard lock(impl_->mutex);
    impl_->trace_sink = std::move(sink);
    if (impl_->trace_sink) {
        impl_->trace_enabled.store(true, std::memory_order_release);
    }
}

template <typename ImplPtr>
void submit_dataset_inspection(ImplPtr impl, std::string train_path, std::string val_path, std::string test_path,
                               std::string preset_name, const std::uint32_t resolution) {
    std::uint64_t generation = 0U;
    {
        std::lock_guard lock(impl->mutex);
        if (impl->shutting_down) {
            return;
        }
        if (impl->dataset.phase == OperationPhase::Running && impl->dataset.compiling) {
            impl->pending_dataset_inspection = PendingDatasetInspection{
                {std::move(train_path), std::move(val_path), std::move(test_path)}, std::move(preset_name), resolution};
            return;
        }
        impl->pending_dataset_inspection.reset();
        generation = ++impl->dataset.generation;
        impl->dataset.phase = OperationPhase::Running;
        impl->dataset.preset_name = preset_name;
        impl->dataset.error.clear();
        impl->dataset.splits.clear();
        impl->dataset.compatible = false;
        impl->dataset.compiling = false;
    }
    impl->trace("dataset.inspect_start", [&] {
        return nlohmann::json{{"generation", generation}, {"preset_name", preset_name}, {"resolution", resolution}};
    });
    const std::array<std::string, 3U> paths{std::move(train_path), std::move(val_path), std::move(test_path)};
    mmltk::runtime::submit_background_task(
        impl->executor, impl->ui_callbacks,
        [paths, preset_name = std::move(preset_name), resolution] {
            return inspect_dataset_paths(paths, preset_name, resolution);
        },
        [impl, generation](DatasetInspectionResult result) {
            const BrowserArtifactController::TraceSink trace = impl->trace_sink_copy();
            const bool compatible = result.compatible;
            const bool failed = !result.error.empty();
            const std::string trace_error = trace ? result.error : std::string{};
            {
                std::lock_guard lock(impl->mutex);
                if (impl->dataset.generation != generation || impl->shutting_down) {
                    return;
                }
                impl->dataset.phase = failed ? OperationPhase::Failed : OperationPhase::Complete;
                impl->dataset.splits = std::move(result.splits);
                impl->dataset.compatible = result.compatible;
                impl->dataset.error = std::move(result.error);
            }
            if (trace) {
                trace(failed ? "dataset.inspect_failed" : "dataset.inspect_complete",
                      {{"generation", generation}, {"compatible", compatible}, {"error", trace_error}});
            }
        },
        [impl, generation](const std::string& error) {
            {
                std::lock_guard lock(impl->mutex);
                if (impl->dataset.generation == generation && !impl->shutting_down) {
                    impl->dataset.phase = OperationPhase::Failed;
                    impl->dataset.error = error;
                }
            }
            impl->trace("dataset.inspect_failed",
                        [&] { return nlohmann::json{{"generation", generation}, {"error", error}}; });
        });
}

template <typename ImplPtr>
void submit_pending_dataset_inspection(const ImplPtr& impl) {
    std::optional<PendingDatasetInspection> pending;
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->shutting_down && !impl->dataset.compiling) {
            pending = std::move(impl->pending_dataset_inspection);
            impl->pending_dataset_inspection.reset();
        }
    }
    if (pending.has_value()) {
        submit_dataset_inspection(impl, std::move(pending->paths[0]), std::move(pending->paths[1]),
                                  std::move(pending->paths[2]), std::move(pending->preset_name), pending->resolution);
    }
}

void BrowserArtifactController::inspect_dataset(std::string train_path, std::string val_path, std::string test_path,
                                                std::string preset_name, const std::uint32_t resolution) {
    submit_dataset_inspection(impl_, std::move(train_path), std::move(val_path), std::move(test_path),
                              std::move(preset_name), resolution);
}

void BrowserArtifactController::compile_dataset(std::string source_dir, std::string output_dir, std::string preset_name,
                                                const std::uint32_t resolution, const bool overwrite,
                                                Completion on_complete) {
    if (source_dir.empty() || output_dir.empty()) {
        throw std::runtime_error("dataset compilation requires non-empty source and output directories");
    }
    const auto* preset = mmltk::rfdetr::find_preset_catalog_entry(preset_name);
    if (preset == nullptr) {
        throw std::runtime_error("unknown RF-DETR preset: " + preset_name);
    }
    const std::shared_ptr<Impl> impl = impl_;
    std::uint64_t generation = 0U;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    const auto started_at = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(impl->mutex);
        if (impl->shutting_down || (impl->dataset.phase == OperationPhase::Running && impl->dataset.compiling)) {
            throw std::runtime_error("dataset artifact operation is already running or shutting down");
        }
        generation = ++impl->dataset.generation;
        DatasetOperationState state;
        state.phase = OperationPhase::Running;
        state.compile_phase = DatasetCompilePhase::Planning;
        state.generation = generation;
        state.source_dir = source_dir;
        state.output_dir = output_dir;
        state.preset_name = preset_name;
        state.compiling = true;
        state.started_at = started_at;
        state.eta = std::make_unique<CompileEtaEstimator>(started_at);
        impl->dataset = std::move(state);
        impl->dataset_cancel = cancelled;
    }
    impl->trace("dataset.compile_start", [&] {
        return nlohmann::json{{"generation", generation},   {"source_dir", source_dir}, {"output_dir", output_dir},
                              {"preset_name", preset_name}, {"resolution", resolution}, {"overwrite", overwrite}};
    });
    mmltk::runtime::submit_background_task(
        impl->executor, impl->ui_callbacks,
        [impl, generation, source_dir = std::move(source_dir), output_dir = std::move(output_dir), resolution,
         overwrite, cancelled, started_at]() {
            const std::filesystem::path output(output_dir);
            validate_nonoverlapping_directories(source_dir, output);
            if (!overwrite && std::filesystem::exists(output)) {
                throw std::runtime_error(
                    "compiled dataset destination already exists; enable Overwrite to replace it: " + output.string());
            }
            const std::filesystem::path staging = create_staging_directory(output);
            StagingDirectoryCleanup cleanup(staging);
            std::vector<std::string> split_names;
            split_names.reserve(3U);
            for (const std::string_view split :
                 {std::string_view{"train"}, std::string_view{"val"}, std::string_view{"test"}}) {
                if (std::filesystem::is_directory(std::filesystem::path(source_dir) / split)) {
                    split_names.emplace_back(split);
                }
            }
            if (split_names.size() < 2U || split_names[0] != "train" || split_names[1] != "val") {
                throw std::runtime_error("raw dataset must contain train and val split directories");
            }

            std::vector<mmltk::CompileDiagnostic> diagnostics;
            mmltk::CompilerConfig config;
            config.source_dir = source_dir;
            config.output_dir = staging.string();
            config.target_width = resolution;
            config.target_height = resolution;
            config.worker_cpus = impl->compile_worker_cpus;
            config.cancel_requested = cancelled.get();
            if (impl->trace_enabled.load(std::memory_order_acquire)) {
                config.diagnostics = &diagnostics;
            }

            mmltk::DatasetCompilePlan plan = mmltk::DatasetCompiler::prepare(std::move(config), split_names);
            const std::size_t operation_total = plan.total_steps();
            std::uint64_t unstarted_images = 0U;
            for (const mmltk::DatasetCompileSplitPlan& split : plan.splits) {
                unstarted_images += split.image_count;
            }
            {
                std::lock_guard lock(impl->mutex);
                if (impl->dataset.generation == generation) {
                    impl->dataset.total = operation_total;
                    impl->dataset.future_images = unstarted_images;
                    impl->dataset.compile_phase = DatasetCompilePhase::Labels;
                }
            }

            std::size_t completed_steps = 0U;
            std::uint64_t completed_dropped_instances = 0U;
            std::size_t diagnostic_cursor = 0U;
            for (std::size_t split_index = 0U; split_index < plan.splits.size(); ++split_index) {
                const mmltk::DatasetCompileSplitPlan& split_plan = plan.splits[split_index];
                const std::string& split = split_plan.split;
                unstarted_images -= split_plan.image_count;
                auto telemetry = std::make_shared<mmltk::CompileTelemetry>(split_plan.image_count);
                {
                    std::lock_guard lock(impl->mutex);
                    if (impl->dataset.generation == generation) {
                        impl->dataset.active_split = split;
                        impl->dataset.compile_phase = DatasetCompilePhase::Labels;
                        impl->dataset.compile_telemetry = telemetry;
                        impl->dataset.completed_steps = completed_steps;
                        impl->dataset.future_images = unstarted_images;
                        impl->dataset.completed_dropped_instances = completed_dropped_instances;
                        if (impl->dataset.eta != nullptr) {
                            impl->dataset.eta->begin_split();
                        }
                    }
                }
                impl->trace("dataset.compile_split", [&] {
                    return nlohmann::json{{"generation", generation},
                                          {"split", split},
                                          {"image_count", split_plan.image_count},
                                          {"completed_steps", completed_steps},
                                          {"total_steps", operation_total}};
                });

                mmltk::DatasetCompiler::compile(plan, split_index, telemetry.get());
                const mmltk::CompileProgress final_progress = telemetry->snapshot();
                completed_steps += static_cast<std::size_t>(split_plan.image_count) * 2U + 1U;
                completed_dropped_instances += final_progress.dropped_instances;
                {
                    std::lock_guard lock(impl->mutex);
                    if (impl->dataset.generation == generation) {
                        impl->dataset.done = std::min(completed_steps, operation_total);
                        impl->dataset.completed_steps = completed_steps;
                        impl->dataset.dropped_instances = completed_dropped_instances;
                        impl->dataset.completed_dropped_instances = completed_dropped_instances;
                        if (impl->dataset.compile_telemetry == telemetry) {
                            impl->dataset.compile_telemetry.reset();
                        }
                    }
                }

                for (; diagnostic_cursor < diagnostics.size(); ++diagnostic_cursor) {
                    const mmltk::CompileDiagnostic& diagnostic = diagnostics[diagnostic_cursor];
                    impl->trace("dataset.compile_diagnostic", [&] {
                        const std::string_view kind =
                            diagnostic.kind == mmltk::CompileDiagnosticKind::kDroppedInstanceAfterResize
                                ? "dropped_instance_after_resize"
                                : "source_bounding_box_mismatch";
                        return nlohmann::json{{"generation", generation},
                                              {"kind", kind},
                                              {"annotation_path", diagnostic.annotation_path},
                                              {"class_name", diagnostic.class_name},
                                              {"line", diagnostic.line},
                                              {"source_width", diagnostic.source_width},
                                              {"source_height", diagnostic.source_height},
                                              {"target_width", diagnostic.target_width},
                                              {"target_height", diagnostic.target_height},
                                              {"source_foreground", diagnostic.source_foreground},
                                              {"declared_bbox", diagnostic.declared_bbox},
                                              {"mask_bbox", diagnostic.mask_bbox}};
                    });
                }
            }
            if (cancelled->load(std::memory_order_relaxed)) {
                throw std::runtime_error("dataset compilation cancelled");
            }
            {
                std::lock_guard lock(impl->mutex);
                if (impl->dataset.generation == generation) {
                    impl->dataset.compile_phase = DatasetCompilePhase::Publishing;
                    impl->dataset.done = operation_total;
                    impl->dataset.total = operation_total;
                    impl->dataset.elapsed_ms =
                        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now() - started_at)
                                                       .count());
                    impl->dataset.remaining_ms = 0U;
                    impl->dataset.eta_ready = false;
                    impl->dataset.compile_telemetry.reset();
                }
            }
            mmltk::publish_staged_path_atomically(staging, output, overwrite);
            cleanup.published();
            impl->trace("dataset.publish",
                        [&] { return nlohmann::json{{"generation", generation}, {"output_dir", output.string()}}; });
            return output.string();
        },
        [impl, generation, on_complete = std::move(on_complete)](std::string output) mutable {
            {
                std::lock_guard lock(impl->mutex);
                if (impl->dataset.generation != generation || impl->shutting_down) {
                    return;
                }
                impl->dataset.phase = OperationPhase::Complete;
                impl->dataset.output_dir = output;
                impl->dataset.active_split.clear();
                impl->dataset.error.clear();
                impl->dataset.compiling = false;
                impl->dataset.compile_telemetry.reset();
                impl->dataset.eta.reset();
                impl->dataset.elapsed_ms =
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now() - impl->dataset.started_at)
                                                   .count());
            }
            impl->trace("dataset.compile_complete",
                        [&] { return nlohmann::json{{"generation", generation}, {"output_dir", output}}; });
            if (on_complete) {
                on_complete(output);
            }
            submit_pending_dataset_inspection(impl);
        },
        [impl, generation, cancelled](const std::string& error) {
            const bool was_cancelled = cancelled->load(std::memory_order_relaxed);
            {
                std::lock_guard lock(impl->mutex);
                if (impl->dataset.generation == generation && !impl->shutting_down) {
                    impl->dataset.phase = was_cancelled ? OperationPhase::Cancelled : OperationPhase::Failed;
                    impl->dataset.error = error;
                    impl->dataset.compiling = false;
                    impl->dataset.compile_telemetry.reset();
                    impl->dataset.eta.reset();
                    impl->dataset.elapsed_ms =
                        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now() - impl->dataset.started_at)
                                                       .count());
                }
            }
            impl->trace(was_cancelled ? "dataset.compile_cancelled" : "dataset.compile_failed",
                        [&] { return nlohmann::json{{"generation", generation}, {"error", error}}; });
            submit_pending_dataset_inspection(impl);
        });
}

void BrowserArtifactController::cancel_dataset_compile() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->dataset_cancel->store(true, std::memory_order_relaxed);
    }
    impl_->trace("dataset.cancel_requested", [] { return nlohmann::json::object(); });
}

void BrowserArtifactController::acquire_weight(std::string preset_name, Completion on_complete) {
    const std::shared_ptr<Impl> impl = impl_;
    std::uint64_t generation = 0U;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard lock(impl->mutex);
        if (impl->shutting_down) {
            return;
        }
        generation = ++impl->weight.generation;
        impl->weight = WeightOperationState{};
        impl->weight.phase = OperationPhase::Running;
        impl->weight.status = WeightStatus::Verifying;
        impl->weight.generation = generation;
        impl->weight.preset_name = preset_name;
        impl->weight_cancel->store(true, std::memory_order_relaxed);
        impl->weight_cancel = cancelled;
    }
    impl->trace("weight.acquire_start",
                [&] { return nlohmann::json{{"generation", generation}, {"preset_name", preset_name}}; });
    mmltk::logging::logger("gui")->debug("canonical weight acquisition queued: preset={} generation={}", preset_name,
                                         generation);
    mmltk::runtime::submit_background_task(
        impl->executor, impl->ui_callbacks,
        [impl, generation, preset_name = std::move(preset_name), cancelled] {
            impl->trace("weight.worker_start",
                        [&] { return nlohmann::json{{"generation", generation}, {"preset_name", preset_name}}; });
            mmltk::logging::logger("gui")->debug("canonical weight worker started: preset={} generation={}",
                                                 preset_name, generation);
            const auto update = [impl, generation](const WeightStatus status, const std::uint64_t done,
                                                   const std::uint64_t total, const std::uint32_t attempt,
                                                   const bool resumable, const std::string& error,
                                                   const std::chrono::steady_clock::time_point retry_deadline) {
                {
                    std::lock_guard lock(impl->mutex);
                    if (impl->weight.generation != generation || impl->shutting_down) {
                        return;
                    }
                    impl->weight.status = status;
                    impl->weight.downloaded_bytes = done;
                    impl->weight.total_bytes = total;
                    impl->weight.attempt = attempt;
                    impl->weight.resumable = resumable;
                    impl->weight.error = error;
                    impl->weight.retry_deadline = retry_deadline;
                }
                impl->trace("weight.state", [&] {
                    return nlohmann::json{{"generation", generation},
                                          {"status", weight_status_name(status)},
                                          {"downloaded_bytes", done},
                                          {"total_bytes", total},
                                          {"attempt", attempt},
                                          {"resumable", resumable},
                                          {"error", error}};
                });
            };
            const auto set_terminal_error = [impl, generation](const WeightStatus status, const std::string& error) {
                {
                    std::lock_guard lock(impl->mutex);
                    if (impl->weight.generation != generation || impl->shutting_down) {
                        return;
                    }
                    impl->weight.phase =
                        status == WeightStatus::Cancelled ? OperationPhase::Cancelled : OperationPhase::Failed;
                    impl->weight.status = status;
                    impl->weight.error = error;
                    impl->weight.retry_deadline = {};
                }
                impl->trace("weight.terminal_error", [&] {
                    return nlohmann::json{
                        {"generation", generation}, {"status", weight_status_name(status)}, {"error", error}};
                });
            };
            try {
                return acquire_canonical_weight(
                    preset_name, *cancelled,
                    [impl](const std::string_view event, auto&& fields_fn) {
                        impl->trace(event, std::forward<decltype(fields_fn)>(fields_fn));
                    },
                    update);
            } catch (const WeightTransferError& error) {
                set_terminal_error(weight_status_for_failure(error.kind()), error.what());
                throw;
            } catch (const std::filesystem::filesystem_error& error) {
                set_terminal_error(WeightStatus::FilesystemError, error.what());
                throw;
            } catch (const std::exception& error) {
                set_terminal_error(WeightStatus::CannotDownload, error.what());
                throw;
            }
        },
        [impl, generation, on_complete = std::move(on_complete)](CanonicalWeightResult result) mutable {
            {
                std::lock_guard lock(impl->mutex);
                if (impl->weight.generation != generation || impl->shutting_down) {
                    return;
                }
                impl->weight.phase = OperationPhase::Complete;
                impl->weight.status = WeightStatus::Ready;
                impl->weight.path = result.path;
                impl->weight.error.clear();
                impl->weight.downloaded_bytes = result.downloaded_bytes;
                impl->weight.total_bytes = result.total_bytes;
                impl->weight.attempt = result.attempt;
                impl->weight.retry_deadline = {};
                impl->weight.resumable = false;
            }
            if (on_complete) {
                on_complete(result.path);
            }
            impl->trace("weight.acquire_complete", [&] {
                return nlohmann::json{{"generation", generation},
                                      {"path", result.path},
                                      {"downloaded_bytes", result.downloaded_bytes},
                                      {"total_bytes", result.total_bytes},
                                      {"attempt", result.attempt}};
            });
            mmltk::logging::logger("gui")->debug("canonical weight acquisition complete: generation={} path={}",
                                                 generation, result.path);
        },
        [impl, generation, cancelled](const std::string& error) {
            const bool was_cancelled = cancelled->load(std::memory_order_relaxed);
            {
                std::lock_guard lock(impl->mutex);
                if (impl->weight.generation == generation && !impl->shutting_down) {
                    impl->weight.phase = was_cancelled ? OperationPhase::Cancelled : OperationPhase::Failed;
                    impl->weight.status = was_cancelled ? WeightStatus::Cancelled : impl->weight.status;
                    if (impl->weight.error.empty()) {
                        impl->weight.error = error;
                    }
                }
            }
            impl->trace(was_cancelled ? "weight.acquire_cancelled" : "weight.acquire_failed",
                        [&] { return nlohmann::json{{"generation", generation}, {"error", error}}; });
            if (!was_cancelled) {
                mmltk::logging::logger("gui")->warn("canonical weight acquisition failed: generation={} error={}",
                                                    generation, error);
            }
        });
}

void BrowserArtifactController::cancel_weight_acquisition() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->weight_cancel->store(true, std::memory_order_relaxed);
    }
    impl_->trace("weight.cancel_requested", [] { return nlohmann::json::object(); });
}

void BrowserArtifactController::shutdown() {
    std::lock_guard lock(impl_->mutex);
    impl_->shutting_down = true;
    impl_->pending_dataset_inspection.reset();
    impl_->dataset_cancel->store(true, std::memory_order_relaxed);
    impl_->weight_cancel->store(true, std::memory_order_relaxed);
}

mmltk::browser::ArtifactState BrowserArtifactController::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    mmltk::browser::ArtifactState snapshot;
    snapshot.dataset.phase = impl_->dataset.phase;
    snapshot.dataset.compile_phase = impl_->dataset.compile_phase;
    snapshot.dataset.generation = impl_->dataset.generation;
    snapshot.dataset.source_dir = impl_->dataset.source_dir;
    snapshot.dataset.output_dir = impl_->dataset.output_dir;
    snapshot.dataset.preset_name = impl_->dataset.preset_name;
    snapshot.dataset.active_split = impl_->dataset.active_split;
    snapshot.dataset.done = impl_->dataset.done;
    snapshot.dataset.total = impl_->dataset.total;
    snapshot.dataset.elapsed_ms = impl_->dataset.elapsed_ms;
    snapshot.dataset.remaining_ms = impl_->dataset.remaining_ms;
    snapshot.dataset.dropped_instances = impl_->dataset.dropped_instances;
    snapshot.dataset.eta_ready = impl_->dataset.eta_ready;
    if (impl_->dataset.compiling && impl_->dataset.compile_telemetry != nullptr) {
        const mmltk::CompileProgress progress = impl_->dataset.compile_telemetry->snapshot();
        snapshot.dataset.compile_phase = browser_compile_phase(progress.phase);
        snapshot.dataset.done = std::min(impl_->dataset.completed_steps + progress.done, impl_->dataset.total);
        snapshot.dataset.dropped_instances = impl_->dataset.completed_dropped_instances + progress.dropped_instances;
        if (impl_->dataset.eta != nullptr) {
            const CompileEtaSnapshot estimate = impl_->dataset.eta->sample(progress, impl_->dataset.future_images);
            snapshot.dataset.elapsed_ms = estimate.elapsed_ms;
            snapshot.dataset.remaining_ms = estimate.remaining_ms;
            snapshot.dataset.eta_ready = estimate.ready;
        }
    }
    if (impl_->dataset.compiling) {
        snapshot.dataset.elapsed_ms =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - impl_->dataset.started_at)
                                           .count());
    }
    snapshot.dataset.compatible = impl_->dataset.compatible;
    snapshot.dataset.compiling = impl_->dataset.compiling;
    snapshot.dataset.error = impl_->dataset.error;
    snapshot.dataset.splits.reserve(impl_->dataset.splits.size());
    for (const mmltk::CompiledDatasetInfo& split : impl_->dataset.splits) {
        snapshot.dataset.splits.push_back(mmltk::browser::ArtifactSplitState{
            split.path.string(), split.image_count, split.width, split.height, split.channels,
            static_cast<std::uint32_t>(split.class_names.size()), split.class_names});
    }
    snapshot.weight.phase = impl_->weight.phase;
    snapshot.weight.status = impl_->weight.status;
    snapshot.weight.generation = impl_->weight.generation;
    snapshot.weight.preset_name = impl_->weight.preset_name;
    snapshot.weight.path = impl_->weight.path;
    snapshot.weight.error = impl_->weight.error;
    snapshot.weight.downloaded_bytes = impl_->weight.downloaded_bytes;
    snapshot.weight.total_bytes = impl_->weight.total_bytes;
    snapshot.weight.attempt = impl_->weight.attempt;
    const auto now = std::chrono::steady_clock::now();
    if (impl_->weight.retry_deadline > now) {
        const std::uint64_t remaining_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(impl_->weight.retry_deadline - now).count());
        snapshot.weight.retry_after_ms = ((remaining_ms + 999U) / 1'000U) * 1'000U;
    }
    snapshot.weight.resumable = impl_->weight.resumable;
    return snapshot;
}

}  
