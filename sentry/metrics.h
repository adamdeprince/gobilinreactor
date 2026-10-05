#pragma once
#include <cstdint>
#include <cstdio>
#include <time.h>
#include <unistd.h>

namespace goblin {
inline uint64_t MonotonicNanoseconds() {
    timespec now{}; clock_gettime(CLOCK_MONOTONIC,&now);
    return uint64_t(now.tv_sec)*1000000000ULL+now.tv_nsec;
}
inline uint64_t BrokerResidentBytes() {
    FILE* file=fopen("/proc/self/statm","re");
    if(!file)return 0;
    unsigned long total=0,resident=0;
    int n=fscanf(file,"%lu %lu",&total,&resident);fclose(file);
    return n==2 ? uint64_t(resident)*sysconf(_SC_PAGESIZE) : 0;
}
}
