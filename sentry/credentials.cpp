#include "credentials.h"
#include "sentry.h"
#include <algorithm>
#include <cerrno>
#include <unistd.h>

namespace goblin {
namespace { thread_local const Credentials* current = nullptr; const Credentials root; }
const Credentials& CurrentCredentials() { return current ? *current : root; }
CredentialScope::CredentialScope(const Credentials& value) : previous_(current) { current = &value; }
CredentialScope::~CredentialScope() { current = previous_; }
bool Credentials::InGroup(uint32_t group) const {
    return group == fsgid || std::find(groups.begin(), groups.end(), group) != groups.end();
}
int CheckPermission(const struct stat& st, int mode) {
    if (mode & ~(R_OK | W_OK | X_OK)) return -EINVAL;
    const auto& c = CurrentCredentials();
    if (!c.fsuid) return (mode & X_OK) && !S_ISDIR(st.st_mode) && !(st.st_mode & 0111) ? -EACCES : 0;
    unsigned bits = st.st_mode >> (c.fsuid == st.st_uid ? 6 : c.InGroup(st.st_gid) ? 3 : 0);
    return (bits & mode) == unsigned(mode) ? 0 : -EACCES;
}
std::optional<long> Credentials::Handle(const SyscallRequest& req, const GuestMemory& memory) {
    const auto* a = req.args;
    constexpr uint32_t unchanged = UINT32_MAX;
    const bool privileged = euid == 0;
    auto known = [](uint32_t n, uint32_t r, uint32_t e, uint32_t s) { return n == unchanged || n == r || n == e || n == s; };
    switch (req.nr) {
        case 167: // prctl: inherited, irreversible Linux no_new_privs flag
            if (a[0] == 38) {
                if (a[1] != 1 || a[2] || a[3] || a[4]) return -EINVAL;
                no_new_privs = true; return 0;
            }
            if (a[0] == 39) return (a[1] || a[2] || a[3] || a[4]) ? -EINVAL : long(no_new_privs);
            return -EINVAL;
        case 174: return uid;
        case 175: return euid;
        case 176: return gid;
        case 177: return egid;
        case 146: case 144: { // setuid / setgid
            uint32_t n = a[0]; if (n == unchanged) return -EINVAL;
            auto& r = req.nr == 146 ? uid : gid;
            auto& e = req.nr == 146 ? euid : egid;
            auto& s = req.nr == 146 ? suid : sgid;
            auto& f = req.nr == 146 ? fsuid : fsgid;
            if (privileged) r = e = s = f = n;
            else if (n == r || n == s) e = f = n;
            else return -EPERM;
            return 0;
        }
        case 145: case 143: { // setreuid / setregid
            uint32_t nr = a[0], ne = a[1];
            auto& r = req.nr == 145 ? uid : gid; auto& e = req.nr == 145 ? euid : egid;
            auto& s = req.nr == 145 ? suid : sgid; auto& f = req.nr == 145 ? fsuid : fsgid;
            if (!privileged && ((nr != unchanged && nr != r && nr != e) || !known(ne,r,e,s))) return -EPERM;
            const uint32_t old = r;
            if (nr != unchanged) r = nr;
            if (ne != unchanged) e = ne;
            if (nr != unchanged || (ne != unchanged && ne != old)) s = e;
            f = e; return 0;
        }
        case 147: case 149: { // setresuid / setresgid
            uint32_t nr = a[0], ne = a[1], ns = a[2];
            auto& r = req.nr == 147 ? uid : gid; auto& e = req.nr == 147 ? euid : egid;
            auto& s = req.nr == 147 ? suid : sgid; auto& f = req.nr == 147 ? fsuid : fsgid;
            if (!privileged && (!known(nr,r,e,s) || !known(ne,r,e,s) || !known(ns,r,e,s))) return -EPERM;
            if (nr != unchanged) r = nr;
            if (ne != unchanged) e = ne;
            if (ns != unchanged) s = ns;
            f = e; return 0;
        }
        case 148: case 150: {
            uint32_t ids[] = {req.nr == 148 ? uid : gid, req.nr == 148 ? euid : egid, req.nr == 148 ? suid : sgid};
            for (unsigned i = 0; i < 3; ++i) if (!memory.Write(a[i],ids+i,4)) return -EFAULT;
            return 0;
        }
        case 151: case 152: { // setfsuid / setfsgid return the previous ID, including failure
            uint32_t n = a[0]; auto& f = req.nr == 151 ? fsuid : fsgid;
            uint32_t old = f;
            if (n != unchanged && (privileged || n == f || known(n,req.nr == 151 ? uid : gid,req.nr == 151 ? euid : egid,req.nr == 151 ? suid : sgid))) f = n;
            return old;
        }
        case 158: { // getgroups
            int size = a[0]; if (size < 0) return -EINVAL;
            if (!size) return groups.size();
            if (unsigned(size) < groups.size()) return -EINVAL;
            return memory.Write(a[1],groups.data(),groups.size()*4) ? long(groups.size()) : -EFAULT;
        }
        case 159: { // setgroups
            if (!privileged) return -EPERM;
            if (a[0] > 65536) return -EINVAL;
            std::vector<uint32_t> proposed(a[0]);
            if (!memory.Read(a[1],proposed.data(),proposed.size()*4)) return -EFAULT;
            if (std::find(proposed.begin(),proposed.end(),unchanged) != proposed.end()) return -EINVAL;
            std::sort(proposed.begin(),proposed.end()); groups = std::move(proposed); return 0;
        }
        default: return {};
    }
}
}
