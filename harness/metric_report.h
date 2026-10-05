#pragma once
#include "stub.h"
#include <sstream>
#include <unistd.h>

inline std::string MetricReport(const std::string& workload,const goblin::RunResult& result) {
    // Workload names are source constants, never guest-provided JSON fragments.
    std::ostringstream out;
    out << "METRIC {\"workload\":\"" << workload << "\",\"page_size\":" << sysconf(_SC_PAGESIZE)
        << ",\"wall_ns\":" << result.wall_time_ns << ",\"startup_ns\":" << result.startup_ns
        << ",\"syscalls\":" << result.syscalls << ",\"wall_ns_per_syscall\":" << result.ns_per_syscall
        << ",\"broker_ns_per_syscall\":" << result.ns_in_handler
        << ",\"broker_rss_start_bytes\":" << result.broker_rss_start_bytes
        << ",\"broker_rss_end_bytes\":" << result.broker_rss_end_bytes
        << ",\"broker_rss_sampled_peak_bytes\":" << result.broker_rss_sampled_peak_bytes << "}";
    return out.str();
}
