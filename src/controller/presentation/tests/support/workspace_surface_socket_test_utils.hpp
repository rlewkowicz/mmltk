#pragma once
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include "src/common/io/scoped_fd.h"
#include "src/controller/presentation/abi/workspace_surface_import_abi.h"
namespace mmltk::testsupport {
// Upper bound on the descriptors a single workspace surface protocol message carries, which also
// sizes the ancillary control buffer.
inline constexpr std::size_t kMaxWorkspaceSurfaceDescriptors = 8U;
struct WorkspaceSurfaceDescriptors final {
    std::array<mmltk::common::io::ScopedFd, kMaxWorkspaceSurfaceDescriptors> descriptors{};
    std::size_t descriptor_count = 0U;
};
[[nodiscard]] inline mmltk::common::io::ScopedFd connect_workspace_surface_shell(const std::filesystem::path& path) {
    mmltk::common::io::ScopedFd socket{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)};
    if (socket.get() < 0) { throw std::runtime_error("workspace test shell socket creation failed"); }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string native = path.string();
    if (native.size() >= sizeof(address.sun_path)) { throw std::runtime_error("workspace test shell socket path is too long"); }
    std::memcpy(address.sun_path, native.data(), native.size());
    if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        throw std::runtime_error("workspace test shell connection failed");
    }
    return socket;
}
[[nodiscard]] inline mmltk::common::io::ScopedFd workspace_surface_event_descriptor() {
    const int descriptor = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (descriptor < 0) { throw std::runtime_error("workspace test eventfd creation failed"); }
    return mmltk::common::io::ScopedFd{descriptor};
}
[[nodiscard]] inline WorkspaceSurfaceDescriptors receive_message_with_descriptors(const int socket, const std::span<std::byte> record,
                                                                                  const char* const failure_message) {
    WorkspaceSurfaceDescriptors received;
    iovec payload{.iov_base = record.data(), .iov_len = record.size()};
    std::array<std::byte, CMSG_SPACE(sizeof(int) * kMaxWorkspaceSurfaceDescriptors)> control{};
    msghdr header{};
    header.msg_iov = &payload;
    header.msg_iovlen = 1U;
    header.msg_control = control.data();
    header.msg_controllen = control.size();
    if (::recvmsg(socket, &header, MSG_CMSG_CLOEXEC) != static_cast<ssize_t>(record.size())) { throw std::runtime_error(failure_message); }
    for (cmsghdr* ancillary = CMSG_FIRSTHDR(&header); ancillary != nullptr; ancillary = CMSG_NXTHDR(&header, ancillary)) {
        if (ancillary->cmsg_level != SOL_SOCKET || ancillary->cmsg_type != SCM_RIGHTS) continue;
        const std::size_t count = (ancillary->cmsg_len - CMSG_LEN(0U)) / sizeof(int);
        const auto* const descriptors = reinterpret_cast<const int*>(CMSG_DATA(ancillary));
        for (std::size_t index = 0U; index < count; ++index) {
            if (received.descriptor_count == received.descriptors.size()) { throw std::runtime_error("workspace protocol message carries excess descriptors"); }
            received.descriptors[received.descriptor_count++] = mmltk::common::io::ScopedFd{descriptors[index]};
        }
    }
    return received;
}
// Sends one fixed-size protocol message over a SOCK_SEQPACKET socket, attaching `descriptors` as
// SCM_RIGHTS ancillary data. Returns false when the socket would block; any other failure throws
// with `failure_message`.
[[nodiscard]] inline bool send_message_with_descriptors(const int socket, const std::span<const std::byte> message, const std::span<const int> descriptors,
                                                        const char* const failure_message) {
    if (descriptors.size() > kMaxWorkspaceSurfaceDescriptors) { throw std::runtime_error("workspace protocol message carries too many descriptors"); }
    iovec payload{const_cast<std::byte*>(message.data()), message.size()};
    std::array<std::byte, CMSG_SPACE(sizeof(int) * kMaxWorkspaceSurfaceDescriptors)> control{};
    msghdr header{};
    header.msg_iov = &payload;
    header.msg_iovlen = 1U;
    if (!descriptors.empty()) {
        header.msg_control = control.data();
        header.msg_controllen = CMSG_SPACE(descriptors.size_bytes());
        cmsghdr* const ancillary = CMSG_FIRSTHDR(&header);
        if (ancillary == nullptr) { throw std::runtime_error("workspace protocol control message header is missing"); }
        ancillary->cmsg_level = SOL_SOCKET;
        ancillary->cmsg_type = SCM_RIGHTS;
        ancillary->cmsg_len = CMSG_LEN(descriptors.size_bytes());
        std::memcpy(CMSG_DATA(ancillary), descriptors.data(), descriptors.size_bytes());
    }
    const ssize_t sent = ::sendmsg(socket, &header, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent == static_cast<ssize_t>(message.size())) { return true; }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { return false; }
    throw std::runtime_error(failure_message);
}
[[nodiscard]] inline bool send_workspace_record(const int socket, const mmltk::controller::presentation::detail::workspace_surface_import::Record& record,
                                                const std::span<const int> descriptors = {}) {
    return send_message_with_descriptors(socket, std::as_bytes(std::span{&record, 1U}), descriptors, "workspace protocol test send failed");
}
[[nodiscard]] inline WorkspaceSurfaceDescriptors receive_workspace_record(const int socket,
                                                                          mmltk::controller::presentation::detail::workspace_surface_import::Record& record) {
    return receive_message_with_descriptors(socket, std::as_writable_bytes(std::span{&record, 1U}), "workspace protocol test receive failed");
}
}  // namespace mmltk::testsupport
