#pragma once
#include "guest_memory.h"
#include "vfs.h"
#include "procfs.h"
#include "unix_socket.h"
#include <climits>
#include <map>
#include <optional>

namespace goblin {
class DnsProxy;
struct SyscallRequest;
constexpr long kBlocked = LONG_MIN;

struct Terminal {
    std::weak_ptr<HostFile> master;
    std::string host_slave;
    int number = 0, foreground_group = 0, session = 0;
    uint32_t uid = 0, gid = 0;
    unsigned mode = 0600;
    bool locked = true;
};

struct OpenFile {
    enum Kind { kHost, kInput, kOutput, kError } kind = kHost;
    std::shared_ptr<HostFile> host;
    std::string path;
    int flags = 0;
    bool directory = false;
    bool pollable = false;
    bool pty_master = false;
    int socket_domain = 0;
    bool connecting = false;
    bool dns_forwarded = false;
    std::shared_ptr<Terminal> terminal;
    std::shared_ptr<ProcHandle> proc;
    uint64_t identity = 0;
    std::shared_ptr<UnixEndpoint> unix_socket;
};
struct Descriptor { std::shared_ptr<OpenFile> file; bool cloexec = false; };
struct Output { std::string out, err; };

class FileTable {
public:
    FileTable();
    std::optional<long> Handle(const SyscallRequest& request, const GuestMemory& memory);
    int Install(std::shared_ptr<OpenFile> file, bool cloexec = false, int minimum = 0);
    const Descriptor* Get(int fd) const;
    void CloseExec();
    void CloseRange(unsigned first, unsigned last, bool cloexec);
    void CloseAll() { descriptors_.clear(); unix_namespace->dirty = true; }
    int HostFd(int fd) const;
    int Path(int dirfd, uintptr_t address, const GuestMemory& memory, std::string* path, bool follow = true) const;
    int ResolveSpecial(std::string* path, bool follow = true) const;
    void ShareNamespace(const FileTable& other);
    bool UseTerminal(const std::string& input, std::string* error);
    std::shared_ptr<HostFile> terminal_transport;
    std::string terminal_input;
    std::shared_ptr<Terminal> controlling_terminal() const { return controlling_terminal_ && controlling_terminal_->session == session ? controlling_terminal_ : nullptr; }
    uint64_t output_limit = 16ull << 20;
    std::shared_ptr<Vfs> vfs;
    std::shared_ptr<DnsProxy> dns;
    std::shared_ptr<ProcState> proc = std::make_shared<ProcState>();
    std::shared_ptr<UnixNamespace> unix_namespace = std::make_shared<UnixNamespace>();
    std::vector<std::shared_ptr<OpenFile>> OpenFiles() const;
    short VirtualEvents(int fd, short events) const;
    std::shared_ptr<Output> output;
    std::string cwd = "/";
    std::shared_ptr<HostFile> cwd_handle;
    std::string executable = "/goblin-guest";
    unsigned mask = 0022;
    int lock_owner = 1;
    int current_tid = 1;
    // Virtual terminal state belongs to the guest session, never the Android app.
    int foreground_group = 1;
    int session = 1;
private:
    std::shared_ptr<OpenFile> NewTerminal(int* error);
    std::shared_ptr<Terminal> TerminalAt(const std::string& path) const;
    std::shared_ptr<std::map<int, std::weak_ptr<Terminal>>> terminals_;
    std::shared_ptr<Terminal> controlling_terminal_;
    long Read(int fd, uintptr_t buffer, size_t size, const GuestMemory& memory,
              bool positional = false, uint64_t offset = 0);
    long Write(int fd, uintptr_t buffer, size_t size, const GuestMemory& memory,
               bool positional = false, uint64_t offset = 0);
    long Ioctl(int fd, unsigned long command, uintptr_t argument, const GuestMemory& memory);
    std::optional<long> Network(const SyscallRequest& request, const GuestMemory& memory);
    std::optional<long> Proc(const SyscallRequest& request, const GuestMemory& memory);
    std::optional<long> Unix(const SyscallRequest& request, const GuestMemory& memory);
    long UnixSend(const std::shared_ptr<UnixEndpoint>& sender, const std::shared_ptr<UnixEndpoint>& receiver,
                  std::vector<uint8_t> data, std::vector<std::shared_ptr<OpenFile>> rights, int flags);
    std::map<int, Descriptor> descriptors_;
};
}  // namespace goblin
