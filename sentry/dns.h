#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace goblin {
// Guest 127.0.0.53:53 is translated to this private UDP endpoint. Android's
// resolver supplies the active network, VPN and Private DNS configuration.
class DnsProxy {
public:
    ~DnsProxy();
    bool Start(std::string* error);
    uint16_t port() const { return port_; }
    std::string Diagnostics() const;
private:
    void Serve();
    int socket_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<unsigned> queries_{0}, answers_{0}, failures_{0};
    std::atomic<int> last_error_{0};
    std::thread worker_;
};
}
