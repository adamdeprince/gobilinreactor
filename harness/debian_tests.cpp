#include "debian_tests.h"
#include "program.h"
#include "sentry.h"
#include "stub.h"
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <cerrno>
#include "guest_layout.h"
#include "apt_server.h"

namespace {
bool Run(const std::string& root, const std::vector<std::string>& argv,
         const std::string& output, int status, const std::function<void(const std::string&)>& log,
         const std::string& terminal_input = {}) {
    log("-- Debian: " + argv[0] + " --");
    goblin::Sentry sentry({});
    sentry.arguments = argv;
    sentry.files().executable = argv[0];
    std::string error;
    if (!sentry.files().vfs->Mount(root, &error)) { log("FAILED: " + error); return false; }
    if (!terminal_input.empty() && !sentry.files().UseTerminal(terminal_input, &error)) {
        log("FAILED: " + error); return false;
    }
    // The parent window is scratch space used to launch a new isolated child.
    if (!goblin::GuestWindow::Reset()) { log("FAILED: resetting guest window"); return false; }
    goblin::LoadedImage image;
    if (!goblin::LoadProgram(*sentry.files().vfs, argv[0], &image, &error)) {
        log("FAILED: " + error); return false;
    }
    const auto result = goblin::RunGuest(image, &sentry);
    const bool ok = result.exited && result.status == status && result.error.empty() &&
                    (terminal_input.empty() ? sentry.guest_stdout() == output : sentry.guest_stdout().find(output) != std::string::npos);
    log(std::string(ok ? "PASS: " : "FAILED: ") + argv[0] + " status=" +
        std::to_string(result.status) + " syscalls=" + std::to_string(result.syscalls) +
        " stdout=" + sentry.guest_stdout());
    if (!ok) {
        log("error: " + result.error + " stderr: " + sentry.guest_stderr());
        for (const auto& line : sentry.trace()) log("  " + line);
    }
    return ok;
}
}
bool RunDebianTests(const std::string& root, const std::function<void(const std::string&)>& log) {
    bool ok = Run(root, {"/bin/true"}, "", 0, log);
    ok = Run(root, {"/tmp/filemap"}, "file mapping coherence and faults ok\n", 0, log) && ok;
    ok = Run(root, {"/tmp/pthreads"}, "pthread TLS, locks, conditions, signals, preemption and robust recovery ok\n", 0, log) && ok;
    ok = Run(root, {"/tmp/pthreads", "exec"}, "thread exec ok\n", 0, log) && ok;
    ok = Run(root, {"/tmp/pthreads", "leader-exit"}, "leader exit ok\n", 37, log) && ok;
    ok = Run(root, {"/tmp/pthreads", "group-exit"}, "group exit ok\n", 19, log) && ok;
    ok = Run(root, {"/tmp/identity"}, "credentials, user permissions, ownership, procfs and peer identity ok\n", 0, log) && ok;
    ok = Run(root, {"/tmp/setid"}, "guest set-ID exec, secure environment, no_new_privs and write stripping ok\n", 0, log) && ok;
    ok = Run(root, {"/tmp/network"}, "TCP, UDP, socket vectors and localhost resolution ok\n", 0, log) && ok;
    {
        // Occupy the same abstract name in Android. The guest must bind its own
        // socket and exchange data without delivering anything to this socket.
        goblin::HostFile host(socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        sockaddr_un address{}; address.sun_family=AF_UNIX;
        memcpy(address.sun_path+1,"goblin-private",14);
        if(host.fd<0||bind(host.fd,reinterpret_cast<sockaddr*>(&address),17)<0) {
            log("FAILED: host Unix isolation fixture");ok=false;
        } else {
            ok = Run(root, {"/tmp/proc_ipc"}, "guest procfs, Unix sockets and descriptor passing ok\n", 0, log) && ok;
            char byte;
            if(recv(host.fd,&byte,1,MSG_DONTWAIT)!=-1||errno!=EAGAIN) {
                log("FAILED: guest Unix socket reached the Android namespace");ok=false;
            } else log("PASS: guest and Android abstract socket namespaces are isolated");
        }
    }
    ok = Run(root, {"/tmp/fdexec"}, "exec descriptors and signals ok\n", 0, log) && ok;
    ok = Run(root, {"/bin/echo", "Debian", "ARM64", "works"}, "Debian ARM64 works\n", 0, log) && ok;
    ok = Run(root, {"/bin/dash", "-c", "/bin/echo child; /bin/true; exit 7"}, "child\n", 7, log) && ok;
    ok = Run(root, {"/bin/dash", "-c", "printf 'pipeline works\\n' | /bin/cat | /bin/cat"}, "pipeline works\n", 0, log) && ok;
    ok = Run(root, {"/bin/bash", "--noprofile", "--norc", "-c", "printf 'bash works\\n' | /bin/cat; wait"}, "bash works\n", 0, log) && ok;
    ok = Run(root, {"/bin/bash", "--noprofile", "--norc", "-c", "trap 'echo signal caught' USR1; kill -USR1 $$; /bin/sleep 0.01; echo after signal"},
             "signal caught\nafter signal\n", 0, log) && ok;
    ok = Run(root, {"/bin/bash", "--noprofile", "--norc", "-i"}, "pty ready\r\n", 0, log,
             "printf 'pty ready\\n' | /bin/cat\nexit 0\n") && ok;
    ok = Run(root, {"/bin/bash", "--noprofile", "--norc", "-c",
        "printf 'parent\\n' >/tmp/redirect; (printf 'child\\n' >>/tmp/redirect); /bin/cat /tmp/redirect; printf '%s\\n' \"$HOME\""},
        "parent\nchild\n/root\n", 0, log) && ok;
    ok = Run(root, {"/bin/bash", "--noprofile", "--norc", "-c",
        "/bin/sleep 10 & p=$!; kill -TERM $p; wait $p; s=$?; echo signal-status:$s; test $s = 143"},
        "signal-status:143\n", 0, log) && ok;
    ok = Run(root, {"/bin/bash", "--noprofile", "--norc", "-c",
        "/bin/yes | /bin/head -n 3; wait"}, "y\ny\ny\n", 0, log) && ok;
    ok = Run(root, {"/bin/bash", "--noprofile", "--norc", "-c",
        "/usr/bin/dpkg -i /tmp/hello.deb >/tmp/dpkg.log 2>&1 || { /bin/cat /tmp/dpkg.log; exit 1; }; /usr/bin/hello"},
        "Hello, world!\n", 0, log) && ok;
    ok = Run(root, {"/bin/bash", "--noprofile", "--norc", "-c",
        "printf '#!/bin/sh -e\nprintf \"script:%%s\\\\n\" \"$1\"\n' >/tmp/shebang; chmod 755 /tmp/shebang; exec /tmp/shebang works"},
        "script:works\n", 0, log) && ok;
    AptServer repository;
    std::string error;
    if (!repository.Start(root, &error)) { log("FAILED: apt fixture server: " + error); return false; }
    ok = Run(root, {"/bin/bash", "--noprofile", "--norc", "-c",
        "/usr/bin/apt-get update >/tmp/apt.log 2>&1 && /usr/bin/apt-get --reinstall install -y hello >>/tmp/apt.log 2>&1 || { /bin/cat /tmp/apt.log; exit 1; }; /usr/bin/hello"},
        "Hello, world!\n", 0, log) && ok;
    if (!repository.package_fetches()) { log("FAILED: apt did not fetch the package over HTTP"); ok = false; }
    return ok;
}
