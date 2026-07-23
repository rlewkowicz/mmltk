#include "gui/workspace_surface_broker.h"

#include "gui/workspace_surface_protocol.h"
#include "mmltk/live/workspace_surface_pool.h"
#include "mmltk/live/workspace_trace.h"
#include "mmltk_logging.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace mmltk::gui {

namespace {

using workspace_surface_protocol::Message;
using workspace_surface_protocol::MessageType;

[[nodiscard]] bool constant_time_equal(const std::string_view left, const std::string_view right) noexcept {
    std::size_t difference = left.size() ^ right.size();
    const std::size_t count = std::min(left.size(), right.size());
    for (std::size_t index = 0U; index < count; ++index) {
        difference |= static_cast<unsigned char>(left[index]) ^ static_cast<unsigned char>(right[index]);
    }
    return difference == 0U;
}

[[nodiscard]] bool would_block() noexcept {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

struct SlotKey {
    std::uint64_t generation = 0U;
    std::uint32_t slot = 0U;

    bool operator==(const SlotKey&) const = default;
};

struct SlotKeyHash {
    [[nodiscard]] std::size_t operator()(const SlotKey& key) const noexcept {
        return std::hash<std::uint64_t>{}(key.generation) ^ (std::hash<std::uint32_t>{}(key.slot) << 1U);
    }
};

struct InFlightReservation {
    std::shared_ptr<mmltk::live::WorkspaceSurfacePool> pool;
    std::uint64_t revision = 0U;
};

}  

struct WorkspaceSurfaceBroker::Impl {
    explicit Impl(std::filesystem::path next_socket_path, std::string next_session_token)
        : socket_path(std::move(next_socket_path)), session_token(std::move(next_session_token)) {
        if (session_token.size() != workspace_surface_protocol::kTokenBytes || socket_path.empty()) {
            throw std::runtime_error("workspace surface broker requires a 64-byte token and socket path");
        }
        const std::string path = socket_path.string();
        if (path.size() >= sizeof(sockaddr_un::sun_path)) {
            throw std::runtime_error("workspace surface broker socket path is too long");
        }
        struct stat parent_stat {};
        const std::filesystem::path parent = socket_path.parent_path();
        if (::lstat(parent.c_str(), &parent_stat) != 0 || !S_ISDIR(parent_stat.st_mode) ||
            parent_stat.st_uid != ::getuid() || (parent_stat.st_mode & 0777U) != 0700U) {
            throw std::runtime_error("workspace surface broker directory must be an owned 0700 directory");
        }

        listen_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_fd < 0) {
            throw std::system_error(errno, std::generic_category(), "workspace surface broker socket creation failed");
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
        if (::bind(listen_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0 || ::listen(listen_fd, 1) != 0) {
            const int saved_errno = errno;
            ::close(listen_fd);
            listen_fd = -1;
            (void)::unlink(path.c_str());
            throw std::system_error(saved_errno, std::generic_category(),
                                    "workspace surface broker socket setup failed");
        }
        mmltk::live::trace_workspace("host", "workspace.broker_listening",
                                     [&] { return nlohmann::json{{"socket", path}}; });
    }

    ~Impl() {
        release_all();
        close_client();
        if (listen_fd >= 0) {
            ::close(listen_fd);
        }
        (void)::unlink(socket_path.c_str());
    }

    void close_client() noexcept {
        const bool had_client = client_fd >= 0;
        if (client_fd >= 0) {
            ::close(client_fd);
            client_fd = -1;
        }
        if (had_client) {
            mmltk::live::trace_workspace("host", "workspace.broker_disconnected", [&] {
                return nlohmann::json{{"configured_generation", configured_generation},
                                      {"in_flight", in_flight.size()}};
            });
            release_all();
            if (pool != nullptr) {
                pool->reset_timeline_semaphores();
            }
            for (const auto& [generation, retired_pool] : retiring_pools) {
                (void)generation;
                if (retired_pool != nullptr && retired_pool != pool) {
                    retired_pool->reset_timeline_semaphores();
                }
            }
            retiring_pools.clear();
            offered_generations.clear();
            pending_retirements.clear();
            pending_slot_reservations.clear();
        }
        authenticated = false;
        quarantined = false;
        adapter_ready = false;
        adapter_modifiers.clear();
        mmltk::live::clear_workspace_vulkan_adapter();
        configured_generation = 0U;
        next_config_slot = 0U;
        last_present_revision = 0U;
        sync_ready_generation = 0U;
    }

    void accept_client() {
        if (client_fd >= 0) {
            return;
        }
        const int accepted = ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (accepted < 0) {
            if (!would_block() && errno != EINTR) {
                throw std::system_error(errno, std::generic_category(), "workspace surface broker accept failed");
            }
            return;
        }
        ucred credentials{};
        socklen_t credentials_size = sizeof(credentials);
        if (::getsockopt(accepted, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size) != 0 ||
            credentials_size != sizeof(credentials) || credentials.uid != ::getuid() || expected_process_group <= 0 ||
            ::getpgid(credentials.pid) != expected_process_group) {
            ::close(accepted);
            mmltk::live::trace_workspace("host", "workspace.broker_peer_rejected", [&] {
                return nlohmann::json{{"pid", credentials.pid}, {"uid", credentials.uid}};
            });
            return;
        }
        client_fd = accepted;
        mmltk::live::trace_workspace("host", "workspace.broker_peer_accepted", [&] {
            return nlohmann::json{{"pid", credentials.pid}, {"process_group", expected_process_group}};
        });
    }

    [[nodiscard]] bool send_message(const Message& message, const int fd = -1) {
        if (client_fd < 0 || !workspace_surface_protocol::valid_header(message)) {
            return false;
        }
        iovec payload{const_cast<Message*>(&message), sizeof(message)};
        msghdr header{};
        header.msg_iov = &payload;
        header.msg_iovlen = 1U;
        std::array<std::byte, CMSG_SPACE(sizeof(int))> control{};
        if (fd >= 0) {
            header.msg_control = control.data();
            header.msg_controllen = control.size();
            cmsghdr* cmsg = CMSG_FIRSTHDR(&header);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
        }
        const ssize_t sent = ::sendmsg(client_fd, &header, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent == static_cast<ssize_t>(sizeof(message))) {
            return true;
        }
        if (sent < 0 && would_block()) {
            return false;
        }
        close_client();
        return false;
    }

    void send_error(const std::uint32_t code, const std::string_view detail) {
        Message message;
        message.type = MessageType::Error;
        message.error_code = code;
        const std::size_t count = std::min(detail.size(), message.token.size() - 1U);
        std::memcpy(message.token.data(), detail.data(), count);
        message.token[count] = '\0';
        (void)send_message(message);
    }

    void fail_client(const std::uint32_t code, const std::string_view detail) {
        mmltk::live::trace_workspace("host", "workspace.broker_error",
                                     [&] { return nlohmann::json{{"code", code}, {"detail", detail}}; });
        send_error(code, detail);
        if (client_fd < 0) {
            return;
        }
        authenticated = false;
        adapter_ready = false;
        adapter_modifiers.clear();
        mmltk::live::clear_workspace_vulkan_adapter();
        quarantined = true;
    }

    void receive_messages() {
        while (client_fd >= 0) {
            Message message{};
            iovec payload{&message, sizeof(message)};
            std::array<std::byte, CMSG_SPACE(sizeof(int))> control{};
            msghdr header{};
            header.msg_iov = &payload;
            header.msg_iovlen = 1U;
            header.msg_control = control.data();
            header.msg_controllen = control.size();
            const ssize_t received = ::recvmsg(client_fd, &header, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            if (received < 0 && (would_block() || errno == EINTR)) {
                return;
            }
            if (quarantined) {
                if (received < 0) {
                    close_client();
                    return;
                }
                for (cmsghdr* cmsg = CMSG_FIRSTHDR(&header); cmsg != nullptr; cmsg = CMSG_NXTHDR(&header, cmsg)) {
                    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
                        cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
                        const std::size_t bytes = cmsg->cmsg_len - CMSG_LEN(0U);
                        const std::size_t count = bytes / sizeof(int);
                        const int* fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
                        for (std::size_t index = 0U; index < count; ++index) {
                            ::close(fds[index]);
                        }
                    }
                }
                if (received == 0) {
                    close_client();
                }
                if (received <= 0) {
                    return;
                }
                continue;
            }
            if (received != static_cast<ssize_t>(sizeof(message)) ||
                (header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
                !workspace_surface_protocol::valid_header(message)) {
                fail_client(1U, "invalid workspace broker record");
                return;
            }

            int received_fd = -1;
            bool ancillary_valid = true;
            for (cmsghdr* cmsg = CMSG_FIRSTHDR(&header); cmsg != nullptr; cmsg = CMSG_NXTHDR(&header, cmsg)) {
                if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
                    cmsg->cmsg_len == CMSG_LEN(sizeof(int)) && received_fd < 0) {
                    std::memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(received_fd));
                } else {
                    ancillary_valid = false;
                }
            }
            if (!ancillary_valid) {
                if (received_fd >= 0) {
                    ::close(received_fd);
                }
                if (authenticated) {
                    fail_client(1U, "invalid ancillary descriptor count");
                } else {
                    close_client();
                }
                return;
            }

            if (!authenticated) {
                const std::string_view token(message.token.data(), message.token.size());
                if (message.type != MessageType::Hello || received_fd >= 0 ||
                    !constant_time_equal(token, session_token)) {
                    if (received_fd >= 0) {
                        ::close(received_fd);
                    }
                    fail_client(2U, "invalid Vulkan adapter negotiation record");
                    return;
                }
                authenticated = true;
                mmltk::live::trace_workspace("host", "workspace.broker_authenticated",
                                             [] { return nlohmann::json::object(); });
                continue;
            }

            if (message.type == MessageType::Error && received_fd < 0 && message.error_code != 0U) {
                const std::size_t detail_size = strnlen(message.token.data(), message.token.size());
                mmltk::logging::logger("gui.workspace_broker")
                    ->error("Firefox workspace broker error {}: {}", message.error_code,
                            std::string_view(message.token.data(), detail_size));
                authenticated = false;
                adapter_ready = false;
                adapter_modifiers.clear();
                mmltk::live::clear_workspace_vulkan_adapter();
                quarantined = true;
                return;
            }

            if (message.type == MessageType::Adapter && received_fd < 0) {
                const bool first = message.modifier_index == 0U;
                if (message.modifier_count == 0U || message.modifier_count > 256U ||
                    message.modifier_index >= message.modifier_count ||
                    (message.flags & workspace_surface_protocol::kFlagTimelineSemaphore) == 0U ||
                    (first && !adapter_modifiers.empty()) ||
                    (!first && adapter_modifiers.size() != message.modifier_index) ||
                    (!first &&
                     (adapter_render_major != message.render_major || adapter_render_minor != message.render_minor ||
                      adapter_device_uuid != message.device_uuid))) {
                    fail_client(3U, "invalid Vulkan adapter capability record");
                    return;
                }
                if (first) {
                    adapter_ready = false;
                    adapter_render_major = message.render_major;
                    adapter_render_minor = message.render_minor;
                    adapter_device_uuid = message.device_uuid;
                    adapter_modifiers.clear();
                    adapter_modifiers.reserve(message.modifier_count);
                }
                adapter_modifiers.push_back(message.drm_modifier);
                if (adapter_modifiers.size() == message.modifier_count) {
                    try {
                        std::sort(adapter_modifiers.begin(), adapter_modifiers.end());
                        adapter_modifiers.erase(std::unique(adapter_modifiers.begin(), adapter_modifiers.end()),
                                                adapter_modifiers.end());
                        mmltk::live::WorkspaceVulkanAdapterCapabilities capabilities;
                        capabilities.render_major = adapter_render_major;
                        capabilities.render_minor = adapter_render_minor;
                        capabilities.device_uuid = adapter_device_uuid;
                        capabilities.rgba8_modifiers = adapter_modifiers;
                        capabilities.timeline_semaphore = true;
                        mmltk::live::publish_workspace_vulkan_adapter(std::move(capabilities));
                        adapter_ready = true;
                        mmltk::live::trace_workspace("host", "workspace.adapter_received", [&] {
                            return nlohmann::json{{"render_major", adapter_render_major},
                                                  {"render_minor", adapter_render_minor},
                                                  {"modifier_count", adapter_modifiers.size()},
                                                  {"timeline_semaphore", true}};
                        });
                    } catch (const std::exception& error) {
                        mmltk::logging::logger("gui.workspace_broker")
                            ->error("workspace adapter negotiation failed: {}", error.what());
                        fail_client(3U, error.what());
                        return;
                    }
                }
                continue;
            }

            if (message.type == MessageType::ReserveSlot && received_fd < 0 && message.generation != 0U) {
                if (retiring_pools.contains(message.generation)) {
                    continue;
                }
                if (pool == nullptr || pool->generation() != message.generation) {
                    fail_client(4U, "workspace slot reservation generation is not active");
                    return;
                }
                if (!descriptor_value.has_value() || descriptor_value->generation != message.generation ||
                    message.slot >= descriptor_value->slots.size()) {
                    fail_client(4U, "workspace slot reservation is outside the active descriptor");
                    return;
                }
                const SlotKey key{message.generation, message.slot};
                if (!pending_slot_reservations.insert(key).second) {
                    fail_client(5U, "workspace slot reservation request is duplicated");
                    return;
                }
                mmltk::live::trace_workspace("host", "workspace.slot_reservation_requested", [&] {
                    return nlohmann::json{{"generation", message.generation}, {"slot", message.slot}};
                });
                continue;
            }

            if (message.type == MessageType::Ack && received_fd < 0 && message.generation != 0U &&
                message.flags == 0U) {
                const auto retired = retiring_pools.find(message.generation);
                if (retired == retiring_pools.end()) {
                    fail_client(4U, "unexpected workspace retirement acknowledgement");
                    return;
                }
                release_generation(message.generation);
                retiring_pools.erase(retired);
                mmltk::live::trace_workspace("host", "workspace.generation_retired",
                                             [&] { return nlohmann::json{{"generation", message.generation}}; });
                continue;
            }

            if (message.type != MessageType::SlotReady || received_fd < 0) {
                if (received_fd >= 0) {
                    ::close(received_fd);
                }
                fail_client(7U, "unexpected workspace broker record");
                return;
            }
            std::shared_ptr<mmltk::live::WorkspaceSurfacePool> target_pool;
            bool retiring = false;
            if (pool != nullptr && pool->generation() == message.generation) {
                target_pool = pool;
            } else if (const auto found = retiring_pools.find(message.generation); found != retiring_pools.end()) {
                target_pool = found->second;
                retiring = true;
            }
            if (target_pool == nullptr) {
                ::close(received_fd);
                fail_client(8U, "workspace slot-ready generation is unknown");
                return;
            }
            if (retiring) {
                ::close(received_fd);
                release(message.generation, message.slot, 0U);
                continue;
            }
            const SlotKey slot_key{message.generation, message.slot};
            auto reservation = in_flight.find(slot_key);
            if (reservation == in_flight.end() || reservation->second.pool != target_pool ||
                reservation->second.revision != 0U) {
                ::close(received_fd);
                fail_client(9U, "workspace slot-ready has no active import reservation");
                return;
            }
            std::string import_error;
            const bool replacing_timeline = target_pool->timeline_slot_ready(message.generation, message.slot);
            if (!replacing_timeline) {
                target_pool->invalidate_presentation();
            }
            const bool imported = replacing_timeline
                                      ? target_pool->replace_timeline_semaphore(message.generation, message.slot,
                                                                                received_fd, &import_error)
                                      : target_pool->import_timeline_semaphore(message.generation, message.slot,
                                                                               received_fd, &import_error);
            if (!imported) {
                mmltk::logging::logger("gui.workspace_broker")
                    ->error("workspace timeline import failed: {}", import_error);
                fail_client(11U, import_error);
                return;
            }
            mmltk::live::trace_workspace("host", "workspace.timeline_fd_imported", [&] {
                return nlohmann::json{
                    {"generation", message.generation}, {"slot", message.slot}, {"replacement", replacing_timeline}};
            });
            release(message.generation, message.slot, 0U);
        }
    }

