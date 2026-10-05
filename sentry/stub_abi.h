// Wire layout shared by the C++ sentry and the freestanding C runtime.
#pragma once
#include <stdint.h>
#include <linux/filter.h>
#include <asm/sigcontext.h>

struct StubContext {
    struct sigcontext machine;
    uint64_t tls;
};

enum StubState {
    STUB_IDLE, STUB_REQUEST, STUB_REPLY, STUB_EXITED, STUB_FAULTED,
    STUB_APPLIED
};

struct StubShared {
    uint32_t state;
    uint32_t sleepers;
    int32_t exit_status;
    int64_t nr;
    uint64_t args[6];
    uint64_t pc;
    int64_t ret;
    int32_t fault_sig;
    int32_t fault_code;
    uint64_t fault_pc;
    uint64_t fault_addr;
    int32_t op_kind;
    uint64_t op_addr;
    uint64_t op_len;
    int32_t op_prot;
    int32_t op_flags;
    int32_t op_fd;
    uint64_t op_offset;
    uint64_t op_new_addr;
    uint64_t op_new_len;
    int64_t applied_result;
    int32_t boot_stage;
    int32_t context_changed;
    struct StubContext context;
};

enum {
    STUB_CODE_SIZE = 65536,
    STUB_BOOT_OFFSET = 65536,
    STUB_SIGNAL_STACK_OFFSET = 131072,
    STUB_SIGNAL_STACK_SIZE = 524288,
    STUB_REGION_SIZE = 2097152,
    STUB_MAX_FILTER = 1024
};

struct StubBoot {
    uint64_t stack_top;  // used by the entry assembly before any C code
    uint64_t region_start;
    uint64_t region_end;
    uint64_t guest_start;
    uint64_t guest_end;
    uint64_t guest_low_start;
    uint64_t guest_low_end;
    uint64_t guest_low_shadow;
    uint64_t guest_entry;
    uint64_t guest_sp;
    uint64_t shared_start;
    uint64_t shared_size;
    uint64_t signal_stack;
    uint64_t signal_stack_size;
    uint64_t fd_limit;
    int32_t parent_pid;
    int32_t shared_memory_fd;
    int32_t transfer_fd;
    uint16_t filter_size;
    struct sock_filter filter[STUB_MAX_FILTER];
    int32_t resume;
    struct StubContext context;
};
