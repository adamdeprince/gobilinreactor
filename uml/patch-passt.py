#!/usr/bin/env python3
"""Adapt passt's host integration to an ordinary Android application UID."""
from pathlib import Path
import json
import subprocess
import sys

root = Path(sys.argv[1])
# This is the disposable checkout created by the build, not the application
# working tree. Always derive the patch from the pinned upstream files so that
# later transformations cannot invalidate the idempotence of earlier ones.
revision = json.loads((Path(__file__).with_name('sources.lock.json')).read_text())['passt']['commit']
git = ['git', '-c', 'safe.directory=' + str(root.resolve()), '-C', str(root)]
actual = subprocess.check_output([*git, 'rev-parse', 'HEAD'], text=True).strip()
if actual != revision:
    raise RuntimeError('Refusing to patch a passt checkout other than the pinned revision')
for name in ['ip.h', 'tcp.c', 'udp.c', 'conf.c', 'isolation.c', 'fwd_rule.h', 'fwd_rule.c', 'fwd.h', 'fwd.c', 'pif.c', 'pesto.c', 'pesto.h', 'util.h']:
    original = subprocess.check_output([*git, 'show', revision + ':' + name])
    (root / name).write_bytes(original)

def replace(name, before, after):
    p = root / name
    text = p.read_text()
    if after and after in text:
        return
    if not after and before not in text:
        return
    if before not in text:
        raise RuntimeError(f'Upstream context changed: {name}: {before[:60]}')
    p.write_text(text.replace(before, after, 1))

replace('ip.h', 'struct ipv6hdr {', '#ifndef __ANDROID__\nstruct ipv6hdr {')
replace('ip.h', '} __attribute__((packed));\t/* required for some archs */', '} __attribute__((packed));\t/* required for some archs */\n#endif')
replace('tcp.c', '#ifndef __USE_MISC', '#if !defined(__USE_MISC) && !defined(__ANDROID__)')
replace('tcp.c', 'IN6_IS_ADDR_UNSPECIFIED(low_rtt_dst + i)', 'IN6_IS_ADDR_UNSPECIFIED(&low_rtt_dst[i].a6)')
replace('conf.c', '\tnl_sock_init(c, false);\n\tif (!v6_only',
        '#ifndef __ANDROID__\n\tnl_sock_init(c, false);\n\tif (!v6_only')
replace('conf.c', '\t\tc->ifi6 = conf_ip6(ifi6, &c->ip6);',
        '\t\tc->ifi6 = conf_ip6(ifi6, &c->ip6);\n#endif /* Android uses the explicit guest addresses and ordinary sockets. */')
# Android already supplies the app UID, SELinux domain and seccomp policy.
# Its app domain cannot create the extra namespaces used by desktop passt.
replace('isolation.c', 'void isolate_initial(void)\n{',
        'void isolate_initial(void)\n{\n#ifdef __ANDROID__\n\treturn; /* No capabilities are granted to the Android app. */\n#endif')
replace('isolation.c', 'int isolate_fds(int argc, char **argv)\n{',
        'int isolate_fds(int argc, char **argv)\n{\n#ifdef GOBLIN_MANAGED_NETWORK\n\treturn conf_tap_fd(argc, argv); /* Preserve ART/Binder descriptors. */\n#endif')
replace('isolation.c', 'const char *userns)\n{\n\tuint64_t ns_caps = 0;',
        'const char *userns)\n{\n#ifdef __ANDROID__\n\treturn; /* Keep the Android application identity. */\n#endif\n\tuint64_t ns_caps = 0;')
replace('isolation.c', 'int isolate_prefork(const struct ctx *c)\n{',
        'int isolate_prefork(const struct ctx *c)\n{\n#ifdef __ANDROID__\n\treturn 0; /* Android owns the outer sandbox. */\n#endif')
replace('isolation.c', 'void isolate_postfork(const struct ctx *c)\n{',
        'void isolate_postfork(const struct ctx *c)\n{\n#ifdef __ANDROID__\n\treturn; /* The generated desktop filter omits bionic syscalls. */\n#endif')

# The desktop namespace-call macro reserves a 1 MiB automatic clone stack,
# even in unselected branches after inlining. Android uses the supplied socket
# in its existing app sandbox, never desktop host network namespaces.
replace('util.h', '#define RCVBUF_BIG', '''#ifdef GOBLIN_MANAGED_NETWORK
void goblin_network_namespace_unavailable(void) __attribute__((noreturn));
#undef NS_CALL
#define NS_CALL(fn, arg) do { (void)(fn); (void)(arg); goblin_network_namespace_unavailable(); } while (0)
#endif

#define RCVBUF_BIG''')