    void refresh_descriptor() {
        if (pool == nullptr) {
            descriptor_value.reset();
            return;
        }
        mmltk::live::WorkspaceSwapchainDescriptor current = pool->descriptor();
        if (!current.valid()) {
            descriptor_value.reset();
            return;
        }
        if (!descriptor_value.has_value() || descriptor_value->generation != current.generation) {
            release_all();
            configured_generation = 0U;
            next_config_slot = 0U;
            last_present_revision = 0U;
            sync_ready_generation = 0U;
        }
        descriptor_value = std::move(current);
    }

    [[nodiscard]] bool configuration_delivered() const noexcept {
        return authenticated && adapter_ready && descriptor_value.has_value() &&
               configured_generation == descriptor_value->generation &&
               next_config_slot == descriptor_value->slots.size();
    }

    [[nodiscard]] bool import_reservations_pending() const noexcept {
        if (!descriptor_value.has_value()) {
            return true;
        }
        const std::uint64_t generation = descriptor_value->generation;
        if (std::ranges::any_of(pending_slot_reservations,
                                [generation](const SlotKey& key) { return key.generation == generation; })) {
            return true;
        }
        return std::ranges::any_of(in_flight, [generation](const auto& entry) {
            return entry.first.generation == generation && entry.second.revision == 0U;
        });
    }

