#pragma once

#include <sys/types.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "src/controller/presentation/detail/workspace_surface_import_abi.h"
#include "src/common/io/scoped_fd.h"
#include "src/controller/presentation/workspace_presentation_types.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/presentation/visual_diagnostics.h"

namespace mmltk::controller::presentation {

namespace detail {
struct WorkspaceFrameSignal;
}
namespace workspace_surface_import = detail::workspace_surface_import;

// The one identity an admitted allocation carries across the import boundary.
// This random capability is deliberately unrelated to application generation,
// lease, or revision identities. The shell treats it only as an admission key.
struct WorkspaceSurfaceImportId {
    std::uint64_t high = 0U;
    std::uint64_t low = 0U;

    [[nodiscard]] explicit inline operator bool() const noexcept { return high != 0U || low != 0U; }

    [[nodiscard]] static WorkspaceSurfaceImportId generate();
    [[nodiscard]] std::string to_string() const;
    auto operator<=>(const WorkspaceSurfaceImportId&) const = default;
};

// What the shell reported about one admitted allocation.
struct WorkspaceSurfaceImportOutcome {
    WorkspaceSurfaceImportOutcome() = default;
    WorkspaceSurfaceImportOutcome(const WorkspaceSurfaceImportOutcome&) = delete;
    WorkspaceSurfaceImportOutcome& operator=(const WorkspaceSurfaceImportOutcome&) = delete;
    WorkspaceSurfaceImportOutcome(WorkspaceSurfaceImportOutcome&&) noexcept = default;
    WorkspaceSurfaceImportOutcome& operator=(WorkspaceSurfaceImportOutcome&&) noexcept = default;

    WorkspaceSurfaceImportId id{};
    bool imported = false;
    workspace_surface_import::FailureCode failure = workspace_surface_import::FailureCode::Import;
    // Meaningful only for FailureCode::Layout: the row pitch and size the
    // browser's device requires for the same extent. The importing driver owns
    // that number, so this is the one thing the shell ever tells the host that
    // the host could not have computed for itself.
    std::uint64_t required_stride = 0U;
    std::uint64_t required_size = 0U;
    // Present exactly for a successful Ready response. CUDA consumes ownership
    // when the timeline semaphore import succeeds.
    mmltk::common::io::ScopedFd timeline_descriptor;
};

struct WorkspaceRendererSampleIdentity {
    WorkspaceContentIdentity content{};
    std::uint64_t presentation_revision = 0U;

    [[nodiscard]] inline bool valid() const noexcept { return content.valid() && presentation_revision != 0U; }
    auto operator<=>(const WorkspaceRendererSampleIdentity&) const = default;
};

struct WorkspaceSurfaceRetired {
    WorkspaceSurfaceImportId id{};
    std::uint64_t generation = 0U;
};

enum class WorkspaceSurfaceWithdrawalProgress : std::uint8_t {
    Submitted,
    Retained,
    Pending,
    Retired,
    Capacity,
    Invalid,
};

struct WorkspaceSurfaceWithdrawal {
    WorkspaceSurfaceWithdrawalProgress progress = WorkspaceSurfaceWithdrawalProgress::Invalid;
    WorkspaceSurfaceImportId id{};
    std::uint64_t generation = 0U;
};

// One exact page-visible sample owned by the renderer until its final GPU use
// completes. The native import identity maps the browser sample back to the
// application backbuffer generation; presentation revision and content form
// the exact renderer sample identity.
struct WorkspaceRendererPresentation {
    WorkspaceSurfaceImportId resource{};
    std::uint64_t generation = 0U;
    WorkspacePresentationLayer layer = WorkspacePresentationLayer::Invalid;
    std::uint32_t slot = UINT32_MAX;
    WorkspaceContentIdentity content{};
    std::uint64_t presentation_revision = 0U;

    [[nodiscard]] inline WorkspaceRendererSampleIdentity sample() const noexcept {
        return {.content = content, .presentation_revision = presentation_revision};
    }

