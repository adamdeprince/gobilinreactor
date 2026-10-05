#include "persistent_tests.h"
#include "session.h"
#include "apt_server.h"
#include "session_tests.h"
#include <android/asset_manager.h>
#include <sys/file.h>
#include <unistd.h>

bool RunPersistentTests(const std::string& data, AAssetManager* assets, const std::function<void(const std::string&)>& log) {
    goblin::HostFile lock(open((data + "/debian.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
    if (lock.fd < 0 || flock(lock.fd, LOCK_EX | LOCK_NB) < 0) { log("FAILED: persistent session already active"); return false; }
    const std::string root = data + "/debian";
    AAsset* seed = AAssetManager_open(assets, "debian.pack", AASSET_MODE_BUFFER);
    if (!seed) { log("FAILED: missing Debian bootstrap seed"); return false; }
    std::string error;
    bool installed = goblin::EnsurePersistentRoot(static_cast<const uint8_t*>(AAsset_getBuffer(seed)), AAsset_getLength64(seed), root, &error);
    AAsset_close(seed);
    if (!installed) { log("FAILED: persistent seed: " + error); return false; }
    goblin::RunLimits limits; limits.wall_time_ms = 1200000;
    auto run = [&](const char* name, const std::string& command) {
        log(std::string("-- Persistent: ") + name + " --");
        std::string output, errors;
        auto result = goblin::RunInRoot(root, {"/bin/bash", "--noprofile", "--norc", "-c", command}, limits, nullptr, false, &output, &errors);
        bool ok = result.exited && result.status == 0 && result.error.empty();
        log(std::string(ok ? "PASS: " : "FAILED: ") + name + " status=" + std::to_string(result.status) + " " + result.error);
        if (!ok) {
            // logcat has a per-entry size limit; retain each output line.
            std::string all = output + errors;
            for (size_t at = 0; at < all.size();) {
                size_t end = all.find('\n', at); if (end == std::string::npos) end = all.size();
                log(all.substr(at, end - at)); at = end + 1;
            }
        }
        return ok;
    };
    if (!run("Debian package bootstrap", "/bin/bash /usr/local/sbin/goblin-bootstrap >/var/log/goblin-bootstrap.log 2>&1 || { cat /var/log/goblin-bootstrap.log; exit 1; }")) return false;
    if (!run("recover interrupted package configuration", "dpkg --configure -a >/var/log/goblin-configure.log 2>&1 || { cat /var/log/goblin-configure.log; exit 1; }")) return false;
    if (!run("installed database and maintainer scripts", "test -z \"$(dpkg --audit)\" && dpkg-query -S /usr/bin/dpkg && test -s /etc/ssl/certs/ca-certificates.crt && test -s /var/lib/dpkg/info/libc6:arm64.list && printf 'persistent\\n' >/root/restart-proof")) return false;
    if (!run("remount persistence", "test \"$(cat /root/restart-proof)\" = persistent && test -f /var/lib/goblin/bootstrap-complete")) return false;
    if (!run("public DNS through Android", "exec getent ahostsv4 deb.debian.org")) return false;
    if (!run("public HTTPS repository and Debian signature", "apt-get -o APT::Update::Error-Mode=any update >/var/log/goblin-apt.log 2>&1 || { cat /var/log/goblin-apt.log; exit 1; }; test -s /var/lib/apt/lists/deb.debian.org_debian_dists_trixie_InRelease")) return false;
    {
        AptServer invalid;
        if (!invalid.StartInvalidRelease(root, &error)) { log("FAILED: invalid signature fixture: " + error); return false; }
        if (!run("reject modified repository signature", "mkdir -p /tmp/invalid-lists/partial; apt-get -o APT::Update::Error-Mode=any -o Dir::Etc::sourcelist=/tmp/invalid-release.list -o Dir::Etc::sourceparts=- -o Dir::State::lists=/tmp/invalid-lists update >/tmp/invalid-signature.log 2>&1; status=$?; cat /tmp/invalid-signature.log; test $status = 100 && grep -qiE 'signature|verification' /tmp/invalid-signature.log")) return false;
        if (!invalid.release_fetches()) { log("FAILED: invalid signature was not fetched"); return false; }
    }
    if (!run("install curl git python3 with public apt", "apt-get -y --no-install-recommends install curl git python3 >/var/log/goblin-install.log 2>&1 || { cat /var/log/goblin-install.log; exit 1; }; test -z \"$(dpkg --audit)\"")) return false;
    if (!run("curl DNS and trusted HTTPS", "curl --fail --silent --show-error --max-time 45 https://deb.debian.org/debian/README >/tmp/debian-readme && grep -q Debian /tmp/debian-readme")) return false;
    if (!run("reject untrusted HTTPS certificate", "mkdir -p /tmp/empty-cas && openssl req -x509 -newkey ed25519 -noenc -keyout /tmp/test-ca.key -out /tmp/test-ca.pem -subj /CN=Goblin-test-CA -days 1 >/tmp/test-ca.log 2>&1 && { curl --cacert /tmp/test-ca.pem --capath /tmp/empty-cas --silent --show-error --max-time 45 https://deb.debian.org/debian/README >/tmp/untrusted-tls.log 2>&1; status=$?; cat /tmp/untrusted-tls.log; test $status = 60; }")) return false;
    if (!run("git repository and commit", "mkdir -p /root/git-test && cd /root/git-test && git init -q && git config user.name 'Goblin test' && git config user.email 'test@localhost' && printf 'persistent git\\n' >file && git add file && git -c commit.gpgsign=false commit --allow-empty -qm verified && test \"$(git log -1 --format=%s)\" = verified")) return false;
    if (!run("Python files TLS and subprocess", "python3 -c 'import ssl,subprocess,pathlib; p=pathlib.Path(\"/root/python-proof\"); p.write_text(\"persistent Python\"); assert p.read_text()==\"persistent Python\"; assert ssl.create_default_context().get_ca_certs(); assert subprocess.check_output([\"/bin/echo\",\"python-child\"])==b\"python-child\\n\"'")) return false;
    if (!RunSessionTests(root, assets, log)) return false;
    if (!run("Goblin Reactor signed ARM64 repository", R"SH(
set -eu
source=/etc/apt/sources.list.d/goblinreactor.sources
test -r /etc/apt/keyrings/goblinreactor-archive-keyring.gpg
grep -qx 'Architectures: arm64' "$source"
grep -qx 'Signed-By: /etc/apt/keyrings/goblinreactor-archive-keyring.gpg' "$source"
apt-get -o APT::Update::Error-Mode=any update >/var/log/goblin-reactor-apt.log 2>&1 || { cat /var/log/goblin-reactor-apt.log; exit 1; }
test -s /var/lib/apt/lists/apt.goblinreactor.com_dists_trixie_InRelease
apt-cache policy goblin-purrfect >/tmp/goblin-reactor-policy
cat /tmp/goblin-reactor-policy
grep -q 'https://apt.goblinreactor.com.*trixie/main arm64 Packages' /tmp/goblin-reactor-policy
rm -f /tmp/goblin-reactor-policy
)SH")) return false;
    return run("Goblin Reactor rejects a different archive key", R"SH(
set -eu
proof=$(mktemp -d /tmp/goblin-repository-signature.XXXXXX)
trap 'rm -rf "$proof"' EXIT
mkdir -p "$proof/lists/partial"
sed 's|/etc/apt/keyrings/goblinreactor-archive-keyring.gpg|/usr/share/keyrings/debian-archive-keyring.gpg|' /etc/apt/sources.list.d/goblinreactor.sources > "$proof/wrong-key.sources"
status=0
apt-get -o APT::Update::Error-Mode=any -o Dir::Etc::sourcelist="$proof/wrong-key.sources" -o Dir::Etc::sourceparts=- -o Dir::State::lists="$proof/lists" update >"$proof/log" 2>&1 || status=$?
cat "$proof/log"
test "$status" = 100
grep -qiE 'NO_PUBKEY|Missing key|public key' "$proof/log"
)SH");
}
