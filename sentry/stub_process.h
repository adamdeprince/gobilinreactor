#pragma once
#include "channel.h"
#include "elf_loader.h"

namespace goblin {
// Owned by the broker. A pid is retained until waitpid reaps it; no guest
// message can name a host process or influence the process_vm target.
class StubProcess {
public:
    ~StubProcess();
    bool Start(const LoadedImage& image, Sentry* sentry, bool test_gate,
               const StubContext* resume, std::string* error);
    bool Audit(std::string* error) const;
    void Stop();
    pid_t pid = -1;
    int host_status = 0;
    SyscallChannel channel;
private:
    void* region_ = nullptr;
};
}