    auto operator<=>(const WorkspaceRendererPresentation&) const = default;
};

// Host-owned mapping for the generic identity paired with the native eventfd.
// The shell maps its transferred descriptor read-only; the presentation owner
// writes the fixed storage before raising an edge, so backbuffer replacement
// and completed-copy redraws use the native boundary without per-frame
// WebSocket traffic.
class WorkspaceSurfaceFrameSignal {
   public:
    WorkspaceSurfaceFrameSignal() noexcept = default;
    ~WorkspaceSurfaceFrameSignal();
    WorkspaceSurfaceFrameSignal(const WorkspaceSurfaceFrameSignal&) = delete;
    WorkspaceSurfaceFrameSignal& operator=(const WorkspaceSurfaceFrameSignal&) = delete;
    WorkspaceSurfaceFrameSignal(WorkspaceSurfaceFrameSignal&& other) noexcept;
    WorkspaceSurfaceFrameSignal& operator=(WorkspaceSurfaceFrameSignal&& other) noexcept;

    [[nodiscard]] static WorkspaceSurfaceFrameSignal create();
    [[nodiscard]] int descriptor() const noexcept;
    [[nodiscard]] detail::WorkspaceFrameSignal* mapping() const noexcept;
    void reset() noexcept;

   private:
    mmltk::common::io::ScopedFd descriptor_{};
    detail::WorkspaceFrameSignal* mapping_ = nullptr;
};

// The host end of the import channel: an AF_UNIX socket that carries the dmabuf
// descriptor, frame edge, and shared frame identity over SCM_RIGHTS.
//
// The host drives. It admits an allocation under an identifier and hands over
// the descriptor backing it; the shell answers imported or failed; the host
// later withdraws the identifier. The shell treats the surface capability,
// typed layer, and logical content identity as opaque transport data;
// application interpretation and retirement stay in Presentation.
//
// This owns only the socket and its bounded ABI-correlation ledger.
class WorkspaceSurfaceImportChannel final {
   public:
    explicit WorkspaceSurfaceImportChannel(const std::filesystem::path& socket_path, VisualDiagnosticSink diagnostics = {});
    ~WorkspaceSurfaceImportChannel();

    WorkspaceSurfaceImportChannel(const WorkspaceSurfaceImportChannel&) = delete;
    WorkspaceSurfaceImportChannel& operator=(const WorkspaceSurfaceImportChannel&) = delete;

    [[nodiscard]] const std::filesystem::path& socket_path() const noexcept;
    // Readable whenever Firefox has connected or replied.
    [[nodiscard]] int poll_fd() const noexcept;
    // True once the shell has connected and the channel is usable.
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool wants_write() const noexcept;
    [[nodiscard]] bool claimable(WorkspaceSurfaceImportId) const noexcept;
    [[nodiscard]] bool consume_peer_loss() noexcept;

    // Restricts the accepted peer to one process group, so a stale or foreign
    // process cannot claim the channel across a browser restart.
    void set_expected_process_group(pid_t process_group);
    void reset_peer() noexcept;

    // Admits `id` and transfers `descriptor` to the shell. Ownership of the
    // descriptor moves; the kernel keeps the allocation alive for as long as
    // the shell's import holds it, so the caller may release its own mapping
    // independently.
    //
    // `frame_edge` is the eventfd the shell watches for the one signal the host
    // raises per completed backbuffer copy. It is borrowed rather than moved,
    // because the host keeps the signalling end. The channel takes a private
    // duplicate while a nonblocking record is queued; SCM_RIGHTS then installs
    // the shell's descriptor for the same open file. The eventfd is the only
    // per-frame wakeup that crosses the boundary, and the host never reads or
    // waits on it. `frame_signal` is the read-only shared snapshot identifying
    // the exact timeline copy authorized by that edge.
    [[nodiscard]] bool admit(WorkspaceSurfaceImportId id, std::uint64_t generation, std::uint32_t width, std::uint32_t height,
                             std::uint64_t stride, std::uint64_t size, mmltk::common::io::ScopedFd descriptor, int frame_edge,
                             int frame_signal, std::uint64_t selection_generation = 0U, std::uint64_t frame_revision = 0U);
    // Transfers one exact withdrawal ticket. Capacity retains the ticket in the
    // channel, and a claimed capability remains admitted through Retired.
    [[nodiscard]] WorkspaceSurfaceWithdrawal withdraw(WorkspaceSurfaceImportId id);

    // Accepts a connection and drains replies when the writer observes
    // readiness.
    void pump();
    [[nodiscard]] std::optional<WorkspaceSurfaceImportOutcome> take_outcome();
    [[nodiscard]] std::optional<WorkspaceSurfaceImportId> take_capacity_wake();
    [[nodiscard]] std::optional<WorkspaceSurfaceRetired> take_retirement();
    // Set once the active protocol session fails.
    [[nodiscard]] std::optional<std::string> terminal_error() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::controller::presentation
