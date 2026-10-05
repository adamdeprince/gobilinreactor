#include "archive.h"
#include "protocol.h"
#include "client.h"
#include "ports.h"
#include "../terminal/engine.h"
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <jni.h>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace goblin_uml;
void Check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message + ": " + strerror(errno));
}
struct Fd {
    int n = -1;
    Fd() = default;
    explicit Fd(int value) : n(value) { Check(n >= 0, "open descriptor"); }
    ~Fd() { if (n >= 0) close(n); }
    Fd(Fd&& other) noexcept : n(other.n) { other.n = -1; }
    Fd& operator=(Fd&& other) noexcept { if (n >= 0) close(n); n = other.n; other.n = -1; return *this; }
    Fd(const Fd&) = delete;
};
void Nonblock(int fd) { Check(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0, "nonblocking descriptor"); }
void Write(int fd, const void* data, size_t size) {
    auto* bytes = static_cast<const char*>(data);
    while (size) { ssize_t n = write(fd, bytes, size); if (n < 0 && errno == EINTR) continue; Check(n > 0, "write"); bytes += n; size -= n; }
}
bool Exists(const std::string& path) { struct stat st{}; return lstat(path.c_str(), &st) == 0; }
void Directory(const std::string& path) { Check(mkdir(path.c_str(), 0700) == 0 || errno == EEXIST, path); }
void File(const std::string& path, const std::string& text) {
    Fd out(open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600)); Write(out.n, text.data(), text.size());
}
std::string ReadText(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) return {};
    Fd input(fd); std::string result; char bytes[8192]; ssize_t n;
    while ((n = read(input.n, bytes, sizeof(bytes))) != 0) {
        if (n < 0 && errno == EINTR) continue;
        Check(n > 0, "reading " + path); result.append(bytes, n);
    }
    return result;
}
void Asset(AAssetManager* assets, const std::string& name, const std::string& destination) {
    AAsset* asset = AAssetManager_open(assets, name.c_str(), AASSET_MODE_STREAMING);
    if (!asset) throw std::runtime_error("Missing APK asset: " + name);
    try {
        Fd out(open((destination + ".new").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600));
        char bytes[65536]; int n;
        while ((n = AAsset_read(asset, bytes, sizeof(bytes))) > 0) Write(out.n, bytes, n);
        Check(n == 0 && fsync(out.n) == 0, "extracting " + name);
        Check(rename((destination + ".new").c_str(), destination.c_str()) == 0, "publishing " + name);
    } catch (...) { AAsset_close(asset); throw; }
    AAsset_close(asset);
}
struct Buffer {
    std::string bytes; size_t at = 0;
    bool empty() const { return at == bytes.size(); }
    void append(const void* data, size_t size) { bytes.append(static_cast<const char*>(data), size); }
    bool flush(int fd) {
        if (empty()) return true;
        ssize_t n = write(fd, bytes.data() + at, bytes.size() - at);
        if (n > 0) at += n;
        else if (n < 0 && errno != EAGAIN && errno != EINTR) return false;
        if (empty()) { bytes.clear(); at = 0; }
        return true;
    }
    void message(uint32_t op, uint32_t id, const void* data = nullptr, size_t size = 0) {
        Header h{kMagic, op, id, uint32_t(size)}; append(&h, sizeof(h)); if (size) append(data, size);
    }
};
struct Terminal {
    int id, pid = 0, state = 0;
    std::string user, input;
    bool create = false, opened = false, close = false, close_sent = false, resized = true;
    uint32_t rows = 24, columns = 80;
};
struct Runtime {
    std::mutex mutex;
    std::atomic<bool> running{true}, stop{false};
    std::string state = "Preparing environment", data, library, diagnostics;
    std::map<int, std::shared_ptr<Terminal>> terminals;
    int selected = 0, next = 1, test_mode = -1;
    Fd wake{eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    uint64_t generation = 0;
    jclass dns_class = nullptr;
    jmethodID dns_resolve = nullptr;
    jclass managed_class = nullptr;
    jmethodID managed_start = nullptr, managed_stop = nullptr;
    std::vector<std::pair<uint32_t, std::string>> dns_answers;
    bool rescue = false;
    void notify() { uint64_t one = 1; ssize_t ignored = write(wake.n, &one, sizeof(one)); (void)ignored; }
};
std::mutex current_mutex;
std::shared_ptr<Runtime> current;
uint64_t generation = 0;
std::shared_ptr<Runtime> Current() { std::lock_guard<std::mutex> lock(current_mutex); return current; }
std::string JavaString(JNIEnv* env, jstring value) {
    const char* bytes = env->GetStringUTFChars(value, nullptr); std::string text(bytes); env->ReleaseStringUTFChars(value, bytes); return text;
}
void JavaCheck(JNIEnv* env) {
    if (!env->ExceptionCheck()) return;
    jthrowable error = env->ExceptionOccurred(); env->ExceptionClear();
    jclass type = env->GetObjectClass(error);
    jmethodID describe = env->GetMethodID(type, "toString", "()Ljava/lang/String;");
    auto value = static_cast<jstring>(env->CallObjectMethod(error, describe));
    std::string message = value ? JavaString(env, value) : "Android environment host failed";
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (value) env->DeleteLocalRef(value);
    env->DeleteLocalRef(type); env->DeleteLocalRef(error);
    throw std::runtime_error(message);
}
int StartManaged(JNIEnv* env, const std::shared_ptr<Runtime>& r, bool kernel,
                 const std::vector<std::string>& args, const std::map<int, int>& files, int status) {
    Check(env->PushLocalFrame(16) == JNI_OK, "Android descriptor handoff");
    struct Frame { JNIEnv* env; ~Frame() { env->PopLocalFrame(nullptr); } } frame{env};
    jclass string = env->FindClass("java/lang/String");
    jobjectArray arguments = env->NewObjectArray(args.size(), string, nullptr);
    for (size_t i = 0; i < args.size(); ++i) {
        jstring value = env->NewStringUTF(args[i].c_str()); env->SetObjectArrayElement(arguments, i, value); env->DeleteLocalRef(value);
    }
    std::vector<jint> sources, targets;
    for (auto item : files) { targets.push_back(item.first); sources.push_back(item.second); }
    jintArray src = env->NewIntArray(sources.size()), dst = env->NewIntArray(targets.size());
    env->SetIntArrayRegion(src, 0, sources.size(), sources.data()); env->SetIntArrayRegion(dst, 0, targets.size(), targets.data());
    JavaCheck(env);
    int pid = env->CallStaticIntMethod(r->managed_class, r->managed_start, jboolean(kernel), arguments, src, dst, jint(status));
    JavaCheck(env); Check(pid > 0, "start Android environment host"); return pid;
}
void StopManaged(JNIEnv* env, const std::shared_ptr<Runtime>& r, bool kernel) {
    env->CallStaticVoidMethod(r->managed_class, r->managed_stop, jboolean(kernel)); JavaCheck(env);
}
// The managed host owns waitpid. Its pipe closes even if Android kills the host.
bool HostExited(int fd, int* status) {
    ssize_t n; do { n = read(fd, status, sizeof(*status)); } while (n < 0 && errno == EINTR);
    if (n < 0 && errno == EAGAIN) return false;
    Check(n >= 0, "read environment host status");
    if (n != sizeof(*status)) *status = 127 << 8;
    return true;
}
void State(const std::shared_ptr<Runtime>& r, const std::string& state) { std::lock_guard<std::mutex> lock(r->mutex); r->state = state; }
void Feed(const std::shared_ptr<Runtime>& r, int id, const std::string& bytes) {
    if (r->test_mode < 0) GoblinKittyFeedSession(id, bytes.data(), bytes.size());
    else { std::lock_guard<std::mutex> lock(r->mutex); r->diagnostics += bytes; }
}
int NewTerminal(const std::shared_ptr<Runtime>& r, const std::string& user, bool create) {
    if (user.empty() || user.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos || (create && user == "root")) return 0;
    std::lock_guard<std::mutex> lock(r->mutex);
    if (!r->running || r->stop) return 0;
    auto session = std::make_shared<Terminal>(); session->id = r->next++; session->user = user; session->create = create;
    r->terminals[session->id] = session; r->selected = session->id;
    if (r->test_mode < 0) GoblinKittySelect(session->id);
    r->notify(); return session->id;
}
pid_t Spawn(const std::string& path, std::vector<std::string> arguments,
            const std::map<int, int>& descriptors, const std::string& temporary) {
    // Prepare allocations and descriptor copies before fork; the child only
    // performs async-signal-safe operations before executing its packaged ELF.
    std::vector<char*> argv; for (auto& arg : arguments) argv.push_back(arg.data()); argv.push_back(nullptr);
    std::vector<std::string> environment{"PATH=/system/bin", "TMPDIR=" + temporary, "HOME=" + temporary};
    std::vector<char*> env; for (auto& value : environment) env.push_back(value.data()); env.push_back(nullptr);
    std::vector<Fd> sources; std::vector<int> targets; int last = 2;
    for (const auto& item : descriptors) { sources.emplace_back(fcntl(item.second, F_DUPFD_CLOEXEC, 64)); targets.push_back(item.first); last = std::max(last, item.first); }
    long descriptors_max = sysconf(_SC_OPEN_MAX);
    pid_t pid = fork(); Check(pid >= 0, "launch UML process");
    if (!pid) {
        setsid(); prctl(PR_SET_PDEATHSIG, SIGKILL); if (getppid() == 1) _exit(125);
        for (size_t i = 0; i < sources.size(); ++i) if (dup2(sources[i].n, targets[i]) < 0) _exit(126);
        for (int fd = last + 1; fd < descriptors_max; ++fd) close(fd);
        rlimit files{}; if (getrlimit(RLIMIT_NOFILE, &files) == 0) { files.rlim_cur = files.rlim_max; setrlimit(RLIMIT_NOFILE, &files); }
        execve(path.c_str(), argv.data(), env.data()); _exit(127);
    }
    return pid;
}
struct Client { Fd fd; std::string incoming; Buffer outgoing; uint32_t request = 0; bool complete = false; };
std::mutex ports_mutex;
std::string ApplyPorts(const std::shared_ptr<Runtime>& r, const std::string& text) {
    std::lock_guard<std::mutex> guard(ports_mutex);
    try {
        auto rules = PortArguments(text);
        const std::string directory = r->data + "/uml";
        std::vector<std::string> args{"pesto", "--clear", "HOST"};
        args.insert(args.end(), rules.begin(), rules.end()); args.push_back(directory + "/network.sock");
        int output[2]; Check(pipe2(output, O_CLOEXEC) == 0, "port configuration pipe");
        Fd reader(output[0]), writer(output[1]), null(open("/dev/null", O_RDONLY | O_CLOEXEC));
        pid_t child = Spawn(r->library + "/libgoblinuml-ports.so", args, {{0, null.n}, {1, writer.n}, {2, writer.n}}, directory + "/tmp");
        writer = Fd();
        std::string log; char bytes[8192]; ssize_t n;
        while ((n = read(reader.n, bytes, sizeof(bytes))) != 0) {
            if (n < 0 && errno == EINTR) continue;
            Check(n > 0, "read port configuration result"); log.append(bytes, n);
        }
        int status; while (waitpid(child, &status, 0) < 0) Check(errno == EINTR, "wait for port configuration");
        if (!WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error("Port configuration failed: " + log);
        File(directory + "/ports.conf.new", text);
        Fd saved(open((directory + "/ports.conf.new").c_str(), O_RDONLY | O_CLOEXEC)); Check(fsync(saved.n) == 0, "save ports");
        Check(rename((directory + "/ports.conf.new").c_str(), (directory + "/ports.conf").c_str()) == 0, "publish ports");
        Fd parent(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)); Check(fsync(parent.n) == 0, "commit port configuration");
        return {};
    } catch (const std::exception& error) { return error.what(); }
}
void Worker(std::shared_ptr<Runtime> r, JavaVM* vm, jobject reference) {
    sigset_t blocked; sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
    pid_t kernel = -1, network = -1;
    JNIEnv* env = nullptr; bool attached = vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
    std::string failure;
    try {
        if (!attached) throw std::runtime_error("Cannot attach environment setup worker");
        AAssetManager* assets = AAssetManager_fromJava(env, reference);
        const std::string directory = r->data + "/uml", share = directory + "/share", image = directory + "/rootfs.ext4";
        Directory(directory); Directory(share); Directory(directory + "/tmp");
        Fd lock(open((r->data + "/debian.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
        Check(flock(lock.n, LOCK_EX | LOCK_NB) == 0, "another environment runtime is active");
        // Refresh APK-owned boot components independently of deployment
        // configuration versions. The persistent guest disk is reused below.
        Asset(assets, "uml-initramfs.cpio.gz", directory + "/initramfs.cpio.gz");
        if (r->test_mode >= 0) Asset(assets, "uml-acceptance.sh", share + "/acceptance.sh");
        Directory(share + "/deployment");
        AAssetDir* deployment = AAssetManager_openDir(assets, "deployment");
        if (!deployment) throw std::runtime_error("Missing deployment assets");
        while (const char* name = AAssetDir_getNextFileName(deployment)) Asset(assets, "deployment/" + std::string(name), share + "/deployment/" + name);
        AAssetDir_close(deployment);
        bool fresh = !Exists(image); const std::string disk = fresh ? image + ".pending" : image;
        if (fresh) {
            State(r, "Migrating environment");
            Feed(r, 1, "Preparing environment and preserving your files…\r\n");
            if (Exists(r->data + "/debian")) {
                std::string error;
                if (!ExportLegacy(r->data + "/debian", share + "/import.pack", &error)) throw std::runtime_error("Environment migration: " + error);
            } else { unlink((share + "/import.pack").c_str()); Asset(assets, "debian.pack", share + "/debian.pack"); }
            File(share + "/new-root", "Create the pending filesystem; original files are retained.\n");
        } else unlink((share + "/new-root").c_str());
        struct statvfs space{}; Check(statvfs(directory.c_str(), &space) == 0, "phone filesystem capacity");
        const uint64_t capacity = uint64_t(space.f_blocks) * space.f_frsize;
        Fd disk_file(open(disk.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
        struct stat st{}; Check(fstat(disk_file.n, &st) == 0, "Environment disk image");
        // A sparse backing disk has the phone filesystem's capacity, with no
        // smaller Goblin quota. Android allocates blocks only as they are used.
        if (uint64_t(st.st_size) < capacity) {
            Check(ftruncate(disk_file.n, capacity) == 0, "grow environment backing disk");
            if (!fresh) File(share + "/grow-root", "Backing storage grew.\n");
        }
        std::string archive_error;
        if (!ExportLegacy(share, directory + "/boot-assets.pack", &archive_error, {"debian.pack", "import.pack"}))
            throw std::runtime_error("Preparing environment boot assets: " + archive_error);
        {
            Fd archive(open((directory + "/boot-assets.pack").c_str(), O_RDWR | O_CLOEXEC));
            struct stat info{}; Check(fstat(archive.n, &info) == 0, "boot archive size");
            Check(ftruncate(archive.n, (info.st_size + 511) & ~off_t(511)) == 0 && fsync(archive.n) == 0, "align boot archive");
        }
        Fd boot_assets(open((directory + "/boot-assets.pack").c_str(), O_RDONLY | O_CLOEXEC));
        Fd initrd(open((directory + "/initramfs.cpio.gz").c_str(), O_RDONLY | O_CLOEXEC));
        Fd seed;
        if (fresh) {
            // Stream the seed or legacy data from a separate block device. An
            // existing Linux tree may be much larger than available guest RAM.
            const std::string path = share + (Exists(share + "/import.pack") ? "/import.pack" : "/debian.pack");
            {
                Fd writable(open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC));
                struct stat info{}; Check(fstat(writable.n, &info) == 0, "seed archive size");
                Check(ftruncate(writable.n, (info.st_size + 511) & ~off_t(511)) == 0 && fsync(writable.n) == 0, "align seed archive");
            }
            seed = Fd(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        }
        int kernel_exit_pair[2], network_exit_pair[2];
        Check(pipe2(kernel_exit_pair, O_CLOEXEC) == 0, "kernel status pipe");
        Fd kernel_exit(kernel_exit_pair[0]), kernel_status(kernel_exit_pair[1]);
        Check(pipe2(network_exit_pair, O_CLOEXEC) == 0, "network status pipe");
        Fd network_exit(network_exit_pair[0]), network_status(network_exit_pair[1]);
        Nonblock(kernel_exit.n); Nonblock(network_exit.n);
        env->DeleteGlobalRef(reference); reference = nullptr;
        int channel_pair[2], packets_pair[2], passt_pair[2], logs_pair[2];
        Check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channel_pair) == 0, "terminal transport");
        Fd channel(channel_pair[0]), guest_channel(channel_pair[1]);
        Check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, packets_pair) == 0, "network packets");
        Fd packets(packets_pair[0]), guest_packets(packets_pair[1]);
        Check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, passt_pair) == 0, "network transport");
        Fd passt(passt_pair[0]), helper_packets(passt_pair[1]);
        Check(pipe2(logs_pair, O_CLOEXEC) == 0, "kernel log pipe"); Fd logs(logs_pair[0]), log_writer(logs_pair[1]);
        Fd null(open("/dev/null", O_RDWR | O_CLOEXEC));
        Fd log_file(open((directory + "/boot.log").c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600));
        std::vector<std::string> net_args{"passt", "--foreground", "--stderr", "--fd", "3", "--conf-path", directory + "/network.sock", "--address", "10.0.2.15", "--address", "fd00:676f:626c::15", "--netmask", "255.255.255.0", "--gateway", "10.0.2.2", "--gateway", "fe80::1", "--dns", "none", "--no-dhcp", "--no-dhcpv6", "--no-ra"};
        // Start the machine even if another Android app has since claimed a
        // saved port. Apply the saved forwarding rules once the helper is ready.
        auto port_args = PortArguments("");
        net_args.insert(net_args.end(), port_args.begin(), port_args.end());
        network = StartManaged(env, r, false, net_args, {{0, helper_packets.n}, {1, log_writer.n}}, network_status.n);
        network_status = Fd();
        const long physical_pages = sysconf(_SC_PHYS_PAGES), page_size = sysconf(_SC_PAGESIZE);
        Check(physical_pages > 0 && page_size > 0, "phone physical memory");
        const uint64_t memory = uint64_t(physical_pages) * uint64_t(page_size);
        const long cpus = sysconf(_SC_NPROCESSORS_CONF);
        Check(cpus > 0, "phone CPU count");
        std::vector<std::string> kernel_args{"linux", "mem=" + std::to_string(memory), "ncpus=" + std::to_string(cpus), "ubd0=fd:5", "initrd=fd:6", "ubd1r=fd:7",
            "stub_exe=" + r->library + "/libgoblinuml-stub.so", "con=null", "con0=null,fd:1", "ssl=null", "ssl0=fd:3,fd:3",
            "vec0:transport=fd,fd=4,mac=02:00:00:00:00:01", "seccomp=on", "root=/dev/ubda", "rw", "console=tty0", "umid=goblin", "uml_dir=none"};
        if (Exists(directory + "/rescue-requested")) kernel_args.push_back("goblin.rescue=1");
        if (fresh) kernel_args.push_back("ubd2r=fd:8");
        kernel = StartManaged(env, r, true, kernel_args, {{0, null.n}, {1, log_writer.n}, {2, log_writer.n}, {3, guest_channel.n}, {4, guest_packets.n}, {5, disk_file.n}, {6, initrd.n}, {7, boot_assets.n}, {8, fresh ? seed.n : null.n}, {9, lock.n}}, kernel_status.n);
        kernel_status = Fd();
        guest_channel = Fd(); guest_packets = Fd(); helper_packets = Fd(); log_writer = Fd();
        for (int fd : {channel.n, packets.n, passt.n, logs.n}) Nonblock(fd);
        Fd listener(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        const std::string control = directory + "/control.sock"; Check(control.size() < sizeof(address.sun_path), "control socket path");
        memcpy(address.sun_path, control.c_str(), control.size() + 1); unlink(control.c_str());
        Check(bind(listener.n, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && chmod(control.c_str(), 0600) == 0 && listen(listener.n, SOMAXCONN) == 0, "control socket");
        File(directory + "/ctl-path", r->library + "/libgoblinuml-ctl.so\n");
        Buffer outgoing, net_out; std::deque<std::string> packets_out;
        std::string incoming, net_in, test_report; std::vector<std::unique_ptr<Client>> clients;
        bool ready = false, stop_sent = false, testing = false, ports_applied = false; uint32_t request = 0x80000000u;
        constexpr uint32_t test_request = 0x7fffffffu;
        State(r, "Booting environment");
        auto report = [&](bool ok) {
            if (!ok) test_report += "\nGOBLIN FAIL\n";
            File(r->data + "/phase1-report.txt.new", test_report);
            Check(rename((r->data + "/phase1-report.txt.new").c_str(), (r->data + "/phase1-report.txt").c_str()) == 0, "test report");
            __android_log_print(ANDROID_LOG_INFO, "goblin-sentry", "GOBLIN COMPLETE");
        };
        for (;;) {
            if (ready && !ports_applied) {
                ports_applied = true;
                try {
                    auto saved = ReadText(directory + "/ports.conf");
                    if (!saved.empty()) {
                        auto error = ApplyPorts(r, saved);
                        if (!error.empty()) Feed(r, 1, "\r\nSaved port forwarding could not start: " + error + "\r\nUse Network access to retry or change the ports.\r\n");
                    }
                } catch (const std::exception& error) { Feed(r, 1, "\r\nPort forwarding: " + std::string(error.what()) + "\r\n"); }
            }
            if (r->stop && ready && !stop_sent) { outgoing.message(Shutdown, 0); stop_sent = true; }
            if (r->stop && !ready && !stop_sent) { StopManaged(env, r, true); stop_sent = true; }
            if (ready) {
                std::lock_guard<std::mutex> guard(r->mutex);
                for (auto& answer : r->dns_answers) outgoing.message(DnsAnswer, answer.first, answer.second.data(), answer.second.size());
                r->dns_answers.clear();
                for (auto& item : r->terminals) {
                    auto& terminal = *item.second;
                    if (terminal.state >= 2) continue;
                    if (!terminal.opened) { std::string data(1, terminal.create ? 1 : 0); data += terminal.user; outgoing.message(Open, terminal.id, data.data(), data.size()); terminal.opened = true; }
                    if (terminal.resized) { Size size{terminal.rows, terminal.columns}; outgoing.message(Resize, terminal.id, &size, sizeof(size)); terminal.resized = false; }
                    if (terminal.close && !terminal.close_sent) { outgoing.message(Close, terminal.id); terminal.close_sent = true; }
                    if (!terminal.input.empty()) { outgoing.message(Input, terminal.id, terminal.input.data(), terminal.input.size()); terminal.input.clear(); }
                    if (r->test_mode < 0) { char bytes[65536]; size_t n = GoblinKittyTakeSessionInput(terminal.id, bytes, sizeof(bytes)); if (n) outgoing.message(Input, terminal.id, bytes, n); }
                }
                if (r->test_mode >= 0 && !testing) {
                    std::string command = "/bin/sh /run/goblin-host/acceptance.sh " + std::to_string(r->test_mode);
                    outgoing.message(Execute, test_request, command.data(), command.size()); testing = true;
                }
            }
            std::vector<pollfd> pollers{{r->wake.n, POLLIN, 0}, {channel.n, short(POLLIN | (!outgoing.empty() ? POLLOUT : 0)), 0},
                {logs.n, POLLIN, 0}, {packets.n, short(POLLIN | (!packets_out.empty() ? POLLOUT : 0)), 0},
                {passt.n, short(POLLIN | (!net_out.empty() ? POLLOUT : 0)), 0}, {listener.n, POLLIN, 0}};
            for (auto& client : clients) pollers.push_back({client->fd.n, short(POLLIN | (!client->outgoing.empty() ? POLLOUT : 0)), 0});
            int n = poll(pollers.data(), pollers.size(), 20); if (n < 0 && errno == EINTR) continue; Check(n >= 0, "runtime poll");
            if (pollers[0].revents & POLLIN) { uint64_t value; ssize_t ignored = read(r->wake.n, &value, sizeof(value)); (void)ignored; }
            if (pollers[1].revents & POLLOUT) Check(outgoing.flush(channel.n), "write guest control");
            char bytes[65536]; ssize_t length;
            if (pollers[2].revents & POLLIN) {
                length = read(logs.n, bytes, sizeof(bytes)); if (length > 0) { Write(log_file.n, bytes, length); if (!ready) Feed(r, 1, std::string(bytes, length)); }
            }
            if (pollers[1].revents & POLLIN) { length = read(channel.n, bytes, sizeof(bytes)); if (length > 0) incoming.append(bytes, length); }
            while (incoming.size() >= sizeof(Header)) {
                Header h; memcpy(&h, incoming.data(), sizeof(h));
                if (h.magic != kMagic) { incoming.erase(0, 1); continue; }
                if (incoming.size() - sizeof(h) < h.size) break;
                std::string data = incoming.substr(sizeof(h), h.size); incoming.erase(0, sizeof(h) + h.size);
                if (h.operation == DnsQuery) {
                    jbyteArray query = env->NewByteArray(data.size());
                    env->SetByteArrayRegion(query, 0, data.size(), reinterpret_cast<const jbyte*>(data.data()));
                    env->CallStaticVoidMethod(r->dns_class, r->dns_resolve, jlong(r->generation), jint(h.id), query);
                    env->DeleteLocalRef(query);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); outgoing.message(DnsAnswer, h.id); }
                } else if (h.operation == Ready || h.operation == RescueReady) {
                    if (h.operation == RescueReady) {
                        std::lock_guard<std::mutex> guard(r->mutex); r->rescue = true;
                        for (auto& t : r->terminals) t.second->user = "root";
                    }
                    if (fresh && h.operation == Ready) {
                        Check(fsync(disk_file.n) == 0 && rename(disk.c_str(), image.c_str()) == 0, "commit migrated environment filesystem");
                        Fd parent(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
                        Check(fsync(parent.n) == 0, "commit migration directory"); fresh = false;
                        unlink((share + "/new-root").c_str()); unlink((share + "/import.pack").c_str());
                        Fd assets_dir(open(share.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
                        Check(fsync(assets_dir.n) == 0, "commit migration marker");
                    }
                    File(directory + "/kernel-release", data + "\n"); ready = true; State(r, "Running");
                    unlink((share + "/grow-root").c_str());
                    if (r->test_mode < 0) Feed(r, 1, "\033[2J\033[H");
                } else if (h.id == test_request) {
                    if (h.operation == Output) { test_report += data; Feed(r, 0, data); }
                    if (h.operation == Error || h.operation == Exited) {
                        int32_t status = -1; if (h.operation == Exited && data.size() == 4) memcpy(&status, data.data(), 4);
                        if (h.operation == Error) test_report += data;
                        report(!status && test_report.find("GOBLIN PASS") != std::string::npos); r->stop = true;
                    }
                } else if (h.id >= 0x80000000u) {
                    for (auto& client : clients) if (client->request == h.id) {
                        client->outgoing.message(h.operation, 0, data.data(), data.size());
                        if (h.operation == Exited || h.operation == Error) client->complete = true;
                        break;
                    }
                } else {
                    if (h.operation == Output || h.operation == Error) Feed(r, h.id, (h.operation == Error ? "\r\n" : "") + data);
                    std::lock_guard<std::mutex> guard(r->mutex); auto it = r->terminals.find(h.id);
                    if (it != r->terminals.end()) {
                        if (h.operation == Started && data.size() == 4) { memcpy(&it->second->pid, data.data(), 4); it->second->state = 1; }
                        else if (h.operation == Exited) it->second->state = 2;
                        else if (h.operation == Error) it->second->state = 3;
                    }
                }
            }
            if (pollers[3].revents & POLLIN) {
                length = read(packets.n, bytes, sizeof(bytes)); if (length > 0) { uint32_t size = htonl(length); net_out.append(&size, 4); net_out.append(bytes, length); }
            }
            if (pollers[4].revents & POLLOUT) Check(net_out.flush(passt.n), "network helper write");
            if (pollers[4].revents & POLLIN) { length = read(passt.n, bytes, sizeof(bytes)); if (length > 0) net_in.append(bytes, length); }
            while (net_in.size() >= 4) {
                uint32_t size; memcpy(&size, net_in.data(), 4); size = ntohl(size);
                if (size > 65535) throw std::runtime_error("Invalid Ethernet frame from network helper");
                if (net_in.size() - 4 < size) break;
                packets_out.push_back(net_in.substr(4, size)); net_in.erase(0, 4 + size);
            }
            if (pollers[3].revents & POLLOUT && !packets_out.empty()) {
                auto& packet = packets_out.front(); length = write(packets.n, packet.data(), packet.size());
                if (length == ssize_t(packet.size())) packets_out.pop_front();
                else if (length < 0 && errno != EAGAIN && errno != EINTR) throw std::runtime_error("Guest network disconnected");
            }
            for (size_t i = 0; i < clients.size(); ++i) {
                auto& client = *clients[i]; auto revents = pollers[6 + i].revents;
                if (revents & POLLOUT && !client.outgoing.flush(client.fd.n)) client.complete = true;
                if (revents & POLLIN) {
                    length = read(client.fd.n, bytes, sizeof(bytes));
                    if (length > 0) client.incoming.append(bytes, length);
                    else if (!length) { client.complete = true; client.outgoing = {}; }
                }
                if (!client.request && ready && client.incoming.size() >= sizeof(Header)) {
                    Header h; memcpy(&h, client.incoming.data(), sizeof(h));
                    if (h.magic != kMagic || (h.operation != ReadFile && h.operation != Execute && h.operation != ConfigurePorts)) { client.complete = true; continue; }
                    if (client.incoming.size() - sizeof(h) >= h.size) {
                        client.request = request++;
                        if (h.operation == ConfigurePorts) {
                            auto error = ApplyPorts(r, client.incoming.substr(sizeof(h), h.size));
                            if (!error.empty()) client.outgoing.message(Output, 0, error.data(), error.size());
                            int32_t code = error.empty() ? 0 : 1; client.outgoing.message(Exited, 0, &code, sizeof(code)); client.complete = true;
                        } else outgoing.message(h.operation, client.request, client.incoming.data() + sizeof(h), h.size);
                        client.incoming.clear();
                    }
                }
                if (revents & (POLLHUP | POLLERR)) { client.complete = true; client.outgoing = {}; }
            }
            clients.erase(std::remove_if(clients.begin(), clients.end(), [](const auto& c) { return c->complete && c->outgoing.empty(); }), clients.end());
            if (pollers[5].revents & POLLIN) {
                int fd = accept4(listener.n, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (fd >= 0) { auto client = std::make_unique<Client>(); client->fd = Fd(fd); clients.push_back(std::move(client)); }
            }
            int status;
            if (HostExited(network_exit.n, &status)) throw std::runtime_error("UML networking stopped; see uml/boot.log");
            if (HostExited(kernel_exit.n, &status)) {
                if (!stop_sent && (!ready || !WIFEXITED(status) || WEXITSTATUS(status))) throw std::runtime_error("Environment stopped; see uml/boot.log");
                break;
            }
        }
        unlink(control.c_str());
    } catch (const std::exception& error) { failure = error.what(); }
    if (attached) {
        for (bool is_kernel : {true, false}) if ((is_kernel ? kernel : network) > 0) {
            try { StopManaged(env, r, is_kernel); }
            catch (const std::exception& error) { if (failure.empty()) failure = error.what(); }
        }
        if (reference) env->DeleteGlobalRef(reference);
        if (r->dns_class) env->DeleteGlobalRef(r->dns_class);
        if (r->managed_class) env->DeleteGlobalRef(r->managed_class);
        vm->DetachCurrentThread();
    }
    if (!failure.empty()) {
        Feed(r, 1, "\r\n" + failure + "\r\n");
        __android_log_print(ANDROID_LOG_ERROR, "goblin-uml", "%s", failure.c_str());
        if (r->test_mode >= 0) { try { File(r->data + "/phase1-report.txt", failure + "\nGOBLIN FAIL\n"); } catch (...) {} }
    }
    { std::lock_guard<std::mutex> lock(r->mutex); r->state = failure.empty() ? "Stopped" : failure; for (auto& t : r->terminals) if (t.second->state < 2) t.second->state = failure.empty() ? 2 : 3; }
    r->running = false;
}
void Start(JNIEnv* env, jstring directory, jstring library, jobject assets, int test_mode) {
    std::lock_guard<std::mutex> guard(current_mutex);
    if (current && current->running) return;
    if (current && current->test_mode < 0) for (const auto& session : current->terminals) GoblinKittyClose(session.first);
    auto r = std::make_shared<Runtime>(); r->data = JavaString(env, directory); r->library = JavaString(env, library); r->test_mode = test_mode; current = r;
    r->generation = ++generation;
    jclass bridge = env->FindClass("dev/goblinreactor/sentry/NetworkBridge");
    r->dns_class = reinterpret_cast<jclass>(env->NewGlobalRef(bridge));
    r->dns_resolve = env->GetStaticMethodID(bridge, "resolve", "(JI[B)V"); env->DeleteLocalRef(bridge);
    bridge = env->FindClass("dev/goblinreactor/sentry/ManagedLinux");
    r->managed_class = reinterpret_cast<jclass>(env->NewGlobalRef(bridge));
    r->managed_start = env->GetStaticMethodID(bridge, "start", "(Z[Ljava/lang/String;[I[II)I");
    r->managed_stop = env->GetStaticMethodID(bridge, "stop", "(Z)V"); env->DeleteLocalRef(bridge);
    if (test_mode < 0) NewTerminal(r, "goblin", false);
    JavaVM* vm; env->GetJavaVM(&vm); jobject reference = env->NewGlobalRef(assets);
    std::thread(Worker, r, vm, reference).detach();
}

extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_NetworkBridge_answerNative(JNIEnv* env, jclass, jlong generation_id, jint id, jbyteArray response) {
    auto r = Current(); if (!r || r->generation != uint64_t(generation_id) || !r->running) return;
    std::string bytes(env->GetArrayLength(response), '\0');
    env->GetByteArrayRegion(response, 0, bytes.size(), reinterpret_cast<jbyte*>(bytes.data()));
    std::lock_guard<std::mutex> guard(r->mutex); r->dns_answers.emplace_back(uint32_t(id), std::move(bytes)); r->notify();
}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_goblinreactor_sentry_SessionService_rescueNative(JNIEnv*, jclass) {
    auto r = Current(); if (!r) return false; std::lock_guard<std::mutex> guard(r->mutex); return r->rescue;
}
extern "C" JNIEXPORT jstring JNICALL Java_dev_goblinreactor_sentry_NetworkSettings_applyNative(JNIEnv* env, jclass, jstring value) {
    auto r = Current(); auto error = r && r->running ? ApplyPorts(r, JavaString(env, value)) : "Start environment before changing port forwarding";
    return env->NewStringUTF(error.c_str());
}
}

extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_startNative(JNIEnv* e, jclass, jstring d, jstring l, jobject a) { Start(e, d, l, a, -1); }
extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_testsNative(JNIEnv* e, jclass, jstring d, jstring l, jobject a, jint mode) { Start(e, d, l, a, mode); }
extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_stopNative(JNIEnv*, jclass) { if (auto r = Current()) { r->stop = true; r->notify(); } }
extern "C" JNIEXPORT jboolean JNICALL Java_dev_goblinreactor_sentry_SessionService_runningNative(JNIEnv*, jclass) { auto r = Current(); return r && r->running; }
extern "C" JNIEXPORT jstring JNICALL Java_dev_goblinreactor_sentry_SessionService_stateNative(JNIEnv* env, jclass) { auto r = Current(); if (!r) return env->NewStringUTF("Stopped"); std::lock_guard<std::mutex> lock(r->mutex); return env->NewStringUTF(r->state.c_str()); }
extern "C" JNIEXPORT jint JNICALL Java_dev_goblinreactor_sentry_SessionService_selectedNative(JNIEnv*, jclass) { auto r = Current(); if (!r) return 0; std::lock_guard<std::mutex> lock(r->mutex); return r->selected; }
extern "C" JNIEXPORT jint JNICALL Java_dev_goblinreactor_sentry_SessionService_terminalStateNative(JNIEnv*, jclass, jint id) { auto r = Current(); if (!r) return 2; std::lock_guard<std::mutex> lock(r->mutex); auto it = r->terminals.find(id); return it == r->terminals.end() ? 2 : it->second->state; }
extern "C" JNIEXPORT jbyteArray JNICALL Java_dev_goblinreactor_sentry_SessionService_transcriptNative(JNIEnv* env, jclass) {
    auto r = Current(); std::string bytes;
    if (r) { std::lock_guard<std::mutex> lock(r->mutex); bytes = r->diagnostics; }
    jbyteArray result = env->NewByteArray(bytes.size()); if (result) env->SetByteArrayRegion(result, 0, bytes.size(), reinterpret_cast<const jbyte*>(bytes.data())); return result;
}
extern "C" JNIEXPORT jbyteArray JNICALL Java_dev_goblinreactor_sentry_SessionService_readGuestNative(JNIEnv* env, jclass, jstring path) {
    std::string bytes;
    try { if (auto r = Current()) Request(r->data + "/uml/control.sock", ReadFile, JavaString(env, path), [&](const char* data, size_t n) { bytes.append(data, n); }); }
    catch (...) { bytes.clear(); }
    jbyteArray result = env->NewByteArray(bytes.size());
    if (result) env->SetByteArrayRegion(result, 0, bytes.size(), reinterpret_cast<const jbyte*>(bytes.data()));
    return result;
}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_goblinreactor_sentry_SessionService_inputNative(JNIEnv* env, jclass, jbyteArray value) {
    auto r = Current(); if (!r || !r->running) return false;
    std::lock_guard<std::mutex> lock(r->mutex); auto it = r->terminals.find(r->selected); if (it == r->terminals.end() || it->second->state >= 2) return false;
    std::string bytes(env->GetArrayLength(value), '\0'); env->GetByteArrayRegion(value, 0, bytes.size(), reinterpret_cast<jbyte*>(bytes.data())); it->second->input += bytes; r->notify(); return true;
}
extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_resizeNative(JNIEnv*, jclass, jint rows, jint columns) {
    auto r = Current(); if (!r) return; std::lock_guard<std::mutex> lock(r->mutex); auto it = r->terminals.find(r->selected);
    if (it != r->terminals.end()) { it->second->rows = std::clamp(rows, 1, 65535); it->second->columns = std::clamp(columns, 1, 65535); it->second->resized = true; r->notify(); }
}
extern "C" JNIEXPORT jstring JNICALL Java_dev_goblinreactor_sentry_SessionService_terminalsNative(JNIEnv* env, jclass) {
    auto r = Current(); std::ostringstream text;
    if (r) { std::lock_guard<std::mutex> lock(r->mutex); for (const auto& item : r->terminals) if (item.second->state < 2) text << item.first << '\t' << item.second->user << '\t' << item.second->pid << '\n'; }
    return env->NewStringUTF(text.str().c_str());
}
extern "C" JNIEXPORT jint JNICALL Java_dev_goblinreactor_sentry_SessionService_openTerminalNative(JNIEnv* env, jclass, jstring name, jboolean create) { auto r = Current(); return r ? NewTerminal(r, JavaString(env, name), create) : 0; }
extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_selectNative(JNIEnv*, jclass, jint id) {
    auto r = Current(); if (!r) return; std::lock_guard<std::mutex> lock(r->mutex); auto it = r->terminals.find(id); if (it != r->terminals.end() && it->second->state < 2) { r->selected = id; GoblinKittySelect(id); }
}
extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_closeTerminalNative(JNIEnv*, jclass, jint id) {
    auto r = Current(); if (!r) return; std::lock_guard<std::mutex> lock(r->mutex); auto it = r->terminals.find(id); if (it != r->terminals.end()) { it->second->close = true; r->notify(); }
}
extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_releaseTerminalNative(JNIEnv*, jclass, jint id) {
    auto r = Current(); if (!r) return; std::lock_guard<std::mutex> lock(r->mutex); auto it = r->terminals.find(id);
    if (it == r->terminals.end() || it->second->state >= 2) { GoblinKittyClose(id); r->terminals.erase(id); if (r->selected == id) r->selected = 0; }
}
