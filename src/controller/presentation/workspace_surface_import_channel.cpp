#include "src/controller/presentation/detail/workspace_surface_import_channel.h"
#include "src/controller/presentation/presentation_system.h"
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#include "src/common/io/noexcept_io.h"
#include "src/common/io/scoped_fd.h"
import mmltk.common.logging.mmltk_logging;
#include "src/controller/presentation/abi/workspace_frame_signal.h"
#include "src/controller/presentation/detail/workspace_surface_import_channel.h"
#include "src/controller/presentation/presentation_system.h"
namespace mmltk::controller::presentation {
using mmltk::common::io::ScopedFd;
contracts::DiagnosticWorkspace workspace_source_diagnostic(const workspace_surface_import::Record& record) noexcept {
    // CLEANUP-IGNORE: Workspace diagnostic projection has no shared fields or behavior with RF-DETR model preset construction.
    return {.workspace_source_high = record.id_high,
            .workspace_source_low = record.id_low,
            .workspace_allocation = record.allocation_identity,
            .workspace_arena_high = record.arena_high,
            .workspace_arena_low = record.arena_low,
            .workspace_bytes = record.size,
            .workspace_pitch = record.stride,
            .workspace_width = record.width,
            .workspace_height = record.height,
            .direct_sampling = record.direct_sampling != 0U};
}
namespace {
using workspace_surface_import::FailureCode;
using workspace_surface_import::Opcode;
using workspace_surface_import::Record;
// Allocation request eventfd, frame, and access descriptors fit the shell's
// frozen control-message budget.
constexpr std::size_t kControlBytes = 32U;
static_assert(CMSG_SPACE(sizeof(int) * workspace_surface_import::kAllocateDescriptorCount) <= kControlBytes);
// These records use the existing native stderr capture, as do the CUDA import
// records. Observe descriptors only while their existing owners retain them.
// No diagnostic descriptor, protocol field, or lifetime extension is needed.
void trace_memory_descriptor(const char* event, const Record& record, int peer, int descriptor) noexcept {
    const auto* trace = std::getenv("MMLTK_GUI_TRACE_FILE");
    if (!trace || !*trace) return;
    const int saved_errno = errno;
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    const int peer_status = ::getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials, &length);
    const bool peer_known = peer_status == 0 && length == sizeof(credentials);
    struct stat metadata{};
    const int stat_status = ::fstat(descriptor, &metadata);
    const int stat_errno = stat_status == 0 ? 0 : errno;
    char uuid[33]{};
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < sizeof(record.device_uuid); ++index) {
        uuid[index * 2] = hex[record.device_uuid[index] >> 4];
        uuid[index * 2 + 1] = hex[record.device_uuid[index] & 15];
    }
    char output[2048];
    const int bytes = std::snprintf(
        output, sizeof(output),
        "{\"event\":\"presentation.workspace.%s\",\"source\":\"%016llx%016llx\",\"surface\":\"%016llx%016llx\","
        "\"workspace_allocation\":%llu,\"native_process_id\":%ld,\"browser_process_id\":%ld,"
        "\"peer_credentials_known\":%s,\"channel_descriptor\":%d,\"workspace_descriptor\":%d,"
        "\"descriptor_count\":%u,\"memory_descriptor_index\":%zu,\"descriptor_transport\":\"SCM_RIGHTS\","
        "\"device_uuid\":\"%s\",\"device_incarnation\":%llu,\"memory_size\":%llu,\"row_pitch\":%llu,"
        "\"image_offset\":%llu,\"capacity_width\":%u,\"capacity_height\":%u,\"dedicated\":%u,"
        "\"record_bytes\":%zu,\"fd_stat_status\":%d,\"fd_stat_errno\":%d,"
        "\"fd_dev\":%llu,\"fd_ino\":%llu,\"fd_rdev\":%llu,\"fd_mode\":%u,\"fd_size\":%lld,"
        "\"fd_identity_scope\":\"metadata_only_not_gpu_allocation_identity\"}\n",
        event, static_cast<unsigned long long>(record.id_high), static_cast<unsigned long long>(record.id_low),
        static_cast<unsigned long long>(record.arena_high), static_cast<unsigned long long>(record.arena_low),
        static_cast<unsigned long long>(record.allocation_identity), static_cast<long>(::getpid()), peer_known ? static_cast<long>(credentials.pid) : 0L,
        peer_known ? "true" : "false", peer, descriptor, record.descriptors, workspace_surface_import::kReadyMemoryDescriptor, uuid,
        static_cast<unsigned long long>(record.device_incarnation), static_cast<unsigned long long>(record.size),
        static_cast<unsigned long long>(record.stride), static_cast<unsigned long long>(record.offset), record.width, record.height, record.dedicated,
        sizeof(record), stat_status, stat_errno, static_cast<unsigned long long>(metadata.st_dev), static_cast<unsigned long long>(metadata.st_ino),
        static_cast<unsigned long long>(metadata.st_rdev), static_cast<unsigned int>(metadata.st_mode), static_cast<long long>(metadata.st_size));
    if (bytes > 0 && static_cast<std::size_t>(bytes) < sizeof(output))
        mmltk::common::io::write_all_noexcept(STDERR_FILENO, {output, static_cast<std::size_t>(bytes)});
    errno = saved_errno;
}
[[nodiscard]] bool bind_listener(const int fd, const std::filesystem::path& path, std::string& error) {
    sockaddr_un address{};
    const std::string native = path.string();
    if (native.empty() || native.size() >= sizeof(address.sun_path)) {
        error = "workspace import socket path is out of range";
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, native.data(), native.size());
    ::unlink(native.c_str());
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        error = "workspace import socket bind failed";
        return false;
    }
    // The shell runs as the same user; nothing else may connect.
    if (::chmod(native.c_str(), S_IRUSR | S_IWUSR) != 0) {
        error = "workspace import socket permissions could not be restricted";
        return false;
    }
    if (::listen(fd, 1) != 0) {
        error = "workspace import socket listen failed";
        return false;
    }
    return true;
}
}  // namespace
WorkspaceSurfaceImportId WorkspaceSurfaceImportId::generate() {
    WorkspaceSurfaceImportId id;
    do {
        std::array<std::uint64_t, 2U> words{};
        std::size_t offset = 0U;
        while (offset < sizeof(words)) {
            const ssize_t count = ::getrandom(reinterpret_cast<std::byte*>(words.data()) + offset, sizeof(words) - offset, 0U);
            if (count < 0 && errno == EINTR) { continue; }
            if (count <= 0) { throw std::system_error(errno, std::generic_category(), "workspace import capability generation failed"); }
            offset += static_cast<std::size_t>(count);
        }
        id = {.high = words[0], .low = words[1]};
    } while (!id);
    return id;
}
WorkspaceSurfaceFrameSignal::~WorkspaceSurfaceFrameSignal() { reset(); }
WorkspaceSurfaceFrameSignal::WorkspaceSurfaceFrameSignal(WorkspaceSurfaceFrameSignal&& other) noexcept
    : descriptor_(std::move(other.descriptor_)), mapping_(std::exchange(other.mapping_, nullptr)) {}