    void send_configuration() {
        if (!authenticated || !adapter_ready || !descriptor_value.has_value()) {
            return;
        }
        const auto& descriptor = *descriptor_value;
        if (descriptor.slots.size() != 4U) {
            send_error(10U, "workspace pool must expose four slots");
            throw std::runtime_error("workspace broker requires exactly four persistent DMA-BUF slots");
        }
        if (configured_generation != descriptor.generation) {
            configured_generation = descriptor.generation;
            next_config_slot = 0U;
        }
        while (next_config_slot < descriptor.slots.size()) {
            const auto& slot = descriptor.slots[next_config_slot];
            if (!std::binary_search(adapter_modifiers.begin(), adapter_modifiers.end(),
                                    slot.dmabuf_image.drm_modifier)) {
                send_error(11U, "workspace DMA-BUF modifier was not negotiated");
                throw std::runtime_error("workspace DMA-BUF slot modifier was not negotiated with Firefox Vulkan");
            }
            const SlotKey key{descriptor.generation, slot.slot_index};
            const auto reservation = in_flight.find(key);
            if (reservation == in_flight.end()) {
                if (!pool->acquire_slot_for_import(slot.slot_index)) {
                    return;
                }
                in_flight.emplace(key, InFlightReservation{pool, 0U});
            } else if (reservation->second.revision != 0U) {
                return;
            }
            Message message;
            message.type = MessageType::ConfigureSlot;
            message.generation = descriptor.generation;
            message.slot = slot.slot_index;
            message.slot_count = static_cast<std::uint32_t>(descriptor.slots.size());
            message.width = descriptor.width;
            message.height = descriptor.height;
            message.stride = slot.dmabuf_image.stride_bytes;
            message.offset = slot.dmabuf_image.offset;
            message.allocation_size = slot.dmabuf_image.allocation_size;
            message.drm_modifier = slot.dmabuf_image.drm_modifier;
            message.drm_format = slot.dmabuf_image.drm_format;
            if (!send_message(message, slot.dmabuf_image.fd)) {
                return;
            }
            mmltk::live::trace_workspace("host", "workspace.dmabuf_fd_configured", [&] {
                return nlohmann::json{{"generation", descriptor.generation},
                                      {"slot", slot.slot_index},
                                      {"modifier", slot.dmabuf_image.drm_modifier},
                                      {"stride", slot.dmabuf_image.stride_bytes},
                                      {"allocation_size", slot.dmabuf_image.allocation_size}};
            });
            offered_generations.insert(descriptor.generation);
            ++next_config_slot;
        }
    }