# Forwarding tables grow with requested rules and listening sockets. The old
# 8-bit epoll rule hint imposed a 255-rule limit unrelated to host resources;
# use the existing address/port lookup instead of that optional hint.
replace('fwd_rule.h', '#define FWD_RULE_BITS\t8\n#define MAX_FWD_RULES\tMAX_FROM_BITS(FWD_RULE_BITS)', '')
replace('fwd_rule.h', '#define MAX_LISTEN_SOCKS\t(NUM_PORTS * 5)', '')
replace('fwd_rule.h', '\tstruct fwd_rule rules[MAX_FWD_RULES];\n\tint *rulesocks[MAX_FWD_RULES];\n\tunsigned sock_count;\n\tint socks[MAX_LISTEN_SOCKS];',
        '\tstruct fwd_rule *rules;\n\tint **rulesocks;\n\tunsigned capacity;\n\tsize_t sock_count;')
replace('fwd_rule.h', 'void fwd_rule_clear(struct fwd_table *fwd);',
        'int fwd_rule_reserve(struct fwd_table *fwd, unsigned count);\nvoid fwd_rule_clear(struct fwd_table *fwd);')
replace('fwd.h', '\tunsigned\trule :FWD_RULE_BITS;', '\tuint8_t\t\treserved;')
replace('pif.c', '\tref.listen.rule = rule;', '\t(void)rule; /* Rules are found from the actual socket address. */\n\tref.listen.reserved = 0;')
replace('tcp.c', 'flow_target(c, flow, ref.listen.rule, IPPROTO_TCP)', 'flow_target(c, flow, FWD_NO_HINT, IPPROTO_TCP) /* Goblin: address lookup */')
replace('udp.c', 'udp_sock_fwd(c, ref.fd, ref.listen.rule,', 'udp_sock_fwd(c, ref.fd, FWD_NO_HINT,')
replace('fwd_rule.c', '#include <fcntl.h>', '#include <fcntl.h>\n#include <stdlib.h>\n#include <limits.h>')
replace('fwd_rule.c', 'void fwd_rule_clear(struct fwd_table *fwd)\n{', '''int fwd_rule_reserve(struct fwd_table *fwd, unsigned count)
{
	size_t capacity;
	struct fwd_rule *rules;
	int **sockets;

	if (count <= fwd->capacity)
		return 0;
	capacity = (size_t)fwd->capacity * 2;
	if (capacity < count)
		capacity = count;
	if (capacity > UINT_MAX)
		capacity = UINT_MAX;
	if (capacity > SIZE_MAX / sizeof(*rules) ||
	    capacity > SIZE_MAX / sizeof(*sockets))
		return -ENOMEM;
	rules = realloc(fwd->rules, capacity * sizeof(*rules));
	if (!rules)
		return -ENOMEM;
	fwd->rules = rules;
	sockets = realloc(fwd->rulesocks, capacity * sizeof(*sockets));
	if (!sockets)
		return -ENOMEM;
	fwd->rulesocks = sockets;
	memset(sockets + fwd->capacity, 0,
	       (capacity - fwd->capacity) * sizeof(*sockets));
	fwd->capacity = capacity;
	return 0;
}

void fwd_rule_clear(struct fwd_table *fwd)
{''')
replace('fwd_rule.c', '\tfwd->count = 0;\n\tfwd->sock_count = 0;',
        '\tfor (unsigned i = 0; i < fwd->count; i++) {\n\t\tfree(fwd->rulesocks[i]);\n\t\tfwd->rulesocks[i] = NULL;\n\t}\n\tfwd->count = 0;\n\tfwd->sock_count = 0;')
replace('fwd_rule.c', '\tfwd->count--;\n\n\tmemmove', '\tfree(fwd->rulesocks[i]);\n\tfwd->count--;\n\n\tmemmove')
p=root/'fwd_rule.c'; text=p.read_text()
start=text.find('\tif (fwd->count >= ARRAY_SIZE(fwd->rules)) {')
end=text.find('\tfor (port = new->first; port <= new->last; port++)', start)
if start >= 0:
    assert end > start
    text=text[:start]+'''	if (fwd->count == UINT_MAX || fwd_rule_reserve(fwd, fwd->count + 1))
		return -ENOMEM;
	fwd->rulesocks[fwd->count] = malloc(num * sizeof(int));
	if (!fwd->rulesocks[fwd->count])
		return -ENOMEM;

'''+text[end:]
    p.write_text(text)
