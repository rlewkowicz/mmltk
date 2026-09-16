#pragma once
#include <sys/types.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <inplace_vector>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/contract/training_metrics.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/io/scoped_fd.h"
#include "src/controller/contracts/compute.h"
#include "src/frameworks/process/subprocess_utils.h"
namespace mmltk::controller::services {
inline constexpr std::size_t kTrainProcessReadBudget = std::size_t{64U} * 1024U;
struct TrainProcessProgress final {
    mmltk::controller::contracts::ComputeProgress progress;
    std::filesystem::path checkpoint_path{};
    std::optional<mmltk::backend::models::rfdetr::TrainingRecord> metrics{};
    mmltk::backend::models::rfdetr::TrainingPersistence persistence{};
};
// This is a service-process exit observation, not a second public compute
// operation vocabulary.  LocalTrain maps it into mmltk::controller::contracts::ComputeTerminal.
enum class TrainProcessExitOutcome : std::uint8_t { Succeeded, Failed, Cancelled };
struct TrainProcessExit final {
    TrainProcessExitOutcome outcome = TrainProcessExitOutcome::Failed;
    std::int32_t wait_status = 0;
    bool setup_failure = false;
    std::optional<TrainProcessProgress> final_progress;
    std::string error;
};
struct TrainProcessOptions final {
    std::chrono::milliseconds escalation_delay{std::chrono::seconds{5}};
};
// A stop capability is minted with its token for one run.  The source may be
// retained by a system; the token is consumed exclusively by the process work.
struct TrainProcessStopTag;
using TrainProcessStopSource = mmltk::common::concurrency::EventCancellationSource<TrainProcessStopTag, false>;
using TrainProcessStopToken = mmltk::common::concurrency::EventCancellationToken<TrainProcessStopTag>;
struct TrainProcessRunResult final {
    TrainProcessExit terminal;
    std::string output;
};
struct TrainProcessProgressObserver final {
    void* context = nullptr;
    void (*report)(void*, const TrainProcessProgress&) noexcept = nullptr;
    void operator()(const TrainProcessProgress& progress) const noexcept {
        if (report != nullptr) report(context, progress);
    }
};
// Owns one local train process group, all Linux readiness sources, and exact
// terminal reaping. Callers only consume owner-neutral observations.
class TrainProcessClient final {
   public:
    TrainProcessClient() noexcept;
    ~TrainProcessClient() noexcept;
    TrainProcessClient(const TrainProcessClient&) = delete;
    TrainProcessClient& operator=(const TrainProcessClient&) = delete;
    TrainProcessClient(TrainProcessClient&&) noexcept;
    TrainProcessClient& operator=(TrainProcessClient&&) noexcept = delete;
    [[nodiscard]] static TrainProcessClient launch(const mmltk::backend::models::rfdetr::TrainRequest& request, const std::filesystem::path& cli_path,
                                                   std::string_view fallback_preset_name = {}, TrainProcessOptions options = {});
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] std::int32_t process_group_id() const noexcept;
    [[nodiscard]] int stdout_fd() const noexcept;
    [[nodiscard]] int pid_fd() const noexcept;
    [[nodiscard]] int setup_error_fd() const noexcept;
    [[nodiscard]] int progress_fd() const noexcept;
    [[nodiscard]] int control_fd() const noexcept;
    [[nodiscard]] int escalation_fd() const noexcept;
    [[nodiscard]] bool request_stop(bool force) noexcept;
    [[nodiscard]] bool consume_stop_request();
    [[nodiscard]] bool consume_escalation();
    std::size_t consume_output(std::string& output, std::size_t budget = kTrainProcessReadBudget,
                               std::size_t retention_limit = std::numeric_limits<std::size_t>::max());
    [[nodiscard]] std::optional<TrainProcessProgress> consume_progress();
    [[nodiscard]] std::optional<TrainProcessExit> consume_exit(std::string* retained_output = nullptr);
    void force_reap() noexcept;
    [[nodiscard]] TrainProcessRunResult Run(TrainProcessStopToken token, TrainProcessProgressObserver progress = {});

   private:
    struct State final {
        struct GroupMember final {
            pid_t pid = -1;
            mmltk::common::io::ScopedFd pidfd;
        };
        static constexpr std::size_t kGroupMemberCapacity = 16U;
        [[nodiscard]] bool tracks(pid_t candidate) const noexcept;
        void refresh_group_members();
        [[nodiscard]] bool consume_lifecycle();
        pid_t pid = -1;
        pid_t group = -1;
        mmltk::common::io::ScopedFd pidfd;
        mmltk::common::io::ScopedFd stdout_fd;
        mmltk::common::io::ScopedFd setup_fd;
        mmltk::common::io::ScopedFd progress_fd;
        mmltk::common::io::ScopedFd control_fd;
        mmltk::common::io::ScopedFd escalation_fd;
        mmltk::common::io::ScopedFd lifecycle_fd;
        std::inplace_vector<GroupMember, kGroupMemberCapacity> group_members;
        std::filesystem::path output_directory;
        int progress_watch = -1;
        std::uint64_t progress_sequence = 0U;
        std::size_t persistence_marker = 0;
        bool persistence_failed = false;
        bool status_dirty = false;
        int wait_status = 0;
        bool stop_requested = false;
        bool force_requested = false;
        bool reaped = false;
        bool group_tracking = false;
        bool group_quiesced = false;
        bool term_sent = false;
        bool kill_sent = false;
        bool terminal_consumed = false;
        std::chrono::milliseconds escalation_delay{std::chrono::seconds{5}};
    };
    explicit TrainProcessClient(State state) noexcept;
    [[nodiscard]] std::optional<TrainProcessProgress> read_progress();
    std::optional<State> state_;
};
}  // namespace mmltk::controller::services
