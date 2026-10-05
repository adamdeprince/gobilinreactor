#!/usr/bin/env bash
# Run on a Linux build host. NDK_TOOLS is an Android NDK LLVM prebuilt tree;
# its target sysroot and compiler-rt are architecture independent.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE="${UML_SOURCE:-$HERE/build/linux}"
OUTPUT="${UML_OUTPUT:-$HERE/build/kernel}"
NDK_TOOLS="${NDK_TOOLS:?Set NDK_TOOLS to the NDK toolchains/llvm/prebuilt directory}"
CLANG="${CLANG:-clang}"
RESOURCE="$(find "$NDK_TOOLS/lib/clang" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -1)"
mkdir -p "$OUTPUT" "$HERE/build/artifacts"
python3 "$HERE/patch-kernel.py" "$SOURCE"
# Older Linux-host lld releases mishandle compressed NDK debug sections during
# UML's repeated links. Strip only copied target archives, never the SDK itself.
mkdir -p "$OUTPUT/android-libs"
for name in libc libm libdl; do
    llvm-objcopy --strip-debug "$NDK_TOOLS/sysroot/usr/lib/aarch64-linux-android/$name.a" "$OUTPUT/android-libs/$name.a"
done
compiler="$CLANG --target=aarch64-linux-android30 --sysroot=$NDK_TOOLS/sysroot -resource-dir=$RESOURCE --rtlib=compiler-rt --unwindlib=libunwind -fuse-ld=lld -Wno-unused-command-line-argument -L$OUTPUT/android-libs"
args=(ARCH=um SUBARCH=arm64 LLVM=1 "O=$OUTPUT" "CC=$compiler" HOSTCC=clang HOSTCXX=clang++ "CLANG_FLAGS=--target=aarch64-linux-android30 -fintegrated-as" "LDFLAGS_vmlinux=-S")
make -C "$SOURCE" "${args[@]}" defconfig
# Build the full CPU capacity supported by this UML port's Kconfig. The actual
# online count is selected from Android hardware by runtime.cpp at each boot.
"$SOURCE/scripts/config" --file "$OUTPUT/.config" \
    -e STATIC_LINK -e LD_SCRIPT_STATIC -d LD_SCRIPT_DYN -d LD_SCRIPT_DYN_RPATH \
    -e SMP --set-val NR_CPUS 64 \
    -e UML_NET_VECTOR -e HOSTFS -e UNIX98_PTYS -e UNIX -e INET -e IPV6 \
    -e NAMESPACES -e UTS_NS -e IPC_NS -e USER_NS -e PID_NS -e NET_NS \
    -e CGROUPS -e MEMCG -e CGROUP_PIDS -e CGROUP_CPUACCT \
    -e EPOLL -e SIGNALFD -e TIMERFD -e EVENTFD -e INOTIFY_USER \
    -e EXT4_FS_POSIX_ACL -e EXT4_FS_SECURITY -e TMPFS_POSIX_ACL \
    -e OVERLAY_FS -e FUSE_FS -e SECCOMP -e SECCOMP_FILTER \
    -e BLK_DEV_LOOP -e BLK_DEV_INITRD -e DEVTMPFS -e DEVTMPFS_MOUNT \
    -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE \
    --set-str LOCALVERSION '-goblin' --set-str CON_CHAN null --set-str SSL_CHAN null
make -C "$SOURCE" "${args[@]}" olddefconfig
make -C "$SOURCE" "${args[@]}" -j"${JOBS:-$(nproc)}" linux
python3 "$HERE/check-stub.py" "$OUTPUT/arch/um/kernel/skas/stub.o"
cp "$OUTPUT/linux" "$HERE/build/artifacts/libgoblinuml-kernel.so"
cp "$OUTPUT/arch/um/kernel/skas/stub_exe" "$HERE/build/artifacts/libgoblinuml-stub.so"
cp "$OUTPUT/.config" "$HERE/build/artifacts/kernel.config"
mkdir -p "$HERE/build/artifacts/licenses"
cp "$SOURCE/LICENSES/preferred/GPL-2.0" "$HERE/build/artifacts/licenses/Linux-GPL-2.0.txt"
llvm-strip --strip-debug "$HERE/build/artifacts/libgoblinuml-kernel.so"
sha256sum "$HERE/build/artifacts/"*.so
