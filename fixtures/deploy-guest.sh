#!/bin/sh
# Packaged application migration. This runs only inside the Debian guest.
set -eu
export DEBIAN_FRONTEND=noninteractive
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
stage=/var/lib/goblin/deployment
defaults=/var/lib/goblin/config-defaults
completed=/var/lib/goblin/deployment-complete
version=$(cat "$stage/version")
if test "$(cat "$completed" 2>/dev/null || true)" = "$version"; then exit 0; fi
set --
while read -r package version archive; do
    state=$(dpkg-query -W -f='${db:Status-Status} ${Version}' "$package" 2>/dev/null || true)
    case "$state" in
        "installed "*)
            installed=${state#installed }
            if dpkg --compare-versions "$installed" ge "$version"; then continue; fi ;;
    esac
    set -- "$@" "$stage/$archive"
done < "$stage/manifest"
if test "$#" -gt 0; then
    # APT orders pre-dependencies such as libsystemd-shared before systemd.
    # Everything is bundled; deployment must work with no network connection.
    mkdir -p /var/cache/apt/archives
    for archive in "$@"; do
        cached=/var/cache/apt/archives/$(basename "$archive")
        cp "$archive" "$cached.goblin-new"
        mv -f "$cached.goblin-new" "$cached"
    done
    apt-get -y --no-download --no-remove --fix-broken -o Dpkg::Options::=--force-confold install "$@"
fi

# The APK owns its kernel, initramfs and helpers. These files belong to the
# Linux administrator. Keep the last shipped defaults separately so only
# unmodified files receive automatic configuration updates.
umask 022
work=$(mktemp -d /var/lib/goblin/deployment-work.XXXXXX)
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM
mkdir -p "$defaults"
chmod 0700 "$defaults"

same_default() {
    # Never follow a user-created symlink or replace a shared hard link.
    test -f "$1" && test ! -L "$1" &&
        test "$(stat -c '%a:%u:%g:%h' "$1")" = "$(stat -c '%a:%u:%g:%h' "$2")" &&
        cmp -s "$1" "$2"
}

install_default() {
    local source=$1 target=$2 mode=$3
    local previous="$defaults$2" incoming="$work$2" replace=false temporary
    mkdir -p "$(dirname "$incoming")" "$(dirname "$previous")"
    install -m "$mode" -o root -g root "$source" "$incoming"
    if test -f "$previous"; then
        if same_default "$target" "$previous"; then replace=true; fi
    elif test ! -e "$target" && test ! -L "$target" && test ! -e "$completed"; then
        # First installation only. With an older deployment marker but no
        # snapshots, missing files may be intentional deletions; retain them.
        replace=true
    fi
    if "$replace" && ! same_default "$target" "$incoming"; then
        mkdir -p "$(dirname "$target")"
        temporary=$(mktemp "$(dirname "$target")/.goblin-config.XXXXXX")
        install -m "$mode" -o root -g root "$incoming" "$temporary"
        mv -fT "$temporary" "$target"
    elif ! same_default "$target" "$incoming"; then
        printf 'Preserving %s; packaged default: %s\n' "$target" "$previous"
    fi
    # The snapshot also offers the new default for manual review when a user
    # edit, deletion, type, ownership or permission change was preserved.
    # Publish it after the live file. A retry after interruption is harmless,
    # including when the live file already contains this new default.
    mv -fT "$incoming" "$previous"
}

cat > "$work/90-goblin" <<'SUDOERS'
# Managed by Goblin deployment. Applies only to the guest account.
goblin ALL=(ALL:ALL) NOPASSWD: ALL
SUDOERS
chmod 0440 "$work/90-goblin"
# Validate what we ship. An administrator's existing sudo configuration must
# not prevent Linux from booting (a root terminal can repair it).
visudo -cf "$work/90-goblin"
install_default "$work/90-goblin" /etc/sudoers.d/90-goblin 0440
# Keep the archive's trust scoped to its source, and configure it without
# needing network access or touching any other repository on the guest.
install_default "$stage/goblinreactor-archive-keyring.gpg" /etc/apt/keyrings/goblinreactor-archive-keyring.gpg 0644
install_default "$stage/goblinreactor.sources" /etc/apt/sources.list.d/goblinreactor.sources 0644
printf 'nameserver 10.0.2.3\n' > "$work/resolv.conf"
install_default "$work/resolv.conf" /etc/resolv.conf 0644
# Retire only the exact seed policy that blocked service startup when there
# was no init system. Administrator replacements (including symlinks) survive.
printf '#!/bin/sh\n# This userspace session has no system service manager.\nexit 101\n' > "$work/old-policy"
if test -f /usr/sbin/policy-rc.d && test ! -L /usr/sbin/policy-rc.d &&
    test "$(stat -c '%u:%g:%h' /usr/sbin/policy-rc.d)" = 0:0:1 &&
    cmp -s /usr/sbin/policy-rc.d "$work/old-policy"; then
    rm /usr/sbin/policy-rc.d
fi
cat "$stage/version" > "$completed.new"
sync
mv -f "$completed.new" "$completed"
sync
