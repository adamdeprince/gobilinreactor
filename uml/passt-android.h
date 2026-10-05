#pragma once
#include <inttypes.h>
#include <netinet/udp.h>
#include <stdint.h>
#ifndef MAXNS
#define MAXNS 3
#endif
#ifndef MAXDNSRCH
#define MAXDNSRCH 6
#endif
#ifndef _PATH_LOG
#define _PATH_LOG "/dev/log"
#endif
static inline int goblin_vring_need_event(uint16_t event, uint16_t next, uint16_t previous) {
    return (uint16_t)(next - event - 1) < (uint16_t)(next - previous);
}
#define vring_need_event goblin_vring_need_event