    void send_retirements() {
        if (!authenticated) {
            return;
        }
        while (!pending_retirements.empty()) {
            Message message;
            message.type = MessageType::Retire;
            message.generation = pending_retirements.front();
            if (!send_message(message)) {
                return;
            }
            mmltk::live::trace_workspace("host", "workspace.generation_retire_sent",
                                         [&] { return nlohmann::json{{"generation", message.generation}}; });
            pending_retirements.erase(pending_retirements.begin());
        }
    }

    void process_slot_reservations() {
        if (!authenticated) {
            return;
        }
        for (auto current = pending_slot_reservations.begin(); current != pending_slot_reservations.end();) {
            const SlotKey key = *current;
            if (pool == nullptr || pool->generation() != key.generation) {
                fail_client(12U, "workspace slot reservation became stale");
                return;
            }
            if (const auto active = in_flight.find(key); active != in_flight.end()) {
                active->second.revision = 0U;
            } else {
                if (!pool->acquire_slot_for_import(key.slot)) {
                    ++current;
                    continue;
                }
                in_flight.emplace(key, InFlightReservation{pool, 0U});
            }
            pool->invalidate_presentation();
            Message reserved;
            reserved.type = MessageType::Ack;
            reserved.flags = workspace_surface_protocol::kFlagSlotReserved;
            reserved.generation = key.generation;
            reserved.slot = key.slot;
            if (!send_message(reserved)) {
                return;
            }
            mmltk::live::trace_workspace("host", "workspace.slot_reserved", [&] {
                return nlohmann::json{{"generation", key.generation}, {"slot", key.slot}};
            });
            current = pending_slot_reservations.erase(current);
        }
    }