replace('fwd.c', '\t\tfwd->count = fwd->sock_count = 0;', '\t\tfwd_rule_clear(fwd);')
replace('pesto.c', '\tif (pc->fwd.count > MAX_FWD_RULES)\n\t\tdie("Too many forwarding rules");',
        '\tif (fwd_rule_reserve(&pc->fwd, pc->fwd.count))\n\t\tdie("Unable to allocate forwarding rules");')
replace('conf.c', '''		if (count > MAX_FWD_RULES) {
			err("Received %"PRIu32" rules (maximum %u)",
			    count, MAX_FWD_RULES);
			return -1;
		}
''', '''		/* Rules are allocated as they arrive; resource exhaustion is
		 * reported by fwd_rule_add, not an arbitrary table-size quota. */
''')

replace('pesto.c', '\tprog.len = (unsigned short)sizeof(filter_pesto) /', '#ifndef __ANDROID__\n\tprog.len = (unsigned short)sizeof(filter_pesto) /')
replace('pesto.c', '\t\tdie("Failed to apply seccomp filter");', '\t\tdie("Failed to apply seccomp filter");\n#endif /* Android retains its own UID and syscall policy. */')
replace('pif.c', 'listen(ref.fd, 128)', 'listen(ref.fd, SOMAXCONN)')
replace('fwd_rule.c', 'int fwd_rule_reserve(struct fwd_table *fwd, unsigned count)',
        '/*\n * #syscalls:pesto brk mmap munmap mremap\n */\nint fwd_rule_reserve(struct fwd_table *fwd, unsigned count)')
# Remove the old single-line annotation: the upstream filter generator treats
# every token following #syscalls as a syscall name, including a closing */.
p = root / 'fwd_rule.c'
p.write_text(p.read_text().replace('/* #syscalls:pesto brk mmap munmap mremap */\n', ''))
# Version 3 adds an explicit commit acknowledgement. Failed bindings restore
# the prior forwarding table and do not terminate established TCP flows.
replace('pesto.h', '#define PESTO_PROTOCOL_VERSION\t2', '#define PESTO_PROTOCOL_VERSION\t3')
replace('fwd.h', 'void fwd_listen_switch(struct ctx *c);', 'int fwd_listen_switch(struct ctx *c);')
p=root/'fwd.c'; text=p.read_text(); start=text.find('void fwd_listen_switch(struct ctx *c)\n{'); end=text.find('\n/* See enum in kernel', start)
if start >= 0:
    assert end > start
    text=text[:start]+'''int fwd_listen_switch(struct ctx *c)
{
	struct fwd_table *old[PIF_NUM_TYPES];
	unsigned i;
	for (i = 0; i < PIF_NUM_TYPES; i++) {
		old[i] = c->fwd[i];
		if (old[i])
			fwd_listen_close(old[i]);
		c->fwd[i] = c->fwd_pending[i];
		c->fwd_pending[i] = old[i];
	}
	if (fwd_listen_init(c) < 0) {
		for (i = 0; i < PIF_NUM_TYPES; i++) {
			if (c->fwd[i])
				fwd_listen_close(c->fwd[i]);
			c->fwd_pending[i] = c->fwd[i];
			c->fwd[i] = old[i];
		}
		if (fwd_listen_init(c) < 0)
			err("Unable to rebind previous forwarding rules");
		return -1;
	}
	for (i = 0; i < PIF_NUM_TYPES; i++)
		fwd_rule_clear(c->fwd_pending[i]);
	return 0;
}
''' + text[end:]
    p.write_text(text)
replace('conf.c', '\t\tif (conf_recv_rules(c, c->fd_control) < 0)\n\t\t\tgoto close;',
        '\t\tif (conf_recv_rules(c, c->fd_control) < 0) {\n\t\t\twrite_u32(c->fd_control, 1);\n\t\t\tgoto close;\n\t\t}')
replace('conf.c', '\t\tfwd_listen_switch(c);', '\t\twrite_u32(c->fd_control, fwd_listen_switch(c) ? 1 : 0);')
replace('pesto.c', '\tsend_conf(s, &conf);', '''	send_conf(s, &conf);
	uint32_t committed;
	if (read_u32(s, &committed) < 0 || committed)
		die("Cannot bind the requested ports; previous rules retained");''')
