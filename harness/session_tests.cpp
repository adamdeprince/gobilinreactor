#include "session_tests.h"
#include "session.h"
#include "deployment.h"
#include <chrono>
#include <thread>
#include <unistd.h>

bool RunSessionTests(const std::string& root, AAssetManager* assets, const std::function<void(const std::string&)>& log) {
    bool ok = true;
    auto check = [&](bool value, const std::string& name) { log(std::string(value ? "PASS: " : "FAILED: ") + name); ok &= value; };
    goblin::Vfs vfs; std::string error;
    if (!vfs.Mount(root, &error)) { check(false, error); return false; }
    AAsset* asset = AAssetManager_open(assets, "resources", AASSET_MODE_BUFFER);
    if (!asset) { check(false, "resource fixture asset"); return false; }
    int fd = vfs.Open("/tmp/resources", O_WRONLY | O_CREAT | O_TRUNC, 0755);
    bool installed = fd >= 0 && write(fd, AAsset_getBuffer(asset), AAsset_getLength64(asset)) == AAsset_getLength64(asset);
    if (fd >= 0) close(fd); AAsset_close(asset);
    if (!installed) { check(false, "resource fixture installation"); return false; }
    goblin::RunLimits limits;
    auto run = [&](const char* mode, const char* expected) {
        std::string output, errors;
        auto result = goblin::RunInRoot(root, {"/tmp/resources", mode}, limits, nullptr, false, &output, &errors);
        bool pass = result.exited && result.status == 0 && result.error.empty() && output == expected;
        check(pass, std::string("resource limit: ") + mode);
        if (!pass) log(result.error + " " + errors);
    };
    run("filesystem", "filesystem semantics verified\n");
    run("sigpipe", "SIGPIPE delivered once\n");
    limits.memory_bytes = 32u << 20; run("memory", "memory bounded\n");
    limits = {}; limits.tasks = 2; run("tasks", "tasks bounded\n");
    limits = {}; vfs.SetDiskLimit(limits.disk_bytes); limits.disk_bytes = vfs.disk_usage() + (64u << 10); run("disk", "disk bounded\n");
    limits = {}; limits.output_bytes = 16;
    std::string out, err;
    auto result = goblin::RunInRoot(root, {"/bin/echo", "012345678901234567890123456789"}, limits, nullptr, false, &out, &err);
    check(result.exited && result.status != 0 && out.size() <= 16, "captured output limit");
    limits = {}; limits.wall_time_ms = 200;
    result = goblin::RunInRoot(root, {"/tmp/resources", "spin"}, limits, nullptr, false, nullptr, nullptr);
    check(result.error == "guest exceeded its wall-time limit", "CPU loop stops at session deadline");

    goblin::SessionControl control;
    limits = {}; limits.wall_time_ms = 30000;
    goblin::RunResult shell;
    std::thread worker([&] { shell = goblin::RunInRoot(root, {"/bin/bash", "--noprofile", "--norc", "-i"}, limits, &control, true, nullptr, nullptr); });
    auto wait = [&](auto condition) {
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < end) {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    };
    control.Input("printf '\\122\\105\\101\\104\\131\\n'\n");
    bool ready = wait([&] { return control.Transcript().find("READY\r\n") != std::string::npos; });
    check(ready, "interactive shell input and output");
    if (ready) {
        control.Input("/tmp/resources job\n");
        // Observe output from the running program. tcsetpgrp happens earlier,
        // while Bash may still have the PTY in readline's non-ISIG mode.
        bool child = wait([&] { return control.foreground_group.load() != 1 && control.signal_keys.load() && control.Transcript().find("JOBREADY\r\n") != std::string::npos; });
        control.Input("\003");
        bool returned = wait([&] { return control.foreground_group.load() == 1; });
        control.Input("printf '\\111\\116\\124:%s\\n' \"$?\"\n");
        check(child && returned && wait([&] { return control.Transcript().find("INT:130\r\n") != std::string::npos; }), "Ctrl-C targets foreground job");
        size_t before_job = control.Transcript().size();
        control.Input("/tmp/resources job\n");
        child = wait([&] { return control.foreground_group.load() != 1 && control.signal_keys.load() && control.Transcript().find("JOBREADY\r\n", before_job) != std::string::npos; });
        control.Input("\032");
        returned = wait([&] { return control.foreground_group.load() == 1 && control.Transcript().find("Stopped") != std::string::npos; });
        control.Input("bg\nfg\n");
        bool resumed = wait([&] { return control.foreground_group.load() != 1 && control.signal_keys.load(); });
        control.Input("\003");
        check(child && returned && resumed && wait([&] { return control.foreground_group.load() == 1; }), "Ctrl-Z, background and foreground resume");
        control.Resize(31, 97);
        control.Input("stty size; printf '\\122\\105\\103\\117\\116\\116\\105\\103\\124\\n'\n");
        check(wait([&] { auto text = control.Transcript(); return text.find("31 97\r\n") != std::string::npos && text.find("RECONNECT\r\n") != std::string::npos; }), "terminal resize and transcript replay");
        control.Input("exit 0\n");
    } else control.stop = true;
    worker.join();
    check(shell.exited && !shell.status && shell.error.empty(), "terminal shell clean shutdown");
    if (!ok) log("Terminal transcript: " + control.Transcript());
    limits = {}; limits.wall_time_ms = 60000;
    if (!goblin::EnsureDefaultUser(root,limits,nullptr,&error)) { check(false,error); return false; }
    std::vector<uint8_t> passwd_before, passwd_after;
    if (!vfs.ReadFile("/etc/passwd",&passwd_before,&error)) { check(false,error); return false; }
    const std::string preserved="/home/goblin/.deploy-proof-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    { goblin::HostFile file(vfs.Open(preserved,O_WRONLY | O_CREAT | O_EXCL,0644));
      if (file.fd<0 || write(file.fd,"preserved",9)!=9) { check(false,"create deployment preservation fixture"); return false; } }
    if (!DeployGuestFeatures(root,assets,limits,nullptr,&error)) { check(false,error); return false; }
    check(DeployGuestFeatures(root,assets,limits,nullptr,&error),"packaged guest deployment is repeatable");
    std::vector<uint8_t> content;
    check(vfs.ReadFile("/etc/passwd",&passwd_after,&error) && passwd_before==passwd_after &&
          vfs.ReadFile(preserved,&content,&error) && std::string(content.begin(),content.end())=="preserved",
          "deployment preserves existing accounts and home files");
    vfs.Unlink(preserved,false);
    goblin::RuntimeControl runtime;
    auto first = runtime.Open("goblin");
    goblin::RunResult runtime_result; std::atomic<bool> runtime_finished{false};
    std::thread runtime_worker([&] {
        runtime_result = goblin::RunInRoot(root,{"/sbin/init"},limits,nullptr,false,nullptr,nullptr,&runtime);
        runtime_finished = true;
    });
    auto has = [&](const auto& session,const std::string& text) { return session->control->Transcript().find(text+"\r\n") != std::string::npos; };
    const std::string marker = "/tmp/goblin-runtime-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    first->control->Input("test $(id -un) = goblin && test \"$HOME:$PWD\" = /home/goblin:/home/goblin && SESSION_PROOF=first && printf '\\125\\123\\105\\122\\117\\113\n'\n");
    bool user = wait([&] { return has(first,"USEROK"); }); check(user,"runtime regular account and home directory");
    if (user) {
        first->control->Input("test $(sudo -n id -u) = 0 && test $(sudo -n -u nobody id -u) = 65534 && test $(id -u) != 0 && printf '\\123\\125\\104\\117\\117\\113\\n'\n");
        bool sudo_ok=wait([&] { return has(first,"SUDOOK"); });
        check(sudo_ok,"deployed passwordless sudo changes only the requested guest command identity");
        if (!sudo_ok) log(first->control->Transcript());
        first->control->Input("nohup sleep 120 </dev/null >"+marker+".log 2>&1 & echo $! >"+marker+"; disown; printf '\\123\\105\\122\\126\\105\\122\n'\n");
        bool server = wait([&] { return has(first,"SERVER"); });
        auto second = runtime.Open("goblin");
        second->control->Input("test -z \"$SESSION_PROOF\" && kill -0 $(cat "+marker+") && printf '\\123\\110\\101\\122\\105\\104\n'\n");
        check(server && wait([&] { return has(second,"SHARED"); }) && first->pid != second->pid,"independent terminal shells share the Linux process namespace");
        first->control->Input("exit\n"); second->control->Input("exit\n");
        check(wait([&] { return first->state == goblin::TerminalSession::kExited && second->state == goblin::TerminalSession::kExited; }) && !runtime_finished,
              "exiting all terminal shells leaves the Linux runtime alive");
        auto third = runtime.Open("goblin");
        third->control->Input("kill -0 $(cat "+marker+") && kill $(cat "+marker+") && rm -f "+marker+" "+marker+".log && printf '\\123\\125\\122\\126\\111\\126\\105\\104\n'\n");
        check(wait([&] { return has(third,"SURVIVED"); }),"detached Linux process survives with no terminals and remains manageable");
        third->control->stop = true;
        check(wait([&] { return third->state == goblin::TerminalSession::kExited; }) && !runtime_finished,"closing a terminal terminates its shell without stopping Linux");
    }
    runtime.stop = true; runtime_worker.join();
    check(runtime_result.error.empty(),"explicit runtime shutdown reaps its remaining processes");
    return ok;
}
