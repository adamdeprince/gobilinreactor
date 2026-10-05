/* Native ARM64 regression for shared-MM execution on separate UML CPUs.
 * The ping-pong loop makes no syscalls: counting CPUs or Python threads cannot
 * substitute for two cores exchanging data while both execute guest code.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x, errno); exit(1); } } while (0)
static unsigned cpus;
static uint64_t frequency;
static _Thread_local volatile uint64_t cookie;
static _Thread_local sigjmp_buf recovery;
static _Thread_local volatile sig_atomic_t expecting_fault, received_signal;
static atomic_uint ping, pong, stop_ping;
static pthread_barrier_t barrier;
static volatile uint64_t *page;
static size_t page_size;

static uint64_t ticks(void) {
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
}
static void pin(unsigned cpu) {
    cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(cpu, &mask);
    CHECK(pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) == 0);
}
static void rendezvous(void) {
    int r = pthread_barrier_wait(&barrier);
    CHECK(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
}
static void *answer(void *unused) {
    (void)unused; pin(1); cookie = 0x1234abcd;
    rendezvous();
    while (!atomic_load_explicit(&stop_ping, memory_order_relaxed)) {
        unsigned value = atomic_load_explicit(&ping, memory_order_acquire);
        CHECK(cookie == 0x1234abcd);
        atomic_store_explicit(&pong, value, memory_order_release);
    }
    return NULL;
}
static void concurrent_instructions(void) {
    pthread_t thread;
    CHECK(pthread_barrier_init(&barrier, NULL, 2) == 0);
    CHECK(pthread_create(&thread, NULL, answer, NULL) == 0);
    pin(0); rendezvous();
    unsigned fast = 0;
    uint64_t best = UINT64_MAX;
    for (unsigned i = 1; i <= 1000; i++) {
        uint64_t start = ticks();
        atomic_store_explicit(&ping, i, memory_order_release);
        while (atomic_load_explicit(&pong, memory_order_acquire) != i)
            CHECK(ticks() - start < frequency * 5);
        uint64_t elapsed = ticks() - start;
        if (elapsed < best) best = elapsed;
        if (elapsed < frequency / 10000) fast++; /* 100 microseconds */
    }
    atomic_store(&stop_ping, 1);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(pthread_barrier_destroy(&barrier) == 0);
    printf("Native cross-core handoffs: %u/1000 below 100 us, fastest %.3f us\n",
           fast, (double)best * 1e6 / frequency);
    CHECK(fast >= 50);
    puts("PASS: two threads execute guest instructions concurrently");
}

struct work { unsigned cpu; uint64_t iterations, result; int parallel; };
static uint64_t compute(uint64_t iterations, uint64_t seed) {
    uint64_t x = seed;
    for (uint64_t i = 0; i < iterations; i++) {
        x ^= x >> 13; x *= UINT64_C(0x9e3779b97f4a7c15); x ^= x << 7;
    }
    return x;
}
static void *work(void *argument) {
    struct work *w = argument; pin(w->cpu); cookie = w->cpu + 0xa100;
    if (w->parallel) rendezvous();
    w->result = compute(w->iterations, w->cpu + 1);
    CHECK(cookie == w->cpu + 0xa100);
    if (w->parallel) rendezvous();
    return NULL;
}
static void throughput(void) {
    uint64_t iterations = 1000000, start, duration, result = 0;
    do {
        start = ticks(); result ^= compute(iterations, 42); duration = ticks() - start;
        if (duration < frequency / 8) iterations *= 2;
    } while (duration < frequency / 8);
    struct work *w = calloc(cpus, sizeof(*w));
    pthread_t *threads = calloc(cpus, sizeof(*threads));
    uint64_t *expected = calloc(cpus, sizeof(*expected));
    CHECK(w && threads && expected);
    start = ticks();
    for (unsigned i = 0; i < cpus; i++) {
        w[i] = (struct work){i, iterations, 0, 0};
        CHECK(pthread_create(&threads[i], NULL, work, &w[i]) == 0);
        CHECK(pthread_join(threads[i], NULL) == 0);
        expected[i] = w[i].result;
    }
    uint64_t serial = ticks() - start;
    CHECK(pthread_barrier_init(&barrier, NULL, cpus + 1) == 0);
    for (unsigned i = 0; i < cpus; i++) {
        w[i].parallel = 1;
        CHECK(pthread_create(&threads[i], NULL, work, &w[i]) == 0);
    }
    start = ticks(); rendezvous(); rendezvous(); duration = ticks() - start;
    for (unsigned i = 0; i < cpus; i++) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(w[i].result == expected[i]);
    }
    CHECK(pthread_barrier_destroy(&barrier) == 0);
    printf("CPU work: %u cores, serial %.3f s, parallel %.3f s, speedup %.2fx, checksum %" PRIu64 "\n",
           cpus, (double)serial/frequency, (double)duration/frequency, (double)serial/duration, result);
    /* Heterogeneous cores and host load affect this number. The syscall-free
     * handoff above is the direct regression for the former serialization. */
    puts("PASS: CPU-affine work and independent TLS on every guest CPU");
    free(expected); free(threads); free(w);
}