WorkspaceSurfaceFrameSignal& WorkspaceSurfaceFrameSignal::operator=(WorkspaceSurfaceFrameSignal&& other) noexcept {
    if (this != &other) {
        reset();
        descriptor_ = std::move(other.descriptor_);
        mapping_ = std::exchange(other.mapping_, nullptr);
    }
    return *this;
}
WorkspaceSurfaceFrameSignal WorkspaceSurfaceFrameSignal::create() {
    WorkspaceSurfaceFrameSignal result;
    const int descriptor = ::memfd_create("mmltk-workspace-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (descriptor < 0) { throw std::system_error(errno, std::generic_category(), "workspace frame signal memfd creation failed"); }
    result.descriptor_ = ScopedFd{descriptor};
    constexpr std::size_t kSignalBytes = detail::kWorkspaceFrameMappingBytes;
    if (::ftruncate(descriptor, static_cast<off_t>(kSignalBytes)) != 0) {
        throw std::system_error(errno, std::generic_category(), "workspace frame signal sizing failed");
    }
    void* const mapping = ::mmap(nullptr, kSignalBytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED) { throw std::system_error(errno, std::generic_category(), "workspace frame signal mapping failed"); }
    result.mapping_ = static_cast<detail::WorkspaceFrameSignal*>(mapping);
    *result.mapping_ = {};
    if (::fcntl(descriptor, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
        throw std::system_error(errno, std::generic_category(), "workspace frame signal sealing failed");
    }
    return result;
}
int WorkspaceSurfaceFrameSignal::descriptor() const noexcept { return descriptor_.get(); }
detail::WorkspaceFrameSignal* WorkspaceSurfaceFrameSignal::mapping() const noexcept { return mapping_; }
void WorkspaceSurfaceFrameSignal::reset() noexcept {
    if (mapping_ != nullptr) {
        static_cast<void>(::munmap(mapping_, detail::kWorkspaceFrameMappingBytes));
        mapping_ = nullptr;
    }
    descriptor_.reset();
}
std::string WorkspaceSurfaceImportId::to_string() const {
    if (!*this) { return {}; }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << high << std::setw(16) << low;
    return stream.str();
}
struct WorkspaceSurfaceImportChannel::Impl {
    static constexpr std::size_t kLedgerCapacity = 256U;
    static constexpr std::size_t kPendingRecordCapacity = 32U;
    enum class PeerEpochState : std::uint8_t {
        Accepting,
        Connected,
        ClosedPendingObservation,
        ClosedObserved,
    };
    struct Admission {
        Record request{};
        std::uint64_t generation = 0U;
        std::uint64_t selection_generation = 0U;
        std::uint64_t frame_revision = 0U;
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        bool arena = false;
        bool binding_retired = false;
        std::optional<Record> acquired{};
        bool release_submitted = false;
        std::uint64_t last_transfer = 0U;
    };
    struct PendingRecord {
        Record record{};
        std::array<ScopedFd, workspace_surface_import::kAllocateDescriptorCount> descriptors{};
        std::size_t descriptor_count = 0U;
    };
    VisualDiagnosticSink diagnostics{};
    using AdmissionRecord = std::pair<WorkspaceSurfaceImportId, Admission>;
    using RetirementRecord = std::pair<WorkspaceSurfaceImportId, std::uint64_t>;
    // CLEANUP-IGNORE: This import-channel owner preallocates its own fixed ledgers and queues.
    explicit Impl(const VisualDiagnosticSink sink) : diagnostics(sink) {
        // CLEANUP-IGNORE: This channel reserves each owner-specific fixed-capacity record collection once.
        outcomes.reserve(kLedgerCapacity);
        retirements.reserve(kLedgerCapacity);
        renderer_presentations.reserve(kLedgerCapacity);
        pending.reserve(kPendingRecordCapacity);
        seen.reserve(kLedgerCapacity);
        admitted.reserve(kLedgerCapacity);
        committed.reserve(kLedgerCapacity);
        withdrawn.reserve(kLedgerCapacity);
        retired.reserve(kLedgerCapacity);
        pending_withdrawals.reserve(kLedgerCapacity);
        replied.reserve(kLedgerCapacity);
    }
    [[nodiscard]] static bool contains(const std::vector<WorkspaceSurfaceImportId>& values, const WorkspaceSurfaceImportId id) noexcept {
        return std::ranges::find(values, id) != values.end();
    }
    [[nodiscard]] auto find_admission(const WorkspaceSurfaceImportId id) noexcept { return std::ranges::find(admitted, id, &AdmissionRecord::first); }
    [[nodiscard]] std::uint64_t generation(const WorkspaceSurfaceImportId id) const noexcept {
        const auto admission = std::ranges::find(admitted, id, &AdmissionRecord::first);
        if (admission != admitted.end()) return admission->second.generation;
        const auto retirement = std::ranges::find(retired, id, &RetirementRecord::first);
        return retirement != retired.end() ? retirement->second : 0U;
    }
    void erase_id(std::vector<WorkspaceSurfaceImportId>& values, const WorkspaceSurfaceImportId id) { std::erase(values, id); }
    void erase_admission(const WorkspaceSurfaceImportId id) {
        std::erase_if(admitted, [id](const AdmissionRecord& record) { return record.first == id; });
    }
    std::filesystem::path path;
    ScopedFd listener;
    ScopedFd peer;
    pid_t expected_process_group = -1;
    std::vector<WorkspaceSurfaceImportOutcome> outcomes;
    std::array<Record, 2U * kLedgerCapacity> source_transitions{};
    std::size_t next_source_transition = 0U;
    std::size_t source_transition_count = 0U;
    void push_source_transition(const Record& record) {
        source_transitions[(next_source_transition + source_transition_count) % source_transitions.size()] = record;
        ++source_transition_count;
    }
    // CLEANUP-IGNORE: Transport queues track protocol custody, unlike the shell's independently typed system owners.
    std::optional<WorkspaceSurfaceImportId> capacity_wake;
    std::vector<WorkspaceSurfaceRetired> retirements;
    std::vector<WorkspaceRendererPresentation> renderer_presentations;
    std::vector<PendingRecord> pending;
    std::vector<WorkspaceSurfaceImportId> seen;
    std::vector<AdmissionRecord> admitted;
    std::vector<WorkspaceSurfaceImportId> committed;
    std::vector<WorkspaceSurfaceImportId> withdrawn;
    std::vector<RetirementRecord> retired;
    std::vector<RetirementRecord> pending_withdrawals;
    std::vector<WorkspaceSurfaceImportId> replied;
    std::optional<std::string> terminal;
    PeerEpochState peer_epoch = PeerEpochState::Accepting;
    void fail(std::string reason) {
        if (!terminal.has_value()) { terminal = std::move(reason); }
        // Transport failure closes this peer and discards queued socket work.
        // Presentation releases its physical imports after Firefox settles.
        peer.reset();
        pending.clear();
        peer_epoch = PeerEpochState::ClosedObserved;
    }
    void lose_peer() noexcept {
        peer.reset();
        peer_epoch = PeerEpochState::ClosedPendingObservation;
    }
    // Rejects a peer outside the expected process group, so a browser restart
    // cannot have its predecessor claim the channel.
    [[nodiscard]] bool peer_is_expected(const int fd) const {
        if (expected_process_group < 0) { return true; }
        ucred credentials{};
        socklen_t length = sizeof(credentials);
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) { return false; }
        return ::getpgid(credentials.pid) == expected_process_group;
    }
    // Sends one record together with exactly the descriptors its opcode
    // declares, in one SCM_RIGHTS array. The record's own count is what the
    // shell checks the ancillary data against.
    [[nodiscard]] bool admit(Record record, const std::uint64_t generation, const std::span<const int> descriptors,
                             const std::uint64_t selection_generation = 0U, const std::uint64_t frame_revision = 0U) {
        const WorkspaceSurfaceImportId id{record.id_high, record.id_low};
        record.descriptors = static_cast<std::uint32_t>(descriptors.size());
        if (!id || generation == 0U || !workspace_surface_import::valid(record)) return false;
        if (contains(seen, id) || seen.size() >= kLedgerCapacity || admitted.size() >= kLedgerCapacity) {
            fail("workspace capability admission is duplicate or exceeds capacity");
            return false;
        }
        seen.push_back(id);
        admitted.emplace_back(id, Admission{.request = record,
                                            .generation = generation,
                                            .selection_generation = selection_generation,
                                            .frame_revision = frame_revision,
                                            .width = record.width,
                                            .height = record.height,
                                            .arena = record.opcode == Opcode::Arena});
        EmitAdmission(record, VisualDiagnosticOperation::PresentationAdmissionEnqueued);
        if (send(record, descriptors)) return true;
        erase_id(seen, id);
        erase_admission(id);
        return false;
    }
    [[nodiscard]] bool send(const Record& record, const std::span<const int> descriptors) {
        if (peer.get() < 0 || descriptors.size() != workspace_surface_import::descriptor_count(record.opcode)) { return false; }
        if (pending.size() >= kPendingRecordCapacity) { return false; }
        PendingRecord outbound{.record = record, .descriptor_count = descriptors.size()};
        outbound.record.descriptors = static_cast<std::uint32_t>(descriptors.size());
        for (std::size_t index = 0U; index < descriptors.size(); ++index) {
            int duplicate = -1;
            do { duplicate = ::fcntl(descriptors[index], F_DUPFD_CLOEXEC, 0); } while (duplicate < 0 && errno == EINTR);
            if (duplicate < 0) {
                fail("workspace import descriptor duplication failed");
                return false;
            }
            outbound.descriptors[index] = ScopedFd{duplicate};
        }
        pending.push_back(std::move(outbound));
        flush();
        return !terminal.has_value();
    }
    void EmitAdmission(const Record& record, const VisualDiagnosticOperation operation) {
        diagnostics.Emit([&] {
            const WorkspaceSurfaceImportId id{record.id_high, record.id_low};
            const auto found = find_admission(id);
            const auto& admission = found->second;
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Presentation,
                                        .operation = admission.arena ? operation
                                                                     : (operation == VisualDiagnosticOperation::PresentationAdmissionEnqueued
                                                                            ? VisualDiagnosticOperation::PresentationSourceAdmissionEnqueued
                                                                            : VisualDiagnosticOperation::PresentationSourceAdmissionWritten),
                                        .generation = admission.generation,
                                        .context = {.capacity_width = admission.width,
                                                    .capacity_height = admission.height,
                                                    .surface_high = id.high,
                                                    .surface_low = id.low,
                                                    .selection_generation = admission.selection_generation,
                                                    .frame_revision = admission.frame_revision,
                                                    .condition = static_cast<std::uint64_t>(PresentationCapabilityCondition::Admitted),
                                                    .outcome = 1U,
                                                    .allocation = {.allocation_generation = admission.generation},
                                                    .workspace = admission.arena ? contracts::DiagnosticWorkspace{} : workspace_source_diagnostic(record)}};
        });
    }
    void flush() {
        while (peer.get() >= 0 && !pending.empty()) {
            PendingRecord& outbound = pending.front();
            iovec buffer{.iov_base = &outbound.record, .iov_len = sizeof(outbound.record)};
            std::array<int, workspace_surface_import::kAllocateDescriptorCount> descriptor_values{};
            for (std::size_t index = 0U; index < outbound.descriptor_count; ++index) { descriptor_values[index] = outbound.descriptors[index].get(); }
            std::array<std::byte, kControlBytes> control{};
            msghdr message{};
            message.msg_iov = &buffer;
            message.msg_iovlen = 1;
            if (outbound.descriptor_count != 0U) {
                const std::size_t payload_bytes = sizeof(int) * outbound.descriptor_count;
                message.msg_control = control.data();
                message.msg_controllen = CMSG_SPACE(payload_bytes);
                cmsghdr* const header = CMSG_FIRSTHDR(&message);
                if (header == nullptr) {
                    fail("workspace import control message could not be formed");
                    return;
                }
                header->cmsg_level = SOL_SOCKET;
                header->cmsg_type = SCM_RIGHTS;
                header->cmsg_len = CMSG_LEN(payload_bytes);
                std::memcpy(CMSG_DATA(header), descriptor_values.data(), payload_bytes);
            }
            const ssize_t sent = ::sendmsg(peer.get(), &message, MSG_NOSIGNAL);
            if (sent < 0 && errno == EINTR) { continue; }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { return; }
            if (sent < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN || errno == ESHUTDOWN || errno == ECONNABORTED)) {
                lose_peer();
                return;
            }
            if (sent != static_cast<ssize_t>(sizeof(outbound.record))) {
                fail("workspace import channel write failed");
                return;
            }
            if (outbound.record.opcode == Opcode::Allocate || outbound.record.opcode == Opcode::Arena) {
                const WorkspaceSurfaceImportId id{
                    .high = outbound.record.id_high,
                    .low = outbound.record.id_low,
                };
                if (!contains(committed, id)) committed.push_back(id);
                EmitAdmission(outbound.record, VisualDiagnosticOperation::PresentationAdmissionWritten);
            }
            pending.erase(pending.begin());
        }
    }
    void begin_withdrawal(const WorkspaceSurfaceImportId id, const std::uint64_t generation) {
        std::erase_if(outcomes, [id](const WorkspaceSurfaceImportOutcome& outcome) { return outcome.id == id; });
        if (capacity_wake == id) capacity_wake.reset();
        retired.emplace_back(id, generation);
        withdrawn.push_back(id);
    }
    void promote_withdrawals() {
        while (!pending_withdrawals.empty() && pending.size() < kPendingRecordCapacity && !terminal.has_value()) {
            const auto [id, generation] = pending_withdrawals.front();
            if (!send(Record{.opcode = Opcode::Drop, .id_high = id.high, .id_low = id.low}, {})) return;
            pending_withdrawals.erase(pending_withdrawals.begin());
            begin_withdrawal(id, generation);
        }
    }
    void drain() {
        while (peer.get() >= 0) {
            Record record{};
            iovec buffer{.iov_base = &record, .iov_len = sizeof(record)};
            std::array<std::byte, kControlBytes> control{};
            msghdr message{};
            message.msg_iov = &buffer;
            message.msg_iovlen = 1;
            message.msg_control = control.data();
            message.msg_controllen = control.size();
            const ssize_t read = ::recvmsg(peer.get(), &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            if (read < 0) {
                if (errno == EINTR) { continue; }
                if (errno == EAGAIN || errno == EWOULDBLOCK) { return; }
                if (errno == ECONNRESET || errno == ENOTCONN || errno == ESHUTDOWN || errno == ECONNABORTED) {
                    lose_peer();
                    return;
                }
                fail("workspace import channel read failed");
                return;
            }
            if (read == 0) {
                lose_peer();
                return;
            }
            std::array<ScopedFd, workspace_surface_import::kAllocateDescriptorCount> descriptors{};
            std::size_t descriptor_count = 0U;
            bool ancillary_valid = true;
            for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header)) {
                if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
                    ancillary_valid = false;
                    continue;
                }
                if (header->cmsg_len < CMSG_LEN(0U)) {
                    ancillary_valid = false;
                    continue;
                }
                const std::size_t bytes = header->cmsg_len - CMSG_LEN(0U);
                if (bytes % sizeof(int) != 0U) { ancillary_valid = false; }
                const std::size_t count = bytes / sizeof(int);
                const auto* values = reinterpret_cast<const int*>(CMSG_DATA(header));
                for (std::size_t index = 0U; index < count; ++index) {
                    if (descriptor_count >= descriptors.size()) {
                        ::close(values[index]);
                    } else {
                        descriptors[descriptor_count++] = ScopedFd{values[index]};
                    }
                }
            }
            if (read != static_cast<ssize_t>(sizeof(record)) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 || !ancillary_valid ||
                !workspace_surface_import::valid(record) || descriptor_count != record.descriptors) {
                fail("workspace import channel framing is invalid");
                return;
            }
            const WorkspaceSurfaceImportId id{.high = record.id_high, .low = record.id_low};
            const auto admission = find_admission(id);
            if (record.opcode == Opcode::BindingRetired) {
                if (admission == admitted.end() || !admission->second.arena || admission->second.binding_retired ||
                    source_transition_count == source_transitions.size()) {
                    fail("workspace binding retirement is unknown or exceeds capacity");
                    return;
                }
                admission->second.binding_retired = true;
                push_source_transition(record);
                continue;
            }
            if (record.opcode == Opcode::Acquired) {
                if (admission == admitted.end() || admission->second.arena || !contains(replied, id) || source_transition_count == source_transitions.size() ||
                    admission->second.acquired || record.offset <= admission->second.last_transfer) {
                    fail("workspace acquisition is unknown, duplicate, or exceeds capacity");
                    return;
                }
                admission->second.acquired = record;
                admission->second.last_transfer = record.offset;
                push_source_transition(record);
                continue;
            }
            if (record.opcode == Opcode::ReleaseSubmitted) {
                if (admission == admitted.end() || !admission->second.acquired || admission->second.release_submitted ||
                    source_transition_count == source_transitions.size()) {
                    fail("workspace release submission is unknown, premature, duplicate, or exceeds capacity");
                    return;
                }
                const auto& acquired = *admission->second.acquired;
                if (record.stride != acquired.stride || record.size != acquired.size || record.presentation_revision != acquired.presentation_revision ||
                    record.offset != acquired.offset) {
                    fail("workspace release submission does not match the exact acquisition");
                    return;
                }
                admission->second.release_submitted = true;
                push_source_transition(record);
                continue;
            }
            if (record.opcode == Opcode::Retired) {
                const auto retirement = std::ranges::find(retired, id, &RetirementRecord::first);
                const bool presentation_outstanding =
                    std::ranges::any_of(renderer_presentations, [id](const auto& presentation) { return presentation.resource == id; });
                if (admission == admitted.end() || retirement == retired.end() || !contains(withdrawn, id) || !contains(replied, id) ||
                    (admission != admitted.end() && admission->second.acquired) ||
                    (admission != admitted.end() && admission->second.arena && presentation_outstanding) || retirements.size() >= kLedgerCapacity) {
                    fail("workspace import channel received an invalid retirement terminal");
                    return;
                }
                retirements.push_back(WorkspaceSurfaceRetired{.id = id, .generation = retirement->second});
                erase_admission(id);
                erase_id(committed, id);
                erase_id(replied, id);
                erase_id(withdrawn, id);
                std::erase_if(retired, [id](const RetirementRecord& value) { return value.first == id; });
                continue;
            }
            if (record.opcode == Opcode::Available || record.opcode == Opcode::Presented || record.opcode == Opcode::Completed) {
                // Withdrawal retains the admission until Retired. Progress
                // already queued ahead of Drop still reconciles against the
                // exact sample ledger during that interval.
                if (admission == admitted.end() && !contains(replied, id) && contains(withdrawn, id)) { continue; }
                if (admission == admitted.end() || !admission->second.arena || !contains(replied, id)) {
                    fail("workspace import channel received renderer progress for an inactive capability");
                    return;
                }
                const WorkspaceRendererPresentation presentation{
                    .resource = id,
                    .generation = admission->second.generation,
                    .layer = static_cast<WorkspacePresentationLayer>((record.code - 1U) / 2U),
                    .slot = (record.code - 1U) % 2U,
                    .content = {.session = record.stride, .sequence = record.size},
                    .presentation_revision = record.presentation_revision,
                };
                if (record.opcode == Opcode::Presented) {
                    if (renderer_presentations.size() >= kLedgerCapacity ||
                        std::ranges::find(renderer_presentations, presentation) != renderer_presentations.end()) {
                        fail("workspace renderer presentation ledger rejected a duplicate or overflow");
                        return;
                    }
                    renderer_presentations.push_back(presentation);
                    continue;
                }
                if (record.opcode == Opcode::Completed) {
                    const auto active = std::ranges::find(renderer_presentations, presentation);
                    if (active == renderer_presentations.end()) {
                        fail("workspace renderer completed an unknown presentation");
                        return;
                    }
                    if (active != renderer_presentations.end() - 1) { *active = renderer_presentations.back(); }
                    renderer_presentations.pop_back();
                    if (!contains(withdrawn, id)) capacity_wake = id;
                    continue;
                }
                if (contains(withdrawn, id)) { continue; }
                capacity_wake = id;
                continue;
            }
            if (admission == admitted.end() || contains(replied, id)) {
                fail("workspace import channel received an unknown or duplicate outcome");
                return;
            }
            if ((record.opcode == Opcode::Ready && admission->second.arena) || (record.opcode == Opcode::ArenaReady && !admission->second.arena)) {
                fail("workspace import channel received an outcome for the wrong capability role");
                return;
            }
            if (record.opcode == Opcode::Ready) {
                auto expected = admission->second.request;
                expected.opcode = Opcode::Ready;
                expected.descriptors = workspace_surface_import::kReadyDescriptorCount;
                if (record != expected) {
                    fail("workspace allocation reply changed its immutable request layout");
                    return;
                }
            }
            if (record.opcode == Opcode::Failed && static_cast<FailureCode>(record.code) == FailureCode::Layout) {
                const std::uint64_t row_bytes = static_cast<std::uint64_t>(admission->second.width) * 4U;
                constexpr std::uint64_t kMaximumObjectBytes = static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
                const bool layout_is_bounded = admission->second.height != 0U && record.stride >= row_bytes &&
                                               record.stride <= std::numeric_limits<std::size_t>::max() / admission->second.height &&
                                               record.size >= record.stride * admission->second.height &&
                                               record.size <= std::numeric_limits<std::size_t>::max() && record.size <= kMaximumObjectBytes;
                if (!layout_is_bounded) {
                    fail("workspace import channel received an invalid or overflowing layout requirement");
                    return;
                }
            }
            replied.push_back(id);
            if (contains(withdrawn, id)) {
                // Ready may already be queued ahead of Drop. Validate it and
                // close anything it carried here; retain the tombstone so any
                // availability queued after that outcome but ahead of Drop is
                // also discarded. A withdrawn capability must never re-enter
                // the display system or reset recovery state.
                if (record.opcode == Opcode::Failed) {
                    if (retirements.size() >= kLedgerCapacity) {
                        fail("workspace import retirement outcomes exceeded their bounded ledger");
                        return;
                    }
                    retirements.push_back(WorkspaceSurfaceRetired{.id = id, .generation = admission->second.generation});
                    erase_admission(id);
                    erase_id(committed, id);
                    erase_id(replied, id);
                    erase_id(withdrawn, id);
                    std::erase_if(retired, [id](const RetirementRecord& value) { return value.first == id; });
                }
                continue;
            }
            switch (record.opcode) {
                case Opcode::ArenaReady:
                case Opcode::Ready:
                    if (outcomes.size() >= kLedgerCapacity) {
                        fail("workspace import outcomes exceeded their bounded ledger");
                        return;
                    }
                    outcomes.emplace_back();
                    outcomes.back().id = id;
                    outcomes.back().imported = true;
                    outcomes.back().layout = record;
                    if (record.opcode == Opcode::Ready)
                        trace_memory_descriptor("descriptor_received", record, peer.get(), descriptors[workspace_surface_import::kReadyMemoryDescriptor].get());
                    outcomes.back().memory_descriptor = std::move(descriptors[workspace_surface_import::kReadyMemoryDescriptor]);
                    outcomes.back().timeline_descriptor = std::move(descriptors[workspace_surface_import::kReadyTimelineDescriptor]);
                    break;
                case Opcode::Failed:
                    if (outcomes.size() >= kLedgerCapacity) {
                        fail("workspace import outcomes exceeded their bounded ledger");
                        return;
                    }
                    outcomes.emplace_back();
                    outcomes.back().id = id;
                    outcomes.back().failure = static_cast<FailureCode>(record.code);
                    outcomes.back().required_stride = record.stride;
                    outcomes.back().required_size = record.size;
                    break;
                case Opcode::Arena:
                case Opcode::Acquired:
                case Opcode::ReleaseSubmitted:
                case Opcode::BindingRetired:
                case Opcode::ReadSettled:
                case Opcode::Allocate:
                case Opcode::Drop:
                case Opcode::Available:
                case Opcode::Presented:
                case Opcode::Completed:
                case Opcode::Retired: fail("workspace import channel received an opcode outside the import outcome boundary"); return;
            }
        }
    }
};
WorkspaceSurfaceImportChannel::WorkspaceSurfaceImportChannel(const std::filesystem::path& socket_path, const VisualDiagnosticSink diagnostics)
    : impl_(std::make_unique<Impl>(diagnostics)) {
    impl_->path = socket_path;
    impl_->listener = ScopedFd{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (impl_->listener.get() < 0) { throw std::system_error(errno, std::generic_category(), "workspace import socket could not be created"); }
    std::string error;
    if (!bind_listener(impl_->listener.get(), socket_path, error)) { throw std::system_error(errno, std::generic_category(), error); }
}
WorkspaceSurfaceImportChannel::~WorkspaceSurfaceImportChannel() {
    if (impl_ != nullptr && !impl_->path.empty()) { ::unlink(impl_->path.c_str()); }
}
const std::filesystem::path& WorkspaceSurfaceImportChannel::socket_path() const noexcept { return impl_->path; }
int WorkspaceSurfaceImportChannel::poll_fd() const noexcept { return impl_->peer.get() >= 0 ? impl_->peer.get() : impl_->listener.get(); }
bool WorkspaceSurfaceImportChannel::connected() const noexcept {
    return impl_->peer_epoch == Impl::PeerEpochState::Connected && impl_->peer.get() >= 0 && !impl_->terminal.has_value();
}
bool WorkspaceSurfaceImportChannel::wants_write() const noexcept { return connected() && (!impl_->pending.empty() || !impl_->pending_withdrawals.empty()); }
bool WorkspaceSurfaceImportChannel::claimable(const WorkspaceSurfaceImportId id) const noexcept {
    return connected() && Impl::contains(impl_->committed, id) && !Impl::contains(impl_->withdrawn, id) &&
           std::ranges::find(impl_->pending_withdrawals, id, &Impl::RetirementRecord::first) == impl_->pending_withdrawals.end();
}
bool WorkspaceSurfaceImportChannel::consume_peer_loss() noexcept {
    if (impl_->peer_epoch != Impl::PeerEpochState::ClosedPendingObservation) return false;
    impl_->peer_epoch = Impl::PeerEpochState::ClosedObserved;
    return true;
}
void WorkspaceSurfaceImportChannel::set_expected_process_group(const pid_t process_group) {
    // Naming a new process group is the host saying a replacement browser is
    // coming. Protocol terminals remain irreversible.
    impl_->expected_process_group = process_group;
}
void WorkspaceSurfaceImportChannel::reset_peer() noexcept {
    impl_->peer.reset();
    impl_->peer_epoch = Impl::PeerEpochState::Accepting;
    impl_->outcomes.clear();
    impl_->next_source_transition = 0U;
    impl_->source_transition_count = 0U;
    impl_->capacity_wake.reset();
    impl_->retirements.clear();
    impl_->renderer_presentations.clear();
    impl_->pending.clear();
    impl_->seen.clear();
    impl_->admitted.clear();
    impl_->committed.clear();
    impl_->withdrawn.clear();
    impl_->retired.clear();
    impl_->pending_withdrawals.clear();
    impl_->replied.clear();
}
bool WorkspaceSurfaceImportChannel::admit_arena(const WorkspaceSurfaceImportId id, const std::uint64_t generation, const std::uint32_t width,
                                                const std::uint32_t height, const std::uint64_t selection_generation, const std::uint64_t frame_revision) {
    return connected() && impl_->admit(Record{.opcode = Opcode::Arena, .id_high = id.high, .id_low = id.low, .width = width, .height = height}, generation, {},
                                       selection_generation, frame_revision);
}
bool WorkspaceSurfaceImportChannel::admit_source(Record record, const std::uint64_t generation, const int frame_edge, const int frame_signal,
                                                 const int access_signal, const std::uint64_t selection_generation, const std::uint64_t frame_revision) {
    if (!connected() || frame_edge < 0 || frame_signal < 0 || access_signal < 0) return false;
    record.opcode = Opcode::Allocate;
    const std::array descriptors{frame_edge, frame_signal, access_signal};
    return impl_->admit(record, generation, descriptors, selection_generation, frame_revision);
}
bool WorkspaceSurfaceImportChannel::read_settled(const WorkspaceSurfaceImportId id, const WorkspaceContentIdentity content, const std::uint64_t revision,
                                                 const std::uint64_t transfer_sequence) {
    if (!connected() || !content.valid() || revision == 0U || transfer_sequence == 0U) return false;
    const auto admission = impl_->find_admission(id);
    if (admission == impl_->admitted.end() || admission->second.arena || !Impl::contains(impl_->replied, id) || !admission->second.acquired ||
        !admission->second.release_submitted)
        return false;
    const auto& acquired = *admission->second.acquired;
    if (acquired.stride != content.session || acquired.size != content.sequence || acquired.presentation_revision != revision ||
        acquired.offset != transfer_sequence)
        return false;
    for (std::size_t index = 0U; index != impl_->source_transition_count; ++index) {
        const auto& pending = impl_->source_transitions[(impl_->next_source_transition + index) % impl_->source_transitions.size()];
        if (pending.id_high == id.high && pending.id_low == id.low) return false;
    }
    if (!impl_->send(Record{.opcode = Opcode::ReadSettled,
                            .id_high = id.high,
                            .id_low = id.low,
                            .stride = content.session,
                            .size = content.sequence,
                            .presentation_revision = revision,
                            .offset = transfer_sequence},
                     {}))
        return false;
    admission->second.acquired.reset();
    admission->second.release_submitted = false;
    return true;
}
WorkspaceSurfaceWithdrawal WorkspaceSurfaceImportChannel::withdraw(const WorkspaceSurfaceImportId id) {
    if (!connected() || !id) { return {.progress = WorkspaceSurfaceWithdrawalProgress::Invalid, .id = id}; }
    const auto admission = impl_->find_admission(id);
    if (admission == impl_->admitted.end()) {
        return {.progress = Impl::contains(impl_->seen, id) ? WorkspaceSurfaceWithdrawalProgress::Retired : WorkspaceSurfaceWithdrawalProgress::Invalid,
                .id = id};
    }
    if (Impl::contains(impl_->withdrawn, id)) {
        return {.progress = WorkspaceSurfaceWithdrawalProgress::Pending, .id = id, .generation = admission->second.generation};
    }
    const auto pending = std::ranges::find(impl_->pending_withdrawals, id, &Impl::RetirementRecord::first);
    if (pending != impl_->pending_withdrawals.end()) {
        return {.progress = WorkspaceSurfaceWithdrawalProgress::Pending, .id = id, .generation = pending->second};
    }
    if (impl_->withdrawn.size() + impl_->pending_withdrawals.size() >= Impl::kLedgerCapacity) {
        return {.progress = WorkspaceSurfaceWithdrawalProgress::Capacity, .id = id, .generation = admission->second.generation};
    }
    if (!impl_->send(Record{.opcode = Opcode::Drop, .id_high = id.high, .id_low = id.low}, {})) {
        if (impl_->terminal.has_value()) {
            return {.progress = WorkspaceSurfaceWithdrawalProgress::Invalid, .id = id, .generation = admission->second.generation};
        }
        impl_->pending_withdrawals.emplace_back(id, admission->second.generation);
        return {.progress = WorkspaceSurfaceWithdrawalProgress::Retained, .id = id, .generation = admission->second.generation};
    }
    impl_->begin_withdrawal(id, admission->second.generation);
    return {.progress = WorkspaceSurfaceWithdrawalProgress::Submitted, .id = id, .generation = admission->second.generation};
}
void WorkspaceSurfaceImportChannel::pump() {
    if (impl_->terminal.has_value()) { return; }
    if (impl_->peer_epoch == Impl::PeerEpochState::ClosedPendingObservation || impl_->peer_epoch == Impl::PeerEpochState::ClosedObserved) return;
    if (impl_->peer_epoch == Impl::PeerEpochState::Accepting) {
        int accepted = -1;
        do { accepted = ::accept4(impl_->listener.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK); } while (accepted < 0 && errno == EINTR);
        if (accepted < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) { impl_->fail("workspace import channel accept failed"); }
            return;
        }
        ScopedFd candidate{accepted};
        if (!impl_->peer_is_expected(candidate.get())) {
            mmltk::common::logging::warn([](auto& logger) { logger.warn("workspace import connection refused: unexpected peer process group"); });
            return;
        }
        impl_->peer = std::move(candidate);
        impl_->peer_epoch = Impl::PeerEpochState::Connected;
    }
    // Observe peer lifetime before touching retained outbound work.
    impl_->drain();
    if (impl_->peer_epoch != Impl::PeerEpochState::Connected) return;
    impl_->flush();
    if (impl_->peer_epoch != Impl::PeerEpochState::Connected) return;
    impl_->promote_withdrawals();
    if (impl_->peer_epoch != Impl::PeerEpochState::Connected) return;
    impl_->drain();
}
std::optional<WorkspaceSurfaceImportOutcome> WorkspaceSurfaceImportChannel::take_outcome() {
    if (impl_->outcomes.empty()) { return std::nullopt; }
    WorkspaceSurfaceImportOutcome outcome = std::move(impl_->outcomes.front());
    impl_->outcomes.erase(impl_->outcomes.begin());
    return outcome;
}
std::optional<Record> WorkspaceSurfaceImportChannel::take_source_transition() {
    if (impl_->source_transition_count == 0U) return std::nullopt;
    auto record = impl_->source_transitions[impl_->next_source_transition];
    impl_->next_source_transition = (impl_->next_source_transition + 1U) % impl_->source_transitions.size();
    --impl_->source_transition_count;
    return record;
}
std::optional<WorkspaceSurfaceImportId> WorkspaceSurfaceImportChannel::take_capacity_wake() { return std::exchange(impl_->capacity_wake, std::nullopt); }
std::optional<WorkspaceSurfaceRetired> WorkspaceSurfaceImportChannel::take_retirement() {
    if (impl_->retirements.empty()) return std::nullopt;
    WorkspaceSurfaceRetired retired = impl_->retirements.front();
    impl_->retirements.erase(impl_->retirements.begin());
    impl_->erase_id(impl_->seen, retired.id);
    return retired;
}
std::optional<std::string> WorkspaceSurfaceImportChannel::terminal_error() const { return impl_->terminal; }
}  // namespace mmltk::controller::presentation
