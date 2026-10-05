#!/bin/sh
# Runs inside the shipped UML kernel; test deadlines belong to the test runner.
set -eu
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export DEBIAN_FRONTEND=noninteractive
mode=${1:-0}
work=$(mktemp -d /tmp/goblin-uml-check.XXXXXX)
trap 'rm -rf "$work"' EXIT
uname -a
test "$(uname -m)" = aarch64
case $(uname -r) in *-goblin) ;; *) echo 'Wrong kernel'; exit 1;; esac
test "$(stat -f -c %T /)" = ext2/ext3
test -f /var/lib/goblin/uml-imported
test -d /home/goblin
test "$(su -s /bin/sh goblin -c 'id -u')" -ne 0
test "$(su -s /bin/sh goblin -c "sudo -n su - -c 'id -u'")" = 0
test "$(su -s /bin/sh goblin -c "sudo -n su - -c 'pwd'")" = /root
printf 'PASS: real Linux kernel, ext4, account and sudo login\n'
printf 'Linux hard links\n' > "$work/original"
ln "$work/original" "$work/link"
test "$(stat -c %i "$work/original")" = "$(stat -c %i "$work/link")"
test "$(stat -c %h "$work/link")" = 2
truncate -s 10G "$work/sparse"
test "$(stat -c %s "$work/sparse")" = 10737418240
test "$(stat -c %b "$work/sparse")" -lt 100
printf 'PASS: hard links and sparse file beyond the former 8 GiB quota\n'
printf 'b\na\n' | sort | tr '\n' ' ' > "$work/pipeline"
test "$(cat "$work/pipeline")" = 'a b '
for n in $(seq 1 80); do sleep 3 & done
wait
printf 'PASS: pipelines and more than 64 concurrent processes\n'
test -s /etc/sudoers.d/90-goblin
test -s /etc/apt/keyrings/goblinreactor-archive-keyring.gpg
test -s /etc/apt/sources.list.d/goblinreactor.sources
test -z "$(dpkg --audit)"
printf 'PASS: deployed sudo and signed repository; consistent package state\n'
if [ "$mode" != 0 ]; then
    apt-get update
    apt-get -y install python3 gcc make git ca-certificates curl
    python3 - <<'PY'
import mmap, os, resource, socket, struct, subprocess, threading
# Exercise an allocation above the old 512 MiB guest window without asking
# the test to exhaust the phone. Touch each page so this verifies real memory.
n = 576 * 1024 * 1024
with mmap.mmap(-1, n) as m:
    for i in range(0, n, 16384): m[i] = 1
    assert m[n - 16384] == 1
s = socket.socket(); s.bind(('127.0.0.1', 0)); s.listen()
def serve():
    c, _ = s.accept()
    with c: c.sendall(b'linux-network')
t = threading.Thread(target=serve); t.start()
with socket.create_connection(s.getsockname()) as c: assert c.recv(100) == b'linux-network'
t.join(); s.close()
print('PASS: memory above 512 MiB, threads and Linux sockets')
# Retained installations may use the original virtual DNS address. Test its
# explicit passt target over both transports without rewriting resolv.conf.
query = struct.pack('!HHHHHH', 0x474f, 0x100, 1, 0, 0, 0)
query += b'\x03apt\x0dgoblinreactor\x03com\x00\x00\x01\x00\x01'
for kind in (socket.SOCK_DGRAM, socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.settimeout(10)
        s.connect(('10.0.2.3', 53))
        if kind == socket.SOCK_DGRAM:
            s.send(query); response = s.recv(4096)
        else:
            def receive(n):
                data = b''
                while len(data) < n:
                    part = s.recv(n - len(data))
                    assert part, 'DNS TCP response truncated'
                    data += part
                return data
            s.sendall(struct.pack('!H', len(query)) + query)
            response = receive(struct.unpack('!H', receive(2))[0])
        ident, flags, questions, answers, _, _ = struct.unpack('!HHHHHH', response[:12])
        assert ident == 0x474f and flags & 0x8000 and flags & 15 == 0 and answers > 0
print('PASS: virtual DNS address forwards UDP and TCP without changing guest configuration')
PY
    printf '#include <stdio.h>\nint main(void){puts("UML native compiler");}\n' > "$work/test.c"
    gcc "$work/test.c" -o "$work/test"
    test "$("$work/test")" = 'UML native compiler'
    curl -fsSL https://apt.goblinreactor.com/ >/dev/null
    printf 'PASS: apt, native compiler and HTTPS\n'
fi
if [ "$mode" = 4 ]; then
    apt-get -y install emacs-nox
    emacs --batch -Q --eval '(princ "UML Emacs works\n")'
    test -z "$(dpkg --audit)"
    printf 'PASS: emacs-nox installation and execution\n'
fi
sync
printf 'GOBLIN PASS\n'
