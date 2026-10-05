#pragma once
#include <cstdint>
#include "credentials.h"
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace goblin {
struct OpenFile;
struct ProcTask {
    Credentials credentials;
    int pid = 1, tgid = 1, ppid = 0, group = 1, session = 1, threads = 1, tty = 0, foreground = 0;
    char state = 'R';
    uint64_t bytes = 0, started = 0, blocked_signals = 0;
    std::string name, executable, cwd, maps, command, environment;
    std::vector<std::pair<int, std::shared_ptr<OpenFile>>> files;
};
struct ProcState {
    enum Detail { kBasic = 0, kFiles = 1, kMaps = 2, kArguments = 4 };
    std::function<std::vector<std::pair<int, int>>()> tasks; // (tid, tgid)
    std::function<bool(int, unsigned, ProcTask*)> task;
    uint64_t started_ns = 0, memory_limit = 0;
};
struct ProcHandle {
    struct Entry { std::string name; unsigned char type; uint64_t inode; };
    std::string path, data;
    std::vector<Entry> entries;
    uint64_t offset = 0, inode = 0;
    unsigned mode = 0;
    uint32_t uid = 0, gid = 0;
};
}
