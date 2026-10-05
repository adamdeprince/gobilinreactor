#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace goblin {
struct OpenFile;
struct HostFile;
class UnixNamespace;
struct UnixMessage {
    std::vector<uint8_t> data, source;
    std::vector<std::shared_ptr<OpenFile>> rights;
    size_t offset = 0, charge = 0;
    int pid = 0;
    uint32_t uid = 0, gid = 0;
};
struct UnixEndpoint {
    explicit UnixEndpoint(int type) : type(type) {}
    ~UnixEndpoint();
    uint32_t uid = 0, gid = 0, peer_uid = 0, peer_gid = 0;
    int type, owner = 1, peer_pid = 0, backlog = 0;
    bool listening = false, connected = false, read_closed = false, write_closed = false, passcred = false, collected = false;
    std::vector<uint8_t> address{1, 0};
    std::weak_ptr<UnixEndpoint> peer;
    std::shared_ptr<HostFile> bound_inode;
    std::deque<std::shared_ptr<UnixEndpoint>> pending;
    std::deque<UnixMessage> messages;
    size_t queued = 0;
    std::weak_ptr<UnixNamespace> space;
    void Clear();
};
class UnixNamespace : public std::enable_shared_from_this<UnixNamespace> {
public:
    static constexpr size_t kQueueLimit = 256u << 10, kTotalLimit = 16u << 20;
    std::shared_ptr<UnixEndpoint> Create(int type, int pid);
    bool Charge(size_t bytes);
    void Release(size_t bytes);
    void Collect();
    std::function<std::vector<std::shared_ptr<OpenFile>>()> roots;
    std::map<std::string, std::weak_ptr<UnixEndpoint>> names;
    std::vector<std::weak_ptr<UnixEndpoint>> endpoints;
    uint64_t next_name = 1;
    size_t queued = 0;
    bool dirty = false;
};
}