static void handler(int signal) {
    if (signal == SIGUSR1) { received_signal++; return; }
    if (signal == SIGSEGV && expecting_fault) {
        expecting_fault = 0; siglongjmp(recovery, 1);
    }
    _exit(90);
}
static void protected_access(int write_access) {
    expecting_fault = 1;
    if (sigsetjmp(recovery, 1) == 0) {
        if (write_access) *page = 0;
        else { volatile uint64_t value = *page; (void)value; }
        CHECK(0 && "memory revocation was not visible to another core");
    }
    CHECK(!expecting_fault);
}
static void *mapping_reader(void *unused) {
    (void)unused; pin(1); cookie = 0x987654;
    for (unsigned i = 1; i <= 100; i++) {
        rendezvous(); CHECK(*page == i); rendezvous();
        rendezvous(); protected_access(1); rendezvous();
        rendezvous(); protected_access(0); rendezvous();
        rendezvous(); protected_access(0); rendezvous();
        CHECK(cookie == 0x987654);
    }
    return NULL;
}
static void mapping_coherence(void) {
    struct sigaction sa = {.sa_handler = handler};
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGSEGV, &sa, NULL) == 0);
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    page_size = sysconf(_SC_PAGESIZE);
    page = mmap(NULL, page_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    CHECK(pthread_barrier_init(&barrier, NULL, 2) == 0);
    pthread_t reader; CHECK(pthread_create(&reader, NULL, mapping_reader, NULL) == 0);
    pin(0);
    for (unsigned i = 1; i <= 100; i++) {
        CHECK(mmap((void *)page, page_size, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0) == page);
        *page = i; rendezvous(); rendezvous();
        CHECK(mprotect((void *)page, page_size, PROT_READ) == 0);
        rendezvous(); rendezvous();
        CHECK(mprotect((void *)page, page_size, PROT_NONE) == 0);
        rendezvous(); rendezvous();
        CHECK(munmap((void *)page, page_size) == 0);
        rendezvous(); rendezvous();
    }
    CHECK(pthread_join(reader, NULL) == 0);
    CHECK(pthread_barrier_destroy(&barrier) == 0);
    puts("PASS: cross-core mmap, mprotect, munmap and SIGSEGV delivery (100 rounds)");
}

static atomic_uint arrived;
static void *migrate(void *argument) {
    uintptr_t id = (uintptr_t)argument;
    cookie = id + 0xb000;
    atomic_fetch_add(&arrived, 1);
    rendezvous();
    for (unsigned i = 0; i < 20; i++) {
        pin((id + i) % cpus);
        CHECK(cookie == id + 0xb000);
        CHECK(pthread_kill(pthread_self(), SIGUSR1) == 0);
        CHECK(received_signal == (sig_atomic_t)(i + 1));
        CHECK(sched_yield() == 0);
        CHECK(cookie == id + 0xb000);
    }
    return NULL;
}
static void migration(void) {
    pthread_t threads[80];
    CHECK(pthread_barrier_init(&barrier, NULL, 81) == 0);
    for (uintptr_t i = 0; i < 80; i++) CHECK(pthread_create(&threads[i], NULL, migrate, (void *)i) == 0);
    rendezvous(); CHECK(atomic_load(&arrived) == 80);
    for (unsigned i = 0; i < 80; i++) CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(pthread_barrier_destroy(&barrier) == 0);
    puts("PASS: 80 threads, migration between all CPUs, TLS and targeted signals");
}

static atomic_uint finish_fork, fork_progress;
static void *fork_sibling(void *unused) {
    (void)unused; pin(1); cookie = 0x777777;
    while (!atomic_load(&finish_fork)) {
        atomic_fetch_add(&fork_progress, 1);
        CHECK(cookie == 0x777777);
    }
    return NULL;
}
static void fork_exec(void) {
    pthread_t sibling; pin(0);
    CHECK(pthread_create(&sibling, NULL, fork_sibling, NULL) == 0);
    volatile unsigned *value = mmap(NULL, page_size, PROT_READ|PROT_WRITE,
                                  MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    CHECK(value != MAP_FAILED); *value = 1234;
    for (unsigned i = 0; i < 30; i++) {
        pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) {
            CHECK(*value == 1234); *value = 5678;
            execl("/bin/true", "true", NULL); _exit(91);
        }
        int status; CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(*value == 1234);
    }
    atomic_store(&finish_fork, 1); CHECK(pthread_join(sibling, NULL) == 0);
    CHECK(atomic_load(&fork_progress) > 0);
    CHECK(munmap((void *)value, page_size) == 0);
    puts("PASS: fork copy-on-write and exec while a sibling runs (30 rounds)");
}

static void *mapping_loop(void *arg) {
    pin((uintptr_t)arg);
    for (;;) {
        volatile char *p = mmap(NULL, page_size, PROT_READ|PROT_WRITE,
                               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        CHECK(p != MAP_FAILED); *p = 1;
        CHECK(mprotect((void *)p, page_size, PROT_NONE) == 0);
        CHECK(munmap((void *)p, page_size) == 0);
        atomic_fetch_add(&arrived, 1);
    }
    return NULL;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    cpus = sysconf(_SC_NPROCESSORS_ONLN);
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    CHECK(cpus >= 2 && frequency > 0);
    if (argc == 2 && strcmp(argv[1], "--map-loop") == 0) {
        page_size = sysconf(_SC_PAGESIZE);
        // Test-owned marker lets the host runner identify this exact MM from
        // mapping metadata without reading memory or touching existing guests.
        volatile uint64_t *signature = mmap((void *)UINT64_C(0x1234000000), page_size,
                PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
        CHECK(signature == (void *)UINT64_C(0x1234000000));
        *signature = UINT64_C(0x676f626c696e);
        pthread_t thread; CHECK(pthread_create(&thread, NULL, mapping_loop, (void *)1) == 0);
        while (!atomic_load(&arrived)) sched_yield();
        printf("READY %d\n", getpid());
        mapping_loop((void *)0);
        return 1;
    }
    printf("Guest CPUs: %u; counter frequency: %" PRIu64 "\n", cpus, frequency);
    concurrent_instructions(); throughput(); mapping_coherence(); migration(); fork_exec();
    puts("GOBLIN PARALLEL PASS");
    return 0;
}
