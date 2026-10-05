#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "stub.h"

namespace goblin {
struct RunLimits {
    uint32_t tasks = 64;
    uint64_t memory_bytes = 512ull << 20;
    uint64_t disk_bytes = 8ull << 30;
    uint64_t output_bytes = 16ull << 20;
    uint64_t wall_time_ms = 120000;
};

// The broker owns the PTY; activities only enqueue input and read a bounded
// transcript. Detaching a screen never closes the guest's terminal.
class SessionControl {
public:
    bool Input(const std::string& bytes);
    std::string TakeInput();
    void Append(const std::string& bytes);
    std::string Transcript() const;
    void Resize(unsigned rows, unsigned columns);
    bool TakeResize(unsigned* rows, unsigned* columns);
    std::atomic<bool> stop{false};
    std::atomic<int> foreground_group{1};
    std::atomic<bool> signal_keys{false};
    // Optional terminal backend, installed before the session worker starts.
    std::function<void(const std::string&)> output_handler;
    std::function<std::string()> terminal_input;
private:
    mutable std::mutex mutex_;
    std::string input_, transcript_;
    unsigned rows_ = 24, columns_ = 80;
    bool resized_ = true;
};

struct SessionUser {
    std::string name, home, shell;
    Credentials credentials;
};
bool FindSessionUser(const Vfs& vfs, const std::string& name, SessionUser* user, std::string* error);
bool ConfigureSession(Sentry& sentry, const std::string& name, std::string* error);
bool ValidUsername(const std::string& name);

struct TerminalSession {
    enum State { kQueued, kRunning, kExited, kFailed };
    int id = 0;
    std::string user;
    bool create_user = false;
    std::shared_ptr<SessionControl> control = std::make_shared<SessionControl>();
    std::atomic<int> state{kQueued}, pid{0}, exit_status{0};
};
// The Android service owns this runtime; screens only own terminal sessions.
// All task/VFS changes happen on the one broker scheduler thread.
class RuntimeControl {
public:
    std::shared_ptr<TerminalSession> Open(const std::string& user, bool create = false);
    std::vector<std::shared_ptr<TerminalSession>> Sessions() const;
    std::vector<std::shared_ptr<TerminalSession>> TakePending();
    std::atomic<bool> stop{false};
    std::function<void(TerminalSession&)> prepare;
    std::function<void(int)> retire;
private:
    mutable std::mutex mutex_;
    int next_id_ = 1;
    std::vector<std::shared_ptr<TerminalSession>> sessions_, pending_;
};

bool EnsureDefaultUser(const std::string& root, const RunLimits& limits, SessionControl* control, std::string* error);
bool EnsurePersistentRoot(const uint8_t* seed, size_t size, const std::string& root, std::string* error);
RunResult RunInRoot(const std::string& root, const std::vector<std::string>& arguments,
                    const RunLimits& limits, SessionControl* control,
                    bool terminal, std::string* output, std::string* errors,
                    RuntimeControl* runtime = nullptr);
}
