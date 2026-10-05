#include "session.h"
#include "guest_layout.h"
#include "program.h"
#include "vfs_metadata.h"
#include "dns.h"
#include "metrics.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <sys/mman.h>
#include <unistd.h>

namespace goblin {

bool ValidUsername(const std::string& name) {
    if (name.empty() || name.size() > 32 || (name[0] < 'a' || name[0] > 'z')) return false;
    return name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
}
bool FindSessionUser(const Vfs& vfs, const std::string& name, SessionUser* user, std::string* error) {
    if (!ValidUsername(name)) { *error = "Use a username starting with a lowercase letter (up to 32 letters, numbers, _ or -)."; return false; }
    std::vector<uint8_t> bytes;
    if (!vfs.ReadFile("/etc/passwd",&bytes,error)) return false;
    std::istringstream lines(std::string(bytes.begin(),bytes.end())); std::string line;
    auto split = [](const std::string& line) {
        std::vector<std::string> fields; size_t at = 0;
        for (;;) { size_t end = line.find(':',at); fields.push_back(line.substr(at,end == std::string::npos ? end : end-at)); if (end == std::string::npos) break; at = end+1; }
        return fields;
    };
    auto id = [](const std::string& text, uint32_t* value) {
        if (text.empty() || text.size() > 10 || text.find_first_not_of("0123456789") != std::string::npos) return false;
        uint64_t n = strtoull(text.c_str(),nullptr,10); if (n >= UINT32_MAX) return false; *value = n; return true;
    };
    bool found = false;
    while (std::getline(lines,line)) {
        auto fields = split(line); if (fields.size() != 7 || fields[0] != name) continue;
        uint32_t uid, gid;
        if (!id(fields[2],&uid) || !id(fields[3],&gid) || (name != "root" && uid < 1000) ||
            fields[5].empty() || fields[5][0] != '/' || fields[6] != "/bin/bash") {
            *error = "Select a regular account with /bin/bash as its shell, or root."; return false;
        }
        user->name = name; user->home = fields[5]; user->shell = fields[6];
        auto& c = user->credentials; c = {};
        c.uid = c.euid = c.suid = c.fsuid = uid; c.gid = c.egid = c.sgid = c.fsgid = gid;
        c.groups.push_back(gid); found = true; break;
    }
    if (!found) { *error = "Account '" + name + "' does not exist."; return false; }
    if (!vfs.ReadFile("/etc/group",&bytes,error)) return false;
    std::istringstream groups(std::string(bytes.begin(),bytes.end()));
    while (std::getline(groups,line)) {
        auto fields = split(line); uint32_t gid;
        if (fields.size() != 4 || !id(fields[2],&gid)) continue;
        std::string members = "," + fields[3] + ",";
        if (members.find("," + name + ",") != std::string::npos && !user->credentials.InGroup(gid)) user->credentials.groups.push_back(gid);
    }
    return true;
}
bool ConfigureSession(Sentry& sentry, const std::string& name, std::string* error) {
    SessionUser user;
    if (!FindSessionUser(*sentry.files().vfs,name,&user,error)) return false;
    sentry.credentials = user.credentials;
    CredentialScope scope(sentry.credentials);
    int fd = sentry.files().vfs->Open(user.home,O_PATH | O_DIRECTORY);
    if (fd < 0) { *error = user.home + ": " + strerror(-fd); return false; }
    auto home = std::make_shared<HostFile>(fd);
    int rc = sentry.files().vfs->AccessFd(fd,X_OK);
    if (rc < 0) { *error = user.home + ": " + strerror(-rc); return false; }
    sentry.files().cwd = user.home; sentry.files().cwd_handle = home;
    sentry.environment = {"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=" + user.home, "USER=" + user.name, "LOGNAME=" + user.name, "SHELL=" + user.shell,
        "PWD=" + user.home, "TERM=xterm-256color", "COLORTERM=truecolor", "LC_ALL=C.UTF-8",
        "PS1=\\u@goblin:\\w\\$ "};
    sentry.arguments = {user.shell,"--noprofile","-i"}; sentry.files().executable = user.shell;
    return true;
}
std::shared_ptr<TerminalSession> RuntimeControl::Open(const std::string& user, bool create) {
    if (!ValidUsername(user) || (create && user == "root")) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (retire) for (const auto& session : sessions_) if (session->state >= TerminalSession::kExited) retire(session->id);
    sessions_.erase(std::remove_if(sessions_.begin(),sessions_.end(),[](const auto& s) { return s->state >= TerminalSession::kExited; }),sessions_.end());
    if (sessions_.size() >= 8) return {};
    auto session = std::make_shared<TerminalSession>(); session->id = next_id_++; session->user = user; session->create_user = create;
    if (prepare) prepare(*session);
    sessions_.push_back(session); pending_.push_back(session); return session;
}
std::vector<std::shared_ptr<TerminalSession>> RuntimeControl::Sessions() const {
    std::lock_guard<std::mutex> lock(mutex_); return sessions_;
}
std::vector<std::shared_ptr<TerminalSession>> RuntimeControl::TakePending() {
    std::lock_guard<std::mutex> lock(mutex_); std::vector<std::shared_ptr<TerminalSession>> result; result.swap(pending_); return result;
}

bool SessionControl::Input(const std::string& bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes.size() > 65536 - input_.size()) return false;
    input_ += bytes; return true;
}
std::string SessionControl::TakeInput() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string value; value.swap(input_);
    if (terminal_input) value += terminal_input();
    return value;
}
void SessionControl::Append(const std::string& bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    transcript_ += bytes;
    if (transcript_.size() > (512u << 10)) transcript_.erase(0, transcript_.size() - (512u << 10));
    if (output_handler) output_handler(bytes);
}
std::string SessionControl::Transcript() const { std::lock_guard<std::mutex> lock(mutex_); return transcript_; }
void SessionControl::Resize(unsigned rows, unsigned columns) {
    std::lock_guard<std::mutex> lock(mutex_);
    rows_ = std::clamp(rows, 1u, 500u); columns_ = std::clamp(columns, 1u, 500u); resized_ = true;
}
bool SessionControl::TakeResize(unsigned* rows, unsigned* columns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!resized_) return false;
    *rows = rows_; *columns = columns_; resized_ = false; return true;
}
bool EnsurePersistentRoot(const uint8_t* seed, size_t size, const std::string& root, std::string* error) {
    struct stat existing{};
    if (lstat(root.c_str(), &existing) == 0) {
        if (!S_ISDIR(existing.st_mode)) { *error = "persistent root is not a directory"; return false; }
        Vfs vfs; return vfs.Mount(root, error);
    }
    if (errno != ENOENT) { *error = strerror(errno); return false; }
    if (!seed || !size) { *error = "Debian seed could not be read"; return false; }
    const std::string staging = root + ".seed";
    std::error_code ec;
    // This broker-owned staging path is never exposed to guest path traversal.
    std::filesystem::remove_all(staging, ec);
    if (ec) { *error = ec.message(); return false; }
    if (!InstallRootfs(seed, size, staging, error)) return false;
    // Persist contents before atomically publishing the initial root. A killed
    // extraction leaves only staging, which is rebuilt on the next attempt.
    std::vector<std::string> directories{staging};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(staging, ec)) {
        auto type = entry.symlink_status(ec).type();
        if (ec) { *error = ec.message(); return false; }
        if (type == std::filesystem::file_type::directory) directories.push_back(entry.path());
        else if (type == std::filesystem::file_type::regular) {
            HostFile fd(open(entry.path().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
            if (fd.fd < 0 || fsync(fd.fd) < 0) { *error = "syncing seed file"; return false; }
        }
    }
    for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
        HostFile fd(open(it->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (fd.fd < 0 || fsync(fd.fd) < 0) { *error = "syncing seed directory"; return false; }
    }
    if (rename(staging.c_str(), root.c_str()) < 0) { *error = "publishing persistent root"; return false; }
    HostFile parent(open(std::filesystem::path(root).parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (parent.fd < 0 || fsync(parent.fd) < 0) { *error = "syncing persistent root parent"; return false; }
    return true;
}
bool EnsureDefaultUser(const std::string& root, const RunLimits& limits, SessionControl* control, std::string* error) {
    Vfs vfs; SessionUser user;
    if (!vfs.Mount(root,error)) return false;
    if (FindSessionUser(vfs,"goblin",&user,error)) { error->clear(); return true; }
    std::string output, errors;
    auto result = RunInRoot(root,{"/usr/sbin/useradd","--create-home","--user-group","--shell","/bin/bash","-K","HOME_MODE=0700","--","goblin"},limits,control,false,&output,&errors);
    if (!result.exited || result.status || !result.error.empty()) {
        *error = "Creating goblin: " + result.error + "\n" + output + errors; return false;
    }
    error->clear(); return FindSessionUser(vfs,"goblin",&user,error);
}
RunResult RunInRoot(const std::string& root, const std::vector<std::string>& arguments,
                   const RunLimits& limits, SessionControl* control, bool terminal,
                   std::string* output, std::string* errors, RuntimeControl* runtime) {
    RunResult result;
    const uint64_t begin=MonotonicNanoseconds(), rss=BrokerResidentBytes();
    Sentry sentry({}); sentry.runtime = runtime;
    sentry.arguments = arguments; sentry.files().executable = arguments.at(0);
    sentry.environment.push_back("DEBIAN_FRONTEND=noninteractive");
    sentry.environment.push_back("PS1=goblin:\\w# ");
    if (terminal && control && control->output_handler) {
        for (auto& variable : sentry.environment) {
            if (variable.rfind("TERM=",0)==0) variable="TERM=xterm-256color";
            if (variable.rfind("LC_ALL=",0)==0) variable="LC_ALL=C.UTF-8";
        }
        sentry.environment.push_back("COLORTERM=truecolor");
    }
    sentry.limits = std::make_shared<RunLimits>(limits); sentry.control = control;
    if (!GuestWindow::Reserve(&result.error) || !sentry.files().vfs->Mount(root, &result.error)) return result;
    sentry.files().dns = std::make_shared<DnsProxy>();
    if (!sentry.files().dns->Start(&result.error)) return result;
    // Migrate only the old development seed's exact default. User-supplied
    // resolvers keep ordinary socket behavior and are not silently replaced.
    std::vector<uint8_t> resolver;
    std::string ignored;
    if (sentry.files().vfs->ReadFile("/etc/resolv.conf", &resolver, &ignored) &&
        std::string(resolver.begin(), resolver.end()) == "nameserver 1.1.1.1\nnameserver 8.8.8.8\noptions timeout:3 attempts:2\n") {
        HostFile fd(sentry.files().vfs->Open("/etc/resolv.conf", O_WRONLY | O_TRUNC));
        const std::string value = "nameserver 127.0.0.53\noptions timeout:5 attempts:2\n";
        if (fd.fd < 0 || write(fd.fd, value.data(), value.size()) != ssize_t(value.size())) { result.error = "configuring Android DNS bridge"; return result; }
    }
    if (terminal && !sentry.files().UseTerminal("", &result.error)) return result;
    if (!GuestWindow::Reset()) {
        result.error = "resetting guest launch window"; return result;
    }
    LoadedImage image;
    if (!runtime && !LoadProgram(*sentry.files().vfs, arguments[0], &image, &result.error, limits.memory_bytes)) return result;
    const uint64_t loaded=MonotonicNanoseconds(), staging_rss=BrokerResidentBytes();
    result = RunGuest(image, &sentry);
    result.wall_time_ns=MonotonicNanoseconds()-begin;
    result.startup_ns+=loaded-begin;
    result.broker_rss_start_bytes=rss;
    result.broker_rss_end_bytes=BrokerResidentBytes();
    result.broker_rss_sampled_peak_bytes=std::max({rss,staging_rss,result.broker_rss_end_bytes,result.broker_rss_sampled_peak_bytes});
    if (output) *output = sentry.guest_stdout();
    if (errors) {
        *errors = sentry.guest_stderr();
        if (!result.exited || result.status || !result.error.empty()) {
            *errors += sentry.files().dns->Diagnostics();
            *errors += "Recent syscall errors:\n";
            for (const auto& line : sentry.recent_errors()) *errors += line + "\n";
        }
    }
    return result;
}
}