    void pump() {
        accept_client();
        receive_messages();
        process_slot_reservations();
        send_retirements();
        refresh_descriptor();
        send_configuration();
        receive_messages();
    }

    [[nodiscard]] std::optional<WorkspaceSurfacePresent> poll_latest_present() {
        pump();
        if (!authenticated || !descriptor_value.has_value() || pool == nullptr || import_reservations_pending() ||
            !pool->timeline_sync_ready(descriptor_value->generation)) {
            return std::nullopt;
        }
        const mmltk::live::WorkspacePresentSnapshot snapshot = pool->latest_present();
        if (sync_ready_generation != descriptor_value->generation) {
            sync_ready_generation = descriptor_value->generation;
            last_present_revision = 0U;
        }
        if (!snapshot.valid || snapshot.swapchain_generation != descriptor_value->generation ||
            snapshot.revision == last_present_revision ||
            pending_slot_reservations.contains(SlotKey{snapshot.swapchain_generation, snapshot.front_slot_index})) {
            return std::nullopt;
        }
        Message message;
        message.type = MessageType::Present;
        message.generation = snapshot.swapchain_generation;
        message.revision = snapshot.revision;
        message.slot = snapshot.front_slot_index;
        message.width = snapshot.dims.width;
        message.height = snapshot.dims.height;
        message.source_x = snapshot.source_region.x;
        message.source_y = snapshot.source_region.y;
        message.source_width = snapshot.source_region.width;
        message.source_height = snapshot.source_region.height;
        message.capture_ns = snapshot.capture_ns;
        message.ready_ns = snapshot.ready_ns;
        if (!pool->mark_slot_in_flight(snapshot.front_slot_index, true)) {
            mmltk::live::trace_workspace("host", "workspace.present_dropped", [&] {
                return nlohmann::json{{"generation", snapshot.swapchain_generation},
                                      {"slot", snapshot.front_slot_index},
                                      {"revision", snapshot.revision},
                                      {"reason", "slot_not_reservable"}};
            });
            return std::nullopt;
        }
        in_flight[SlotKey{snapshot.swapchain_generation, snapshot.front_slot_index}] =
            InFlightReservation{pool, snapshot.revision};
        if (!send_message(message)) {
            release(snapshot.swapchain_generation, snapshot.front_slot_index, snapshot.revision);
            return std::nullopt;
        }
        last_present_revision = snapshot.revision;
        mmltk::live::trace_workspace("host", "workspace.present_sent", [&] {
            return nlohmann::json{{"generation", snapshot.swapchain_generation},
                                  {"slot", snapshot.front_slot_index},
                                  {"revision", snapshot.revision},
                                  {"capture_ns", snapshot.capture_ns},
                                  {"ready_ns", snapshot.ready_ns}};
        });
        return WorkspaceSurfacePresent{snapshot.swapchain_generation, snapshot.revision,    snapshot.front_slot_index,
                                       snapshot.dims.width,           snapshot.dims.height, snapshot.source_region,
                                       snapshot.capture_ns,           snapshot.ready_ns};
    }

