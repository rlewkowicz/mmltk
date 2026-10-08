#pragma once
#include <cstring>
#include <span>
#include <sys/socket.h>
namespace mmltk::common::io {
// The caller supplies a control header with room for this descriptor payload.
inline void write_socket_descriptors(cmsghdr& header, std::span<const int> descriptors) noexcept {
 header.cmsg_level = SOL_SOCKET;
 header.cmsg_type = SCM_RIGHTS;
 header.cmsg_len = CMSG_LEN(descriptors.size_bytes());
 std::memcpy(CMSG_DATA(&header), descriptors.data(), descriptors.size_bytes());
}
}  // namespace mmltk::common::io
