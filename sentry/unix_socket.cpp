#include "files.h"
#include "sentry.h"
#include "vfs_metadata.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

namespace goblin {
UnixEndpoint::~UnixEndpoint() { Clear(); }
void UnixEndpoint::Clear() {
    if (auto ns = space.lock()) { ns->Release(queued); ns->dirty = true; }
    queued = 0; messages.clear(); pending.clear();
}
std::shared_ptr<UnixEndpoint> UnixNamespace::Create(int type, int pid) {
    if (endpoints.size() >= 32) endpoints.erase(std::remove_if(endpoints.begin(), endpoints.end(), [](const auto& e) { return e.expired(); }), endpoints.end());
    auto endpoint = std::make_shared<UnixEndpoint>(type);
    endpoint->owner = pid; endpoint->uid = CurrentCredentials().euid; endpoint->gid = CurrentCredentials().egid; endpoint->space = shared_from_this(); endpoints.push_back(endpoint); return endpoint;
}
bool UnixNamespace::Charge(size_t bytes) { if (bytes > kTotalLimit - queued) return false; queued += bytes; return true; }
void UnixNamespace::Release(size_t bytes) { queued -= std::min(queued, bytes); }
void UnixNamespace::Collect() {
    if (!dirty || !roots) return;
    dirty = false;
    std::set<UnixEndpoint*> marked;
    std::vector<std::shared_ptr<UnixEndpoint>> work;
    auto mark = [&](const std::shared_ptr<UnixEndpoint>& socket) {
        if (socket && marked.insert(socket.get()).second) work.push_back(socket);
    };
    for (const auto& file : roots()) mark(file->unix_socket);
    for (size_t at = 0; at < work.size(); ++at) {
        auto endpoint = work[at];
        for (const auto& child : endpoint->pending) mark(child);
        for (const auto& message : endpoint->messages) for (const auto& file : message.rights) mark(file->unix_socket);
    }
    // SCM_RIGHTS can form unreachable socket cycles. Collect from guest fd
    // tables instead of relying on reference counts to release those queues.
    std::vector<std::shared_ptr<UnixEndpoint>> garbage;
    for (const auto& weak : endpoints) if (auto endpoint = weak.lock())
        if (!marked.count(endpoint.get())) garbage.push_back(endpoint);
    for (const auto& endpoint : garbage) { endpoint->collected = true; endpoint->Clear(); }
    garbage.clear();
    endpoints.erase(std::remove_if(endpoints.begin(), endpoints.end(), [](const auto& e) { return e.expired(); }), endpoints.end());
    for (auto it = names.begin(); it != names.end();) {
        if (it->second.expired()) it = names.erase(it); else ++it;
    }
    dirty = false;
}
std::vector<std::shared_ptr<OpenFile>> FileTable::OpenFiles() const {
    std::vector<std::shared_ptr<OpenFile>> result;
    for (const auto& [fd, descriptor] : descriptors_) result.push_back(descriptor.file);
    return result;
}
short FileTable::VirtualEvents(int fd, short events) const {
    auto* d = Get(fd); if (!d || (d->file->flags & O_PATH)) return POLLNVAL;
    if (d->file->unix_socket) {
        auto e = d->file->unix_socket; auto peer = e->peer.lock(); short ready = 0;
        if (e->listening) return e->pending.empty() ? 0 : events & POLLIN;
        bool ended = e->type != SOCK_DGRAM && (!peer || peer->write_closed || peer->collected);
        if (!e->messages.empty() || e->read_closed || ended) ready |= POLLIN;
        if (ended && e->connected) ready |= POLLRDHUP;
        if (e->connected && (!peer || peer->collected || (e->write_closed && ended))) ready |= POLLHUP;
        bool room = unix_namespace->queued + 256 < UnixNamespace::kTotalLimit;
        if (room && !e->write_closed && (!peer || (!peer->read_closed && peer->queued + 256 < UnixNamespace::kQueueLimit))) ready |= POLLOUT;
        return (ready & events) | (ready & (POLLHUP | POLLERR));
    }
    if (d->file->proc) return events & (POLLIN | POLLOUT);
    return events & (d->file->kind == OpenFile::kInput ? POLLIN : POLLOUT);
}
long FileTable::UnixSend(const std::shared_ptr<UnixEndpoint>& sender, const std::shared_ptr<UnixEndpoint>& receiver,
                         std::vector<uint8_t> data, std::vector<std::shared_ptr<OpenFile>> rights, int flags) {
    if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL | MSG_MORE)) return -EOPNOTSUPP;
    if (sender->write_closed) return -EPIPE;
    if (!receiver || receiver->collected || receiver->read_closed) return sender->type == SOCK_DGRAM ? -ECONNREFUSED : -EPIPE;
    if (sender->type != receiver->type) return -EPROTOTYPE;
    if (sender->type == SOCK_STREAM && data.empty()) return 0;
    if (sender->type == SOCK_DGRAM && receiver->connected && receiver->peer.lock() != sender) return -EPERM;
    size_t overhead = 256 + rights.size() * sizeof(std::shared_ptr<OpenFile>);
    size_t room = std::min(UnixNamespace::kQueueLimit - receiver->queued, UnixNamespace::kTotalLimit - unix_namespace->queued);
    if (sender->type != SOCK_STREAM && data.size() + overhead > UnixNamespace::kQueueLimit) return -EMSGSIZE;
    if (room <= overhead) return -EAGAIN;
    if (data.size() > room - overhead) {
        if (sender->type != SOCK_STREAM) return -EAGAIN;
        data.resize(room - overhead);
        data.shrink_to_fit();
    }
    size_t bytes = data.size(), charge = bytes + overhead;
    if (!unix_namespace->Charge(charge)) return -EAGAIN;
    UnixMessage message;
    message.data = std::move(data); message.rights = std::move(rights); message.charge = charge;
    message.source = sender->address; message.pid = lock_owner; message.uid = CurrentCredentials().uid; message.gid = CurrentCredentials().gid;
    receiver->queued += charge; receiver->messages.push_back(std::move(message));
    unix_namespace->dirty = true;
    return bytes;
}
namespace {
std::shared_ptr<OpenFile> SocketFile(const std::shared_ptr<UnixEndpoint>& endpoint, int flags = 0) {
    auto file = std::make_shared<OpenFile>(); file->unix_socket = endpoint;
    file->socket_domain = AF_UNIX; file->pollable = true;
    file->flags = O_RDWR | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0); return file;
}
std::string PathKey(const struct stat& st) { return "p" + std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino); }
}
std::optional<long> FileTable::Unix(const SyscallRequest& req, const GuestMemory& mem) {
    const auto& a = req.args;
    if (req.nr == __NR_socket || req.nr == __NR_socketpair) {
        if (a[0] != AF_UNIX) return req.nr == __NR_socketpair ? std::optional<long>(-EAFNOSUPPORT) : std::nullopt;
        int type = a[1] & ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET) return -ESOCKTNOSUPPORT;
        if (a[2]) return -EPROTONOSUPPORT;
        auto endpoint = unix_namespace->Create(type, lock_owner);
        if (req.nr == __NR_socket) return Install(SocketFile(endpoint, a[1]), a[1] & SOCK_CLOEXEC);
        int pair[2]{};
        if (!mem.Read(a[3], pair, sizeof(pair)) || !mem.Write(a[3], pair, sizeof(pair))) return -EFAULT;
        auto other = unix_namespace->Create(type, lock_owner);
        endpoint->peer = other; other->peer = endpoint;
        endpoint->connected = other->connected = true; endpoint->peer_pid = other->peer_pid = lock_owner;
        endpoint->peer_uid = other->peer_uid = CurrentCredentials().euid; endpoint->peer_gid = other->peer_gid = CurrentCredentials().egid;
        pair[0] = Install(SocketFile(endpoint, a[1]), a[1] & SOCK_CLOEXEC);
        pair[1] = Install(SocketFile(other, a[1]), a[1] & SOCK_CLOEXEC);
        if (pair[0] < 0 || pair[1] < 0 || !mem.Write(a[3], pair, sizeof(pair))) {
            for (int fd : pair) if (fd >= 0) descriptors_.erase(fd);
            return pair[0] < 0 || pair[1] < 0 ? -EMFILE : -EFAULT;
        }
        return 0;
    }
    if (req.nr < __NR_bind || (req.nr > __NR_recvmsg && req.nr != __NR_accept4)) return {};
    const auto* descriptor = Get(a[0]);
    if (!descriptor || !descriptor->file->unix_socket) return {};
    auto file = descriptor->file; auto endpoint = file->unix_socket;
    auto result = [&](long value, int flags = 0) -> long {
        return value == -EAGAIN && !(file->flags & O_NONBLOCK) && !(flags & MSG_DONTWAIT) ? kBlocked : value;
    };
    auto address = [&](uintptr_t pointer, size_t size, std::vector<uint8_t>* raw, std::string* key, bool bind) -> long {
        if (size < offsetof(sockaddr_un, sun_path) || size > sizeof(sockaddr_un)) return -EINVAL;
        raw->resize(size);
        if (!mem.Read(pointer, raw->data(), size)) return -EFAULT;
        sa_family_t family; memcpy(&family, raw->data(), sizeof(family));
        if (family == AF_UNSPEC && !bind) { key->clear(); return 0; }
        if (family != AF_UNIX) return -EAFNOSUPPORT;
        if (size == 2) {
            if (!bind) return -EINVAL;
            std::string name = "goblin-" + std::to_string(unix_namespace->next_name++);
            raw->push_back(0); raw->insert(raw->end(), name.begin(), name.end());
        }
        if (!(*raw)[2]) { key->assign("a"); key->append(reinterpret_cast<char*>(raw->data() + 2), raw->size() - 2); return 0; }
        size_t length = strnlen(reinterpret_cast<char*>(raw->data() + 2), raw->size() - 2);
        std::string path(reinterpret_cast<char*>(raw->data() + 2), length);
        raw->resize(length + 3); raw->back() = 0;
        if (path.empty()) return -EINVAL;
        if (path[0] != '/') {
            std::string base = cwd;
            if (cwd_handle) { int rc = vfs->PathFd(cwd_handle->fd, &base); if (rc < 0) return rc; }
            path = base + "/" + path;
        }
        int rc = ResolveSpecial(&path); if (rc < 0) return rc;
        struct stat st{};
        if (bind) {
            int fd = vfs->Open(path, O_RDWR | O_CREAT | O_EXCL, 0777 & ~mask);
            if (fd < 0) return fd == -EEXIST ? -EADDRINUSE : fd;
            auto inode = std::make_shared<HostFile>(fd);
            rc = SetSocketMetadata(fd);
            if (rc == 0) rc = vfs->StatFd(fd, &st);
            if (rc < 0) { vfs->Unlink(path, false); return rc; }
            endpoint->bound_inode = inode; vfs->Track(inode);
        } else {
            rc = vfs->Stat(path, &st); if (rc < 0) return rc;
            if (!S_ISSOCK(st.st_mode)) return -ECONNREFUSED;
            rc = CheckPermission(st,W_OK); if (rc < 0) return rc;
        }
        *key = PathKey(st); return 0;
    };
    auto put_address = [&](uintptr_t pointer, uintptr_t size_ptr, const std::vector<uint8_t>& raw) -> long {
        if (!pointer) return 0;
        socklen_t capacity, size = raw.size();
        if (!mem.Read(size_ptr, &capacity, sizeof(capacity))) return -EFAULT;
        return mem.Write(pointer, raw.data(), std::min<size_t>(capacity, raw.size())) && mem.Write(size_ptr, &size, sizeof(size)) ? 0 : -EFAULT;
    };
    switch (req.nr) {
        case __NR_bind: {
            if (endpoint->address.size() != 2) return -EINVAL;
            std::string key; std::vector<uint8_t> raw;
            long rc = address(a[1], a[2], &raw, &key, true); if (rc < 0) return rc;
            if (auto previous = unix_namespace->names[key].lock(); previous && !previous->collected) return -EADDRINUSE;
            unix_namespace->names[key] = endpoint; endpoint->address = std::move(raw); return 0;
        }
        case __NR_listen:
            if (endpoint->type == SOCK_DGRAM) return -EOPNOTSUPP;
            if (endpoint->connected || endpoint->address.size() == 2) return -EINVAL;
            endpoint->listening = true; endpoint->backlog = std::clamp<int>(a[1], 1, 128); endpoint->owner = lock_owner; endpoint->uid = CurrentCredentials().euid; endpoint->gid = CurrentCredentials().egid; return 0;
        case __NR_connect: {
            if (endpoint->connected && endpoint->type != SOCK_DGRAM) return -EISCONN;
            if (endpoint->listening) return -EINVAL;
            std::string key; std::vector<uint8_t> raw;
            long rc = address(a[1], a[2], &raw, &key, false); if (rc < 0) return rc;
            if (key.empty()) { endpoint->peer.reset(); endpoint->connected = false; return 0; }
            auto found = unix_namespace->names.find(key);
            auto peer = found == unix_namespace->names.end() ? nullptr : found->second.lock();
            if (!peer || peer->collected) return -ECONNREFUSED;
            if (peer->type != endpoint->type) return -EPROTOTYPE;
            if (peer->type == SOCK_DGRAM) { endpoint->peer = peer; endpoint->connected = true; endpoint->peer_pid = peer->owner; endpoint->peer_uid = peer->uid; endpoint->peer_gid = peer->gid; return 0; }
            if (!peer->listening) return -ECONNREFUSED;
            if (peer->pending.size() >= size_t(peer->backlog)) return result(-EAGAIN);
            auto accepted = unix_namespace->Create(endpoint->type, peer->owner);
            accepted->passcred = peer->passcred; accepted->uid = peer->uid; accepted->gid = peer->gid;
            accepted->peer_uid = CurrentCredentials().euid; accepted->peer_gid = CurrentCredentials().egid;
            accepted->address = peer->address; accepted->peer = endpoint; endpoint->peer = accepted;
            accepted->connected = endpoint->connected = true;
            accepted->peer_pid = lock_owner; endpoint->peer_pid = peer->owner; endpoint->peer_uid = peer->uid; endpoint->peer_gid = peer->gid;
            peer->pending.push_back(accepted); return 0;
        }
        case __NR_accept: case __NR_accept4: {
            if (!endpoint->listening) return -EINVAL;
            int flags = req.nr == __NR_accept4 ? a[3] : 0;
            if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) return -EINVAL;
            if (endpoint->pending.empty()) return result(-EAGAIN);
            auto accepted = endpoint->pending.front(); auto peer = accepted->peer.lock();
            long rc = put_address(a[1], a[2], peer ? peer->address : std::vector<uint8_t>{1, 0}); if (rc < 0) return rc;
            int fd = Install(SocketFile(accepted, flags), flags & SOCK_CLOEXEC);
            if (fd >= 0) endpoint->pending.pop_front(); return fd;
        }
        case __NR_getsockname: return put_address(a[1], a[2], endpoint->address);
        case __NR_getpeername: {
            auto peer = endpoint->peer.lock(); if (!peer || !endpoint->connected) return -ENOTCONN;
            return put_address(a[1], a[2], peer->address);
        }
        case __NR_shutdown:
            if (a[1] > SHUT_RDWR) return -EINVAL;
            if (!endpoint->connected) return -ENOTCONN;
            if (a[1] == SHUT_RD || a[1] == SHUT_RDWR) { endpoint->read_closed = true; endpoint->Clear(); }
            if (a[1] == SHUT_WR || a[1] == SHUT_RDWR) endpoint->write_closed = true;
            return 0;
        case __NR_setsockopt: case __NR_getsockopt: {
            if (a[1] != SOL_SOCKET) return -ENOPROTOOPT;
            if (req.nr == __NR_setsockopt) {
                int value; if (a[4] < sizeof(value)) return -EINVAL;
                if (!mem.Read(a[3], &value, sizeof(value))) return -EFAULT;
                if (a[2] == SO_PASSCRED) endpoint->passcred = value;
                else if (a[2] != SO_SNDBUF && a[2] != SO_RCVBUF && a[2] != SO_REUSEADDR) return -ENOPROTOOPT;
                return 0;
            }
            int value = 0; struct ucred cred{endpoint->peer_pid, endpoint->peer_uid, endpoint->peer_gid};
            const void* payload = &value; socklen_t size = sizeof(value), capacity;
            switch (a[2]) {
                case SO_TYPE: value = endpoint->type; break;
                case SO_ERROR: break;
                case SO_PASSCRED: value = endpoint->passcred; break;
                case SO_ACCEPTCONN: value = endpoint->listening; break;
                case SO_SNDBUF: case SO_RCVBUF: value = UnixNamespace::kQueueLimit; break;
                case SO_PEERCRED: if (!endpoint->connected) return -ENOTCONN; payload = &cred; size = sizeof(cred); break;
                default: return -ENOPROTOOPT;
            }
            if (!mem.Read(a[4], &capacity, sizeof(capacity))) return -EFAULT;
            size = std::min(size, capacity);
            return mem.Write(a[3], payload, size) && mem.Write(a[4], &size, sizeof(size)) ? 0 : -EFAULT;
        }
    }
    bool sending = req.nr == __NR_sendto || req.nr == __NR_sendmsg;
    bool message_call = req.nr == __NR_sendmsg || req.nr == __NR_recvmsg;
    if (!sending && req.nr != __NR_recvfrom && req.nr != __NR_recvmsg) return -EOPNOTSUPP;
    int flags = a[message_call ? 2 : 3];
    if (!sending && (flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC | MSG_CMSG_CLOEXEC | MSG_WAITALL))) return -EOPNOTSUPP;
    msghdr header{}; std::vector<iovec> iov;
    uintptr_t name = 0, name_size_ptr = 0; size_t name_capacity = 0;
    std::vector<uint8_t> control;
    if (message_call) {
        if (!mem.Read(a[1], &header, sizeof(header))) return -EFAULT;
        if (header.msg_iovlen > 1024 || header.msg_controllen > 65536 || header.msg_namelen > sizeof(sockaddr_storage)) return -EINVAL;
        if (!sending && !mem.Write(a[1], &header, sizeof(header))) return -EFAULT;
        iov.resize(header.msg_iovlen);
        if (!mem.Read(reinterpret_cast<uintptr_t>(header.msg_iov), iov.data(), iov.size() * sizeof(iovec))) return -EFAULT;
        name = reinterpret_cast<uintptr_t>(header.msg_name); name_capacity = header.msg_namelen;
        control.resize(header.msg_controllen);
        if (!mem.Read(reinterpret_cast<uintptr_t>(header.msg_control), control.data(), control.size()) ||
            (!sending && !mem.Write(reinterpret_cast<uintptr_t>(header.msg_control), control.data(), control.size()))) return -EFAULT;
    } else {
        iov.push_back({reinterpret_cast<void*>(a[1]), a[2]}); name = a[4];
        if (sending) name_capacity = a[5]; else name_size_ptr = a[5];
    }
    size_t capacity = 0;
    for (const auto& part : iov) { if (part.iov_len > (1u << 20) - capacity) return -EMSGSIZE; capacity += part.iov_len; }
    std::vector<uint8_t> data(capacity); size_t copied = 0;
    for (const auto& part : iov) {
        if (!mem.Read(reinterpret_cast<uintptr_t>(part.iov_base), data.data() + copied, part.iov_len) ||
            (!sending && !mem.Write(reinterpret_cast<uintptr_t>(part.iov_base), data.data() + copied, part.iov_len))) return -EFAULT;
        copied += part.iov_len;
    }
    if (sending) {
        if (endpoint->listening) return -ENOTCONN;
        std::vector<std::shared_ptr<OpenFile>> rights;
        for (size_t at = 0; at + sizeof(cmsghdr) <= control.size();) {
            cmsghdr cmsg{}; memcpy(&cmsg, control.data() + at, sizeof(cmsg));
            if (cmsg.cmsg_len < CMSG_LEN(0) || cmsg.cmsg_len > control.size() - at) return -EINVAL;
            size_t length = cmsg.cmsg_len - CMSG_LEN(0); const auto* payload = control.data() + at + CMSG_LEN(0);
            if (cmsg.cmsg_level != SOL_SOCKET) return -EINVAL;
            if (cmsg.cmsg_type == SCM_RIGHTS) {
                if (length % sizeof(int) || rights.size() + length / sizeof(int) > 253) return -EINVAL;
                for (size_t i = 0; i < length; i += sizeof(int)) {
                    int fd; memcpy(&fd, payload + i, sizeof(fd));
                    const auto* d = Get(fd); if (!d) return -EBADF;
                    rights.push_back(d->file);
                }
            } else if (cmsg.cmsg_type == SCM_CREDENTIALS) {
                struct ucred credentials{};
                if (length != sizeof(credentials)) return -EINVAL;
                memcpy(&credentials, payload, length);
                if (credentials.pid != lock_owner || credentials.uid != CurrentCredentials().uid || credentials.gid != CurrentCredentials().gid) return -EPERM;
            } else return -EINVAL;
            at += CMSG_ALIGN(cmsg.cmsg_len);
        }
        auto peer = endpoint->peer.lock();
        if (name) {
            if (endpoint->type != SOCK_DGRAM) return -EISCONN;
            std::vector<uint8_t> raw; std::string key;
            long rc = address(name, name_capacity, &raw, &key, false); if (rc < 0) return rc;
            auto found = unix_namespace->names.find(key);
            peer = found == unix_namespace->names.end() ? nullptr : found->second.lock();
        } else if (!endpoint->connected) return -ENOTCONN;
        return result(UnixSend(endpoint, peer, std::move(data), std::move(rights), flags), flags);
    }
    if (endpoint->listening) return -EINVAL;
    auto peer = endpoint->peer.lock();
    if (endpoint->messages.empty()) {
        if (endpoint->read_closed || (endpoint->type != SOCK_DGRAM && endpoint->connected && (!peer || peer->write_closed || peer->collected))) return 0;
        if (endpoint->type != SOCK_DGRAM && !endpoint->connected) return -ENOTCONN;
        return result(-EAGAIN, flags);
    }
    if (name && !message_call) {
        socklen_t size; if (!mem.Read(name_size_ptr, &size, sizeof(size)) || !mem.Write(name_size_ptr, &size, sizeof(size))) return -EFAULT;
        name_capacity = size;
    }
    if (name) {
        std::vector<uint8_t> probe(std::min<size_t>(name_capacity, sizeof(sockaddr_storage)));
        if (!mem.Read(name, probe.data(), probe.size()) || !mem.Write(name, probe.data(), probe.size())) return -EFAULT;
    }
    auto& message = endpoint->messages.front();
    size_t available = message.data.size() - message.offset, amount = std::min(capacity, available);
    if (endpoint->type == SOCK_STREAM && !capacity) return 0;
    if (endpoint->type == SOCK_STREAM && (flags & MSG_WAITALL)) {
        bool boundary=!message.rights.empty();
        available=0;
        for (const auto& part : endpoint->messages) {
            if (&part!=&message && (!part.rights.empty() || (endpoint->passcred &&
                (part.pid!=message.pid || part.uid!=message.uid || part.gid!=message.gid)))) { boundary=true; break; }
            available+=part.data.size()-part.offset;
            if (available>=capacity || boundary) break;
        }
        bool ended=endpoint->read_closed || !peer || peer->write_closed || peer->collected;
        if (available<capacity && !boundary && !ended && !(flags & MSG_DONTWAIT) && !(file->flags & O_NONBLOCK)) {
            // Retain bytes until the complete bounded read can be delivered.
            // Reject requests that cannot fit instead of deadlocking the sender
            // behind the namespace's fixed queue quota.
            if (capacity>UnixNamespace::kQueueLimit/2 || endpoint->queued+512>=UnixNamespace::kQueueLimit) return -EMSGSIZE;
            return kBlocked;
        }
        amount=std::min(capacity,available);
    }
    size_t gathered=0;
    for (const auto& part : endpoint->messages) {
        size_t bytes=std::min(amount-gathered,part.data.size()-part.offset);
        if (bytes) memcpy(data.data()+gathered,part.data.data()+part.offset,bytes);
        gathered+=bytes;
        if (gathered==amount) break;
    }
    copied = 0;
    for (const auto& part : iov) {
        size_t bytes = std::min(part.iov_len, amount - copied);
        if (bytes && !mem.Write(reinterpret_cast<uintptr_t>(part.iov_base), data.data() + copied, bytes)) return -EFAULT;
        copied += bytes; if (copied == amount) break;
    }
    int returned_flags = endpoint->type != SOCK_STREAM && amount < available ? MSG_TRUNC : 0;
    std::vector<uint8_t> returned_control;
    auto append_control = [&](int type, const void* payload, size_t size) {
        size_t at = returned_control.size(), length = CMSG_LEN(size), space = CMSG_SPACE(size);
        if (length > control.size() - at) { returned_flags |= MSG_CTRUNC; return false; }
        returned_control.resize(at + std::min(space, control.size() - at), 0);
        cmsghdr cmsg{}; cmsg.cmsg_len = length; cmsg.cmsg_level = SOL_SOCKET; cmsg.cmsg_type = type;
        memcpy(returned_control.data() + at, &cmsg, sizeof(cmsg));
        memcpy(returned_control.data() + at + CMSG_LEN(0), payload, size); return true;
    };
    if (endpoint->passcred && message_call) {
        struct ucred credentials{message.pid, message.uid, message.gid}; append_control(SCM_CREDENTIALS, &credentials, sizeof(credentials));
    }
    if (!message.rights.empty() && message_call) {
        size_t space = control.size() - returned_control.size();
        size_t count = space < CMSG_LEN(sizeof(int)) ? 0 : std::min(message.rights.size(), (space - CMSG_LEN(0)) / sizeof(int));
        std::vector<int> received;
        for (size_t i = 0; i < count; ++i) {
            int fd = Install(message.rights[i], flags & MSG_CMSG_CLOEXEC);
            if (fd < 0) break;
            received.push_back(fd);
        }
        if (received.size() < message.rights.size()) returned_flags |= MSG_CTRUNC;
        if (!received.empty()) append_control(SCM_RIGHTS, received.data(), received.size() * sizeof(int));
    }
    if (name && !mem.Write(name, message.source.data(), std::min(name_capacity, message.source.size()))) return -EFAULT;
    if (message_call) {
        header.msg_namelen = message.source.size(); header.msg_controllen = returned_control.size(); header.msg_flags = returned_flags;
        if (!mem.Write(reinterpret_cast<uintptr_t>(header.msg_control), returned_control.data(), returned_control.size()) || !mem.Write(a[1], &header, sizeof(header))) return -EFAULT;
    } else if (name) {
        socklen_t size = message.source.size(); if (!mem.Write(name_size_ptr, &size, sizeof(size))) return -EFAULT;
    }
    long value = (endpoint->type != SOCK_STREAM && (flags & MSG_TRUNC)) ? available : amount;
    if (!(flags & MSG_PEEK)) {
        message.rights.clear();
        if (endpoint->type != SOCK_STREAM) {
            unix_namespace->Release(message.charge); endpoint->queued -= message.charge; endpoint->messages.pop_front();
        } else {
            size_t remaining=amount;
            while (remaining) {
                auto& front=endpoint->messages.front();
                size_t bytes=std::min(remaining,front.data.size()-front.offset);
                front.offset+=bytes; remaining-=bytes;
                if (front.offset==front.data.size()) {
                    unix_namespace->Release(front.charge); endpoint->queued-=front.charge; endpoint->messages.pop_front();
                }
            }
        }
        unix_namespace->dirty = true;
    }
    return value;
}
}