    void release(const std::uint64_t generation, const std::uint32_t slot, const std::uint64_t revision) noexcept {
        const auto found = in_flight.find(SlotKey{generation, slot});
        if (found == in_flight.end() || found->second.revision != revision) {
            return;
        }
        try {
            if (found->second.pool != nullptr) {
                (void)found->second.pool->mark_slot_in_flight(slot, false);
            }
        } catch (const std::exception& error) {
            mmltk::logging::logger("gui.workspace_broker")->error("workspace slot release failed: {}", error.what());
        }
        in_flight.erase(found);
        mmltk::live::trace_workspace("host", "workspace.slot_released", [&] {
            return nlohmann::json{{"generation", generation}, {"slot", slot}, {"revision", revision}};
        });
    }

    void release_generation(const std::uint64_t generation) noexcept {
        for (auto current = in_flight.begin(); current != in_flight.end();) {
            if (current->first.generation != generation) {
                ++current;
                continue;
            }
            try {
                if (current->second.pool != nullptr) {
                    (void)current->second.pool->mark_slot_in_flight(current->first.slot, false);
                }
            } catch (const std::exception& error) {
                mmltk::logging::logger("gui.workspace_broker")
                    ->error("workspace generation release failed: {}", error.what());
            }
            current = in_flight.erase(current);
        }
    }

