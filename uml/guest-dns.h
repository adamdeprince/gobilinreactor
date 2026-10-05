#pragma once
#include <map>
#include <vector>

// DNS stays on the guest's stable resolver address. Android resolves each raw
// query using its current default network, including VPN and Private DNS.
class GuestDns {
    struct Pending { int tcp; sockaddr_in peer{}; std::string query; };
    struct Connection { std::string input, output; size_t sent = 0; };
    int udp = -1, listener = -1;
    uint32_t next = 1;
    std::map<uint32_t, Pending> pending;
    std::map<int, Connection> connections;
    std::function<void(uint32_t, const std::string&)> send;
    void query(int tcp, const sockaddr_in& peer, std::string bytes) {
        if (bytes.size() < 12) return;
        uint32_t id = next++;
        while (!id || pending.count(id)) id = next++;
        pending.emplace(id, Pending{tcp, peer, bytes}); send(id, bytes);
    }
public:
    explicit GuestDns(std::function<void(uint32_t, const std::string&)> sender) : send(std::move(sender)) {
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(53);
        inet_pton(AF_INET, "10.0.2.3", &address.sin_addr);
        for (int kind : {SOCK_DGRAM, SOCK_STREAM}) {
            int fd = socket(AF_INET, kind | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
                (kind == SOCK_STREAM && listen(fd, SOMAXCONN) < 0)) {
                if (fd >= 0) close(fd);
                if (udp >= 0) { close(udp); udp = -1; }
                throw std::runtime_error("Cannot bind the Android DNS bridge");
            }
            if (kind == SOCK_DGRAM) udp = fd; else listener = fd;
        }
    }
    ~GuestDns() { close(udp); close(listener); for (auto& c : connections) close(c.first); }
    void answer(uint32_t id, std::string bytes) {
        auto p = pending.find(id); if (p == pending.end()) return;
        if (bytes.size() < 12 || bytes.size() > 65535) {
            bytes = p->second.query;
            bytes[2] = char((bytes[2] & 1) | 0x80); bytes[3] = char(0x82); // SERVFAIL
            std::fill(bytes.begin() + 6, bytes.begin() + 12, 0);
        }
        bytes[0] = p->second.query[0]; bytes[1] = p->second.query[1];
        if (p->second.tcp < 0) sendto(udp, bytes.data(), bytes.size(), 0,
            reinterpret_cast<sockaddr*>(&p->second.peer), sizeof(p->second.peer));
        else if (auto c = connections.find(p->second.tcp); c != connections.end()) {
            uint16_t size = htons(bytes.size());
            c->second.output.append(reinterpret_cast<char*>(&size), 2); c->second.output += bytes;
        }
        pending.erase(p);
    }
    void pollfds(std::vector<pollfd>& fds) {
        fds.push_back({udp, POLLIN, 0}); fds.push_back({listener, POLLIN, 0});
        for (auto& c : connections) fds.push_back({c.first, short(POLLIN | (c.second.sent < c.second.output.size() ? POLLOUT : 0)), 0});
    }
    void dispatch(const std::vector<pollfd>& fds, size_t start) {
        char data[65535];
        for (size_t i = start; i < fds.size(); ++i) {
            const auto& p = fds[i]; if (!p.revents) continue;
            if (p.fd == udp) {
                sockaddr_in peer{}; socklen_t size = sizeof(peer);
                ssize_t n = recvfrom(udp, data, sizeof(data), 0, reinterpret_cast<sockaddr*>(&peer), &size);
                if (n > 0) query(-1, peer, std::string(data, n));
            } else if (p.fd == listener) {
                int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (fd >= 0) connections.emplace(fd, Connection{});
            } else {
                auto c = connections.find(p.fd); if (c == connections.end()) continue;
                bool dead = p.revents & (POLLERR | POLLHUP);
                if (p.revents & POLLIN) {
                    ssize_t n = read(p.fd, data, sizeof(data));
                    if (n > 0) c->second.input.append(data, n);
                    else if (!n || (errno != EAGAIN && errno != EINTR)) dead = true;
                    auto& input = c->second.input;
                    while (input.size() >= 2) {
                        uint16_t count; memcpy(&count, input.data(), 2); count = ntohs(count);
                        if (input.size() < size_t(count) + 2) break;
                        query(p.fd, {}, input.substr(2, count)); input.erase(0, count + 2);
                    }
                }
                if (p.revents & POLLOUT) {
                    auto& stream = c->second;
                    ssize_t n = write(p.fd, stream.output.data() + stream.sent, stream.output.size() - stream.sent);
                    if (n > 0) stream.sent += n;
                    else if (n < 0 && errno != EAGAIN && errno != EINTR) dead = true;
                    if (stream.sent == stream.output.size()) { stream.output.clear(); stream.sent = 0; }
                }
                if (dead) {
                    for (auto it = pending.begin(); it != pending.end();) {
                        if (it->second.tcp == p.fd) it = pending.erase(it); else ++it;
                    }
                    close(p.fd); connections.erase(c);
                }
            }
        }
    }
};
