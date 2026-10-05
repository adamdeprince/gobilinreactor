#include "developer_tests.h"
#include "developer_pins.h"
#include "session.h"
#include "metric_report.h"
#include <chrono>
#include <sys/file.h>
#include <unistd.h>

bool RunDeveloperTests(const std::string& data, AAssetManager* assets,
                       const std::function<void(const std::string&)>& log, bool emacs_only) {
    goblin::HostFile lock(open((data + "/debian.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
    if (lock.fd < 0 || flock(lock.fd, LOCK_EX | LOCK_NB) < 0) { log("FAILED: persistent session already active"); return false; }
    const std::string root = data + "/debian";
    AAsset* seed = AAssetManager_open(assets, "debian.pack", AASSET_MODE_BUFFER);
    if (!seed) { log("FAILED: missing Debian seed"); return false; }
    std::string error;
    bool ok = goblin::EnsurePersistentRoot(static_cast<const uint8_t*>(AAsset_getBuffer(seed)), AAsset_getLength64(seed), root, &error);
    AAsset_close(seed);
    if (!ok) { log("FAILED: " + error); return false; }
    goblin::RunLimits limits; limits.wall_time_ms = 1200000;
    auto run = [&](const std::string& name, const std::string& command) {
        log("-- Developer: " + name + " --");
        std::string output, errors;
        auto start = std::chrono::steady_clock::now();
        auto result = goblin::RunInRoot(root, {"/bin/bash", "--noprofile", "--norc", "-ec", command}, limits, nullptr, false, &output, &errors);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        bool passed = result.exited && !result.status && result.error.empty();
        log(std::string(passed ? "PASS: " : "FAILED: ") + name + " status=" + std::to_string(result.status) + " elapsed_ms=" + std::to_string(ms) + " " + result.error);
        log(MetricReport(name,result));
        std::string all = output + errors;
        for (size_t at = 0; at < all.size();) {
            size_t end = all.find('\n', at); if (end == std::string::npos) end = all.size();
            log(all.substr(at, end - at)); at = end + 1;
        }
        return passed;
    };
    if (!run("Debian bootstrap", "bash /usr/local/sbin/goblin-bootstrap >/var/log/goblin-bootstrap.log 2>&1 || { cat /var/log/goblin-bootstrap.log; exit 1; }")) return false;
    if (emacs_only) {
        if (!run("install and repair emacs-nox", "{ apt-get -y --no-remove --fix-broken install && apt-get -y --no-remove install emacs-nox; } >/var/log/goblin-emacs-install.log 2>&1 || { cat /var/log/goblin-emacs-install.log; apt-cache policy emacs-common libgccjit0; exit 1; }; cat /var/log/goblin-emacs-install.log; test -z \"$(dpkg --audit)\"; dpkg-query -W emacs-nox")) return false;
        return run("Emacs batch evaluation and file permissions", R"SH(
work=$(mktemp -d /tmp/goblin-emacs.XXXXXX)
trap 'rm -rf "$work"' EXIT
export GOBLIN_EMACS_TEST="$work/compiled.el"
emacs --batch -Q --eval '(progn (princ (format "EMACS:%s\n" emacs-version)) (let ((file (getenv "GOBLIN_EMACS_TEST"))) (with-temp-file file (insert "(message \"Goblin Emacs works\")\n")) (set-file-modes file #o640 (quote nofollow)) (unless (= (file-modes file) #o640) (error "Wrong file permissions")) (byte-compile-file file)))'
test -s "$work/compiled.elc"
emacs --batch -Q -l "$work/compiled.elc"
test -z "$(dpkg --audit)"
df -h /
)SH");
    }
    if (!run("install developer tools", "apt-get -y --no-install-recommends install gcc make libc6-dev python3-venv procps less vim-tiny git ca-certificates >/var/log/goblin-developer-install.log 2>&1 || { cat /var/log/goblin-developer-install.log; exit 1; }; test -z \"$(dpkg --audit)\"; gcc --version | head -1; make --version | head -1; python3 --version")) return false;
    if (!run("ps shows live guest processes", R"SH(
sleep 30 & child=$!
trap 'kill "$child" 2>/dev/null || true; wait "$child" 2>/dev/null || true' EXIT
ps -p "$$,$child" -o pid=,ppid=,comm= > /tmp/goblin-ps-proof
cat /tmp/goblin-ps-proof
python3 -c 'import sys; p,c=map(int,sys.argv[1:]); rows=[line.split() for line in open("/tmp/goblin-ps-proof")]; assert any(int(r[0])==c and int(r[1])==p and r[2]=="sleep" for r in rows); assert any(int(r[0])==p and r[2]=="bash" for r in rows)' "$$" "$child"
)SH")) return false;
    const std::string work = "/tmp/goblin-developer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if (!run("pinned HTTPS clone", "mkdir '" + work + "'; git -c advice.detachedHead=false clone --depth 1 --branch '" + pin_tag + "' '" + pin_repository + "' '" + work + "/zlib'; cd '" + work + "/zlib'; test \"$(git rev-parse HEAD)\" = '" + pin_commit + "'; git log -1 --format='%H %s'")) return false;
    const std::string cd = "cd '" + work + "/zlib'; ";
    if (!run("GCC and Make build", cd + "./configure --static >configure.log 2>&1 && make -j1 >build.log 2>&1 || { cat configure.log build.log; exit 1; }; make check; ./example")) return false;
    if (!run("Python venv and hashed wheel", "cd '" + work + "'; python3 -m venv venv || { venv/bin/python -m ensurepip --upgrade --default-pip; exit 1; }; printf '%s\\n' 'idna @ " + pin_wheel + " --hash=sha256:" + pin_sha256 + "' >requirements.txt; venv/bin/python -m pip install --disable-pip-version-check --no-deps --require-hashes -r requirements.txt; venv/bin/python -c 'import idna,sys; assert sys.prefix != sys.base_prefix; assert idna.__version__ == \"3.10\"; assert idna.encode(\"b\u00fccher.example\") == b\"xn--bcher-kva.example\"; print(\"venv, wheel hash and Unicode IDNA passed\")'")) return false;
    return run("clean package audit and test cleanup", "test -z \"$(dpkg --audit)\"; rm -rf -- '" + work + "'");
}
