#pragma once
#include <atomic>
#include <string>
#include <thread>
#include <vector>

// A loopback-only acceptance fixture. It serves two pinned byte arrays and
// records successful package fetches; it never resolves client-supplied paths.
class AptServer {
public:
    ~AptServer();
    bool Start(const std::string& root, std::string* error);
    bool StartInvalidRelease(const std::string& root, std::string* error);
    unsigned package_fetches() const { return fetched_; }
    unsigned release_fetches() const { return releases_; }
private:
    bool Listen(std::string* error);
    void Serve();
    int listener_ = -1;
    std::atomic<bool> stopped_{false};
    std::atomic<unsigned> fetched_{0};
    std::atomic<unsigned> releases_{0};
    unsigned port_ = 0;
    std::thread worker_;
    std::vector<uint8_t> package_, index_;
    std::vector<uint8_t> release_;
};
