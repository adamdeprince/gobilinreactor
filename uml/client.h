#pragma once
#include "protocol.h"
#include <cerrno>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace goblin_uml {
// The socket is private to the Android app UID. It controls the guest, never
// Android credentials. Used by acceptance tests and the packaged CLI.
inline int Request(const std::string& socket_path, Operation operation,
                   const std::string& payload,
                   const std::function<void(const char*, size_t)>& output) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error(strerror(errno));
    struct Close { int fd; ~Close() { close(fd); } } closer{fd};
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) throw std::runtime_error("Control path too long");
    memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) throw std::runtime_error(strerror(errno));
    auto transfer = [&](void* data, size_t size, bool sending) {
        auto* bytes = static_cast<char*>(data);
        while (size) {
            ssize_t n = sending ? send(fd, bytes, size, MSG_NOSIGNAL) : read(fd, bytes, size);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error(n ? strerror(errno) : "Linux control disconnected");
            bytes += n; size -= n;
        }
    };
    if (payload.size() > UINT32_MAX) throw std::runtime_error("Control message cannot be represented");
    Header h{kMagic, operation, 0, uint32_t(payload.size())};
    transfer(&h, sizeof(h), true); transfer(const_cast<char*>(payload.data()), payload.size(), true);
    for (;;) {
        transfer(&h, sizeof(h), false);
        if (h.magic != kMagic) throw std::runtime_error("Invalid control response");
        std::string bytes(h.size, '\0'); transfer(bytes.data(), bytes.size(), false);
        if (h.operation == Output || h.operation == FileData) output(bytes.data(), bytes.size());
        else if (h.operation == Error) throw std::runtime_error(bytes);
        else if (h.operation == Exited) {
            if (bytes.size() != sizeof(int32_t)) throw std::runtime_error("Invalid exit status");
            int32_t status; memcpy(&status, bytes.data(), sizeof(status)); return status;
        }
    }
}
}
