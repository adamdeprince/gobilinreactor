#pragma once
#include <stdint.h>

// The host and guest are little-endian ARM64. Transport messages carry PTY
// bytes and lifecycle requests; Linux itself implements all process semantics.
namespace goblin_uml {
constexpr uint32_t kMagic = 0x474c4d55;
enum Operation : uint32_t {
    Ready = 1, Open, Input, Resize, Close, Shutdown, Output, Started, Exited,
    Error, ReadFile, FileData, Execute, RescueReady, DnsQuery, DnsAnswer, ConfigurePorts
};
struct Header { uint32_t magic, operation, id, size; };
struct Size { uint32_t rows, columns; };

// Upgrade archive. File content lengths and timestamps are 64-bit; names are
// length-delimited, so newlines and other valid Linux filename bytes survive.
struct Entry {
    uint32_t name_size, mode, uid, gid;
    uint64_t size;
    int64_t seconds;
    uint32_t nanoseconds, flags;
};
enum : uint32_t { HardLink = 1, MetadataOnly = 2 };
static_assert(sizeof(Header) == 16 && sizeof(Entry) == 40, "wire layout");
}
