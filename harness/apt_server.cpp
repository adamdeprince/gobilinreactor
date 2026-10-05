#include "apt_server.h"
#include "vfs.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

AptServer::~AptServer() {
    stopped_ = true;
    if (worker_.joinable()) worker_.join();
    if (listener_ >= 0) close(listener_);
}
bool AptServer::Start(const std::string& root, std::string* error) {
    goblin::Vfs vfs;
    if (!vfs.Mount(root, error) || !vfs.ReadFile("/tmp/hello.deb", &package_, error) ||
        !vfs.ReadFile("/tmp/Packages", &index_, error)) return false;
    if (!Listen(error)) return false;
    std::string source = "deb [trusted=yes] http://localhost:" + std::to_string(port_) + " ./\n";
    int fd = vfs.Open("/etc/apt/sources.list", O_WRONLY | O_TRUNC);
    if (fd < 0) { *error = strerror(-fd); return false; }
    goblin::HostFile owner(fd);
    if (write(fd, source.data(), source.size()) != static_cast<ssize_t>(source.size())) { *error = "writing apt fixture source"; return false; }
    worker_ = std::thread([this] { Serve(); });
    return true;
}
bool AptServer::Listen(std::string* error) {
    listener_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener_ < 0) { *error = strerror(errno); return false; }
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    if (bind(listener_, reinterpret_cast<sockaddr*>(&address), size) < 0 || listen(listener_, 8) < 0 ||
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) < 0) { *error = strerror(errno); return false; }
    port_ = ntohs(address.sin_port); return true;
}
bool AptServer::StartInvalidRelease(const std::string& root, std::string* error) {
    goblin::Vfs vfs;
    if (!vfs.Mount(root, error) || !vfs.ReadFile("/var/lib/apt/lists/deb.debian.org_debian_dists_trixie_InRelease", &release_, error)) return false;
    std::string bytes(release_.begin(), release_.end());
    size_t origin = bytes.find("Origin: Debian");
    if (origin == std::string::npos) { *error = "missing signed Debian origin"; return false; }
    release_[origin + 8] ^= 1; // Change signed cleartext, preserving the actual signature.
    if (!Listen(error)) return false;
    std::string source = "deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] http://127.0.0.1:" + std::to_string(port_) + " trixie main\n";
    int fd = vfs.Open("/tmp/invalid-release.list", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { *error = strerror(-fd); return false; }
    goblin::HostFile owner(fd);
    if (write(fd, source.data(), source.size()) != static_cast<ssize_t>(source.size())) { *error = "writing apt fixture source"; return false; }
    worker_ = std::thread([this] { Serve(); });
    return true;
}
void AptServer::Serve() {
    while (!stopped_) {
        pollfd ready{listener_, POLLIN, 0};
        if (poll(&ready, 1, 100) <= 0) continue;
        int fd = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) continue;
        goblin::HostFile owner(fd);
        std::string request;
        for (unsigned tries = 0; tries < 50 && !stopped_ && request.size() < 8192; ++tries) {
            char buffer[1024];
            ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n > 0) { request.append(buffer, n); if (request.find("\r\n\r\n") != std::string::npos) break; }
            else if (n == 0 || (errno != EAGAIN && errno != EINTR)) break;
            else { pollfd wait{fd, POLLIN, 0}; poll(&wait, 1, 100); }
        }
        size_t begin = request.find(' '), end = request.find(' ', begin + 1);
        if (begin == std::string::npos || end == std::string::npos) continue;
        std::string path = request.substr(begin + 1, end - begin - 1);
        while (path.compare(0, 3, "/./") == 0) path.erase(0, 2);
        const auto* body = !release_.empty() ? (path == "/dists/trixie/InRelease" ? &release_ : nullptr)
            : path == "/Packages" ? &index_ : path == "/hello.deb" ? &package_ : nullptr;
        std::string response = body ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n";
        response += "Connection: close\r\nContent-Length: " + std::to_string(body ? body->size() : 0) + "\r\n\r\n";
        if (body) response.append(reinterpret_cast<const char*>(body->data()), body->size());
        size_t sent = 0;
        for (unsigned tries = 0; sent < response.size() && tries < 100 && !stopped_; ++tries) {
            ssize_t n = send(fd, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
            if (n > 0) sent += n;
            else if (errno == EAGAIN || errno == EINTR) { pollfd wait{fd, POLLOUT, 0}; poll(&wait, 1, 100); }
            else break;
        }
        if (body == &package_ && sent == response.size()) ++fetched_;
        if (body == &release_ && sent == response.size()) ++releases_;
    }
}
