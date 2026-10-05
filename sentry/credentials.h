#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include <sys/stat.h>

namespace goblin {
class GuestMemory;
struct SyscallRequest;
struct Credentials {
    uint32_t uid = 0, euid = 0, suid = 0, fsuid = 0;
    uint32_t gid = 0, egid = 0, sgid = 0, fsgid = 0;
    std::vector<uint32_t> groups;
    bool no_new_privs = false;
    bool InGroup(uint32_t group) const;
    std::optional<long> Handle(const SyscallRequest&, const GuestMemory&);
};
// Syscalls are dispatched serially on the broker worker. Credentials belong
// to each guest task, never to the Android thread or a shared file table.
const Credentials& CurrentCredentials();
class CredentialScope {
public:
    explicit CredentialScope(const Credentials& value);
    ~CredentialScope();
private:
    const Credentials* previous_;
};
int CheckPermission(const struct stat& st, int mode);
}
