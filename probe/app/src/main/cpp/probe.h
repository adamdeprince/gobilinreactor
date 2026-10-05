// GoblinReactor phase 0 device capability probe.
//
// Every design decision in this project rests on empirical facts about a specific
// device's kernel configuration and SELinux policy. This probe establishes them.
// It must run inside a real app process (SELinux domain `untrusted_app`) with a
// modern targetSdkVersion -- results collected from `adb shell` are meaningless,
// because the `shell` domain has entirely different rules.
#pragma once

#include <string>

namespace goblin {

struct ProbePaths {
    std::string internal_data;   // ANativeActivity::internalDataPath
    std::string native_lib_dir;  // directory holding the APK's extracted .so files
};

// Runs every probe and returns a human-readable report.
std::string RunAllProbes(const ProbePaths& paths);

}  // namespace goblin
