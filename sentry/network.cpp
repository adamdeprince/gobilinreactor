#include "files.h"
#include "sentry.h"
#include "dns.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace goblin {
std::optional<long> FileTable::Network(const SyscallRequest& req, const GuestMemory& mem) {
    if (auto value = Unix(req, mem)) return value;
    const auto& a = req.args;
    constexpr size_t limit = 1 << 20;
    if (req.nr == __NR_sendmmsg) {
        struct Message { msghdr header; uint32_t length, padding; };
        static_assert(sizeof(Message) == 64);
        if (a[2] > 1024) return -EINVAL;
        if (a[1] > UINTPTR_MAX - a[2] * sizeof(Message)) return -EFAULT;
        for (size_t i = 0; i < a[2]; ++i) {
            uintptr_t item = a[1] + i * sizeof(Message), length = item + offsetof(Message, length);
            uint32_t old;
            if (!mem.Read(length, &old, sizeof(old)) || !mem.Write(length, &old, sizeof(old))) return i ? long(i) : -EFAULT;
            SyscallRequest one{}; one.nr = __NR_sendmsg;
            one.args[0] = a[0]; one.args[1] = item; one.args[2] = a[3];
            auto sent = Network(one, mem);
            if (!sent || *sent < 0) return i ? long(i) : sent.value_or(-ENOSYS);
            uint32_t bytes = *sent;
            if (!mem.Write(length, &bytes, sizeof(bytes))) return i ? long(i) : -EFAULT;
        }
        return long(a[2]);
    }
    auto install = [&](int fd, int domain, int flags) -> long {
        if (fd < 0) return -errno;
        auto file = std::make_shared<OpenFile>();
        file->host = std::make_shared<HostFile>(fd);
        file->socket_domain = domain; file->pollable = true;
        file->flags = O_RDWR | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0);
        return Install(file, flags & SOCK_CLOEXEC);
    };
    if (req.nr == __NR_socket) {
        if (a[0] != AF_INET && a[0] != AF_INET6) return -EAFNOSUPPORT;
        int type = a[1] & ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (type != SOCK_STREAM && type != SOCK_DGRAM) return -ESOCKTNOSUPPORT;
        if (a[2] && a[2] != static_cast<unsigned>(type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP)) return -EPROTONOSUPPORT;
        return install(socket(a[0], a[1] | SOCK_CLOEXEC | SOCK_NONBLOCK, a[2]), a[0], a[1]);
    }
    if (req.nr < __NR_bind || (req.nr > __NR_recvmsg && req.nr != __NR_accept4)) return {};
    const auto* descriptor = Get(a[0]);
    if (!descriptor) return -EBADF;
    auto file = descriptor->file;
    if (!file->socket_domain || !file->host) return -ENOTSOCK;
    const int fd = file->host->fd;
    auto outgoing = [&](sockaddr_storage* peer) -> long {
        if (!dns || peer->ss_family != AF_INET) return 0;
        auto* ipv4 = reinterpret_cast<sockaddr_in*>(peer);
        if (ipv4->sin_addr.s_addr != htonl(0x7f000035) || ipv4->sin_port != htons(53)) return 0;
        int type = 0; socklen_t size = sizeof(type);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) < 0) return -errno;
        if (type != SOCK_DGRAM) return -EOPNOTSUPP;
        ipv4->sin_addr.s_addr = htonl(INADDR_LOOPBACK); ipv4->sin_port = htons(dns->port());
        file->dns_forwarded = true; return 0;
    };
    auto incoming = [&](sockaddr_storage* peer) {
        if (!dns || !file->dns_forwarded || peer->ss_family != AF_INET) return;
        auto* ipv4 = reinterpret_cast<sockaddr_in*>(peer);
        if (ipv4->sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ipv4->sin_port == htons(dns->port())) {
            ipv4->sin_addr.s_addr = htonl(0x7f000035); ipv4->sin_port = htons(53);
        }
    };
    auto result = [&](long n, int flags = 0) -> long {
        if (n >= 0) return n;
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && !(file->flags & O_NONBLOCK) && !(flags & MSG_DONTWAIT)) return kBlocked;
        return -errno;
    };
    auto address = [&](uintptr_t pointer, size_t size, sockaddr_storage* out) -> long {
        if (size > sizeof(*out) || size < sizeof(sa_family_t)) return -EINVAL;
        if (!mem.Read(pointer, out, size)) return -EFAULT;
        if (out->ss_family != file->socket_domain && out->ss_family != AF_UNSPEC) return -EAFNOSUPPORT;
        if ((out->ss_family == AF_INET && size < sizeof(sockaddr_in)) ||
            (out->ss_family == AF_INET6 && size < sizeof(sockaddr_in6))) return -EINVAL;
        return 0;
    };
    auto put_address = [&](uintptr_t pointer, uintptr_t length, const sockaddr_storage& value, socklen_t size) -> long {
        if (!pointer) return 0;
        socklen_t capacity;
        if (!mem.Read(length, &capacity, sizeof(capacity))) return -EFAULT;
        return mem.Write(pointer, &value, std::min(capacity, size)) && mem.Write(length, &size, sizeof(size)) ? 0 : -EFAULT;
    };
    switch (req.nr) {
        case __NR_bind: case __NR_connect: {
            sockaddr_storage value{};
            long rc = address(a[1], a[2], &value); if (rc < 0) return rc;
            if (req.nr == __NR_bind) return result(bind(fd, reinterpret_cast<sockaddr*>(&value), a[2]));
            rc = outgoing(&value); if (rc < 0) return rc;
            if (file->connecting) {
                pollfd wait{fd, POLLOUT, 0};
                int ready = poll(&wait, 1, 0);
                if (ready < 0) return -errno;
                if (!ready) return file->flags & O_NONBLOCK ? -EALREADY : kBlocked;
                int error = 0; socklen_t size = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) return -errno;
                file->connecting = false; return -error;
            }
            int n = connect(fd, reinterpret_cast<sockaddr*>(&value), a[2]);
            if (n < 0 && errno == EINPROGRESS) { file->connecting = true; return file->flags & O_NONBLOCK ? -EINPROGRESS : kBlocked; }
            return result(n);
        }
        case __NR_listen: return result(listen(fd, a[1]));
        case __NR_accept: case __NR_accept4: {
            const int flags = req.nr == __NR_accept4 ? a[3] : 0;
            if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) return -EINVAL;
            sockaddr_storage peer{}; socklen_t size = sizeof(peer);
            int accepted = accept4(fd, reinterpret_cast<sockaddr*>(&peer), &size, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (accepted < 0) return result(accepted);
            long rc = put_address(a[1], a[2], peer, size);
            if (rc < 0) { close(accepted); return rc; }
            return install(accepted, file->socket_domain, flags);
        }
        case __NR_getsockname: case __NR_getpeername: {
            sockaddr_storage peer{}; socklen_t size = sizeof(peer);
            int rc = req.nr == __NR_getsockname ? getsockname(fd, reinterpret_cast<sockaddr*>(&peer), &size)
                                               : getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &size);
            if (req.nr == __NR_getpeername) incoming(&peer);
            return rc < 0 ? -errno : put_address(a[1], a[2], peer, size);
        }
        case __NR_shutdown: return result(shutdown(fd, a[1]));
        case __NR_setsockopt: case __NR_getsockopt: {
            bool allowed = false;
            if (a[1] == SOL_SOCKET) switch (a[2]) {
                case SO_ERROR: case SO_TYPE: case SO_REUSEADDR: case SO_REUSEPORT:
                case SO_KEEPALIVE: case SO_RCVBUF: case SO_SNDBUF: case SO_BROADCAST:
                case SO_RCVLOWAT: allowed = true;
            }
            if (a[1] == IPPROTO_TCP) switch (a[2]) {
                case TCP_NODELAY: case TCP_KEEPIDLE: case TCP_KEEPINTVL: case TCP_KEEPCNT: allowed = true;
            }
            // glibc's resolver enables asynchronous UDP errors before sending
            // its first query. These options configure this owned socket only.
            if (a[1] == IPPROTO_IP && a[2] == IP_RECVERR) allowed = true;
            if (a[1] == IPPROTO_IPV6 && (a[2] == IPV6_V6ONLY || a[2] == IPV6_RECVERR)) allowed = true;
            if (!allowed) return -ENOPROTOOPT;
            int value = 0;
            if (req.nr == __NR_setsockopt) {
                if (a[4] < sizeof(value)) return -EINVAL;
                if (!mem.Read(a[3], &value, sizeof(value))) return -EFAULT;
                return result(setsockopt(fd, a[1], a[2], &value, sizeof(value)));
            }
            socklen_t capacity, size = sizeof(value);
            if (!mem.Read(a[4], &capacity, sizeof(capacity))) return -EFAULT;
            if (getsockopt(fd, a[1], a[2], &value, &size) < 0) return -errno;
            size = std::min(size, capacity);
            return mem.Write(a[3], &value, size) && mem.Write(a[4], &size, sizeof(size)) ? 0 : -EFAULT;
        }
        case __NR_sendto: case __NR_recvfrom: {
            if (a[2] > limit) return -EMSGSIZE;
            std::vector<uint8_t> data(a[2]);
            if (!mem.Read(a[1], data.data(), data.size())) return -EFAULT;
            sockaddr_storage peer{}; socklen_t size = sizeof(peer);
            if (req.nr == __NR_sendto) {
                if (a[4]) { long rc = address(a[4], a[5], &peer); if (rc < 0) return rc; }
                if (a[4]) { long rc = outgoing(&peer); if (rc < 0) return rc; }
                return result(sendto(fd, data.data(), data.size(), a[3] | MSG_NOSIGNAL | MSG_DONTWAIT,
                    a[4] ? reinterpret_cast<sockaddr*>(&peer) : nullptr, a[4] ? a[5] : 0), a[3]);
            }
            if (!mem.Write(a[1], data.data(), data.size())) return -EFAULT;
            long n = recvfrom(fd, data.data(), data.size(), a[3] | MSG_DONTWAIT,
                a[4] ? reinterpret_cast<sockaddr*>(&peer) : nullptr, a[4] ? &size : nullptr);
            if (n < 0) return result(n, a[3]);
            incoming(&peer);
            long rc = put_address(a[4], a[5], peer, size);
            return rc < 0 ? rc : mem.Write(a[1], data.data(), std::min<size_t>(n, data.size())) ? n : -EFAULT;
        }
        case __NR_sendmsg: case __NR_recvmsg: {
            msghdr guest{};
            if (!mem.Read(a[1], &guest, sizeof(guest))) return -EFAULT;
            if (guest.msg_iovlen > 1024 || guest.msg_namelen > sizeof(sockaddr_storage)) return -EINVAL;
            if (guest.msg_controllen && req.nr == __NR_sendmsg) return -EOPNOTSUPP;
            std::vector<iovec> pointers(guest.msg_iovlen), local(guest.msg_iovlen);
            if (!mem.Read(reinterpret_cast<uintptr_t>(guest.msg_iov), pointers.data(), pointers.size() * sizeof(iovec))) return -EFAULT;
            size_t bytes = 0;
            for (const auto& iov : pointers) { if (iov.iov_len > limit - bytes) return -EMSGSIZE; bytes += iov.iov_len; }
            std::vector<uint8_t> data(bytes);
            size_t at = 0;
            for (size_t i = 0; i < pointers.size(); ++i) {
                local[i] = {data.data() + at, pointers[i].iov_len}; at += pointers[i].iov_len;
                if (!mem.Read(reinterpret_cast<uintptr_t>(pointers[i].iov_base), local[i].iov_base, local[i].iov_len)) return -EFAULT;
                if (req.nr == __NR_recvmsg && !mem.Write(reinterpret_cast<uintptr_t>(pointers[i].iov_base), local[i].iov_base, local[i].iov_len)) return -EFAULT;
            }
            sockaddr_storage peer{};
            msghdr message{}; message.msg_iov = local.data(); message.msg_iovlen = local.size();
            if (guest.msg_name) {
                message.msg_name = &peer; message.msg_namelen = sizeof(peer);
                if (req.nr == __NR_sendmsg) {
                    long rc = address(reinterpret_cast<uintptr_t>(guest.msg_name), guest.msg_namelen, &peer);
                    if (rc < 0) return rc;
                    rc = outgoing(&peer); if (rc < 0) return rc;
                    message.msg_namelen = guest.msg_namelen;
                }
            }
            if (req.nr == __NR_sendmsg) return result(sendmsg(fd, &message, a[2] | MSG_DONTWAIT | MSG_NOSIGNAL), a[2]);
            long n = recvmsg(fd, &message, a[2] | MSG_DONTWAIT);
            if (n < 0) return result(n, a[2]);
            incoming(&peer);
            size_t copied = 0;
            for (size_t i = 0; i < pointers.size() && copied < static_cast<size_t>(n); ++i) {
                size_t amount = std::min<size_t>(local[i].iov_len, n - copied);
                if (!mem.Write(reinterpret_cast<uintptr_t>(pointers[i].iov_base), local[i].iov_base, amount)) return -EFAULT;
                copied += amount;
            }
            if (guest.msg_name && !mem.Write(reinterpret_cast<uintptr_t>(guest.msg_name), &peer,
                std::min(guest.msg_namelen, message.msg_namelen))) return -EFAULT;
            guest.msg_namelen = message.msg_namelen; guest.msg_controllen = 0; guest.msg_flags = message.msg_flags;
            return mem.Write(a[1], &guest, sizeof(guest)) ? n : -EFAULT;
        }
        default: return -ENOSYS;
    }
}
}
