#pragma once
#include <string>
#include <vector>
namespace goblin_uml {
// Read the previous implementation's private on-disk format without running
// its syscall implementation. The original environment remains recoverable.
bool ExportLegacy(const std::string& root, const std::string& output, std::string* error,
                  const std::vector<std::string>& omit = {});
}