    void release_all() noexcept {
        for (const auto& [key, reservation] : in_flight) {
            try {
                if (reservation.pool != nullptr) {
                    (void)reservation.pool->mark_slot_in_flight(key.slot, false);
                }
            } catch (const std::exception& error) {
                mmltk::logging::logger("gui.workspace_broker")
                    ->error("workspace slot teardown release failed: {}", error.what());
            }
        }
        in_flight.clear();
    }

    std::filesystem::path socket_path;
    std::string session_token;
    int listen_fd = -1;
    int client_fd = -1;
    pid_t expected_process_group = -1;
    bool authenticated = false;
    bool quarantined = false;
    bool adapter_ready = false;
    std::uint32_t adapter_render_major = 0U;
    std::uint32_t adapter_render_minor = 0U;
    std::array<std::uint8_t, workspace_surface_protocol::kDeviceUuidBytes> adapter_device_uuid{};
    std::vector<std::uint64_t> adapter_modifiers;
    std::shared_ptr<mmltk::live::WorkspaceSurfacePool> pool;
    std::optional<mmltk::live::WorkspaceSwapchainDescriptor> descriptor_value;
    std::uint64_t configured_generation = 0U;
    std::size_t next_config_slot = 0U;
    std::uint64_t last_present_revision = 0U;
    std::uint64_t sync_ready_generation = 0U;
    std::unordered_map<SlotKey, InFlightReservation, SlotKeyHash> in_flight;
    std::unordered_set<SlotKey, SlotKeyHash> pending_slot_reservations;
    std::unordered_map<std::uint64_t, std::shared_ptr<mmltk::live::WorkspaceSurfacePool>> retiring_pools;
    std::unordered_set<std::uint64_t> offered_generations;
    std::vector<std::uint64_t> pending_retirements;
    mutable std::mutex mutex;
};

WorkspaceSurfaceBroker::WorkspaceSurfaceBroker(const std::filesystem::path& socket_path, std::string session_token)
    : impl_(std::make_unique<Impl>(socket_path, std::move(session_token))) {}

WorkspaceSurfaceBroker::~WorkspaceSurfaceBroker() = default;

const std::filesystem::path& WorkspaceSurfaceBroker::socket_path() const noexcept {
    return impl_->socket_path;
}

int WorkspaceSurfaceBroker::poll_fd() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->client_fd >= 0 ? impl_->client_fd : impl_->listen_fd;
}

