#include "dns.h"
#include <android/multinetwork.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace goblin {
DnsProxy::~DnsProxy() { stop_ = true; if (worker_.joinable()) worker_.join(); if (socket_ >= 0) close(socket_); }
std::string DnsProxy::Diagnostics() const {
    return "Android DNS: queries=" + std::to_string(queries_.load()) + " answers=" +
        std::to_string(answers_.load()) + " failures=" + std::to_string(failures_.load()) +
        " last_error=" + std::to_string(last_error_.load()) + "\n";
}
bool DnsProxy::Start(std::string* error) {
    socket_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (socket_ < 0) { *error = strerror(errno); return false; }
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(local);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0 ||
        getsockname(socket_, reinterpret_cast<sockaddr*>(&local), &size) < 0) { *error = strerror(errno); return false; }
    port_ = ntohs(local.sin_port);
    worker_ = std::thread([this] { Serve(); }); return true;
}
void DnsProxy::Serve() {
    using Clock = std::chrono::steady_clock;
    struct Query { int fd; sockaddr_in peer; std::vector<uint8_t> request; Clock::time_point deadline; };
    std::vector<Query> pending;
    auto failure = [&](const Query& q) {
        ++failures_;
        auto response = q.request;
        response[2] = 0x80 | (response[2] & 1); response[3] = 0x82; // QR, RD, RA, SERVFAIL.
        for (int i = 6; i < 12; ++i) response[i] = 0;
        sendto(socket_, response.data(), response.size(), MSG_DONTWAIT | MSG_NOSIGNAL,
               reinterpret_cast<const sockaddr*>(&q.peer), sizeof(q.peer));
    };
    while (!stop_) {
        std::vector<pollfd> waits{{socket_, POLLIN, 0}};
        for (const auto& q : pending) waits.push_back({q.fd, POLLIN, 0});
        if (poll(waits.data(), waits.size(), 50) < 0 && errno != EINTR) break;
        // Process old requests before appending new ones; poll indices remain
        // stable and both memory and resolver concurrency are explicitly bounded.
        for (size_t i = pending.size(); i-- > 0;) {
            auto& q = pending[i];
            if (waits[i + 1].revents) {
                std::array<uint8_t, 65536> answer{}; int rcode = 0;
                int n = android_res_nresult(q.fd, &rcode, answer.data(), answer.size()); // consumes fd
                if (n >= 12 && n <= 65507) {
                    ++answers_;
                    answer[0] = q.request[0]; answer[1] = q.request[1];
                    sendto(socket_, answer.data(), n, MSG_DONTWAIT | MSG_NOSIGNAL,
                           reinterpret_cast<const sockaddr*>(&q.peer), sizeof(q.peer));
                } else { last_error_ = n; failure(q); }
                pending.erase(pending.begin() + i);
            } else if (Clock::now() >= q.deadline) {
                last_error_ = -ETIMEDOUT;
                close(q.fd); failure(q); pending.erase(pending.begin() + i);
            }
        }
        if (waits[0].revents & POLLIN) {
            std::array<uint8_t, 4096> query{}; sockaddr_in peer{}; socklen_t size = sizeof(peer);
            ssize_t n = recvfrom(socket_, query.data(), query.size(), MSG_DONTWAIT | MSG_TRUNC,
                                 reinterpret_cast<sockaddr*>(&peer), &size);
            if (n < 12 || n > ssize_t(query.size()) || (query[2] & 0xf8) || query[4] || query[5] != 1) continue;
            ++queries_;
            Query q{-1, peer, {query.begin(), query.begin() + n}, Clock::now() + std::chrono::seconds(10)};
            if (pending.size() < 32) q.fd = android_res_nsend(NETWORK_UNSPECIFIED, query.data(), n, 0);
            if (q.fd < 0) { last_error_ = q.fd; failure(q); } else pending.push_back(std::move(q));
        }
    }
    for (const auto& q : pending) close(q.fd);
}
}