void WorkspaceSurfaceBroker::set_expected_process_group(const pid_t process_group) {
    if (process_group <= 0) {
        throw std::invalid_argument("workspace surface broker requires a positive Firefox process group");
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->client_fd >= 0) {
        throw std::logic_error("workspace surface broker process group cannot change after accepting Firefox");
    }
    impl_->expected_process_group = process_group;
}

void WorkspaceSurfaceBroker::pump() {
    std::scoped_lock lock(impl_->mutex);
    impl_->pump();
}

void WorkspaceSurfaceBroker::update_pool(std::shared_ptr<mmltk::live::WorkspaceSurfacePool> pool) {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->pool != pool) {
        if (impl_->pool != nullptr) {
            const std::uint64_t generation = impl_->pool->generation();
            if (impl_->offered_generations.erase(generation) != 0U) {
                impl_->retiring_pools.insert_or_assign(generation, impl_->pool);
                impl_->pending_retirements.push_back(generation);
            } else {
                impl_->release_generation(generation);
            }
        }
        impl_->pool = std::move(pool);
        mmltk::live::trace_workspace("host", "workspace.pool_replaced", [&] {
            return nlohmann::json{{"generation", impl_->pool != nullptr ? impl_->pool->generation() : 0U},
                                  {"retiring_count", impl_->retiring_pools.size()}};
        });
        impl_->descriptor_value.reset();
        impl_->configured_generation = 0U;
        impl_->next_config_slot = 0U;
        impl_->last_present_revision = 0U;
        impl_->sync_ready_generation = 0U;
        impl_->pending_slot_reservations.clear();
    }
    impl_->pump();
}

std::optional<WorkspaceSurfacePresent> WorkspaceSurfaceBroker::poll_latest_present() {
    std::scoped_lock lock(impl_->mutex);
    return impl_->poll_latest_present();
}

void WorkspaceSurfaceBroker::release(const std::uint64_t generation, const std::uint32_t slot,
                                     const std::uint64_t revision) noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->release(generation, slot, revision);
}

void WorkspaceSurfaceBroker::release_all() noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->release_all();
}

bool WorkspaceSurfaceBroker::ready() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    const auto& descriptor = impl_->descriptor_value;
    return descriptor.has_value() && impl_->configuration_delivered() && !impl_->import_reservations_pending() &&
           impl_->pool != nullptr && impl_->pool->timeline_sync_ready(descriptor->generation);
}

std::optional<mmltk::live::WorkspaceSwapchainDescriptor> WorkspaceSurfaceBroker::descriptor() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->configuration_delivered() ? impl_->descriptor_value : std::nullopt;
}

}  
