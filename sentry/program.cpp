#include "program.h"
#include "guest_layout.h"
#include <sys/mman.h>
namespace goblin {
bool LoadProgram(const Vfs& vfs, const std::string& path, LoadedImage* image, std::string* error, uint64_t memory_limit) {
    std::vector<uint8_t> bytes;
    if (!vfs.ReadFile(path, &bytes, error)) return false;
    if (memory_limit <= GuestWindow::kStackSize + 65536) { *error = "guest memory limit is too small for its stack"; return false; }
    memory_limit -= GuestWindow::kStackSize + 65536;
    if (!LoadElf(bytes.data(), bytes.size(), image, error, 0, memory_limit)) return false;
    memory_limit -= image->image_end - image->image_start;
    if (image->interpreter.empty()) return true;
    if (!vfs.ReadFile(image->interpreter, &bytes, error)) return false;
    LoadedImage interpreter;
    if (!LoadElf(bytes.data(), bytes.size(), &interpreter, error, GuestWindow::start() + (2 << 20), memory_limit)) return false;
    if (!interpreter.interpreter.empty() || (interpreter.image_start < image->image_end &&
        image->image_start < interpreter.image_end)) {
        *error = "invalid or overlapping ELF interpreter"; return false;
    }
    image->interpreter_base = interpreter.bias;
    image->start_entry = interpreter.entry;
    image->segments.insert(image->segments.end(), interpreter.segments.begin(), interpreter.segments.end());
    image->interpreter_storage.push_back(std::move(interpreter.storage));
    return true;
}
}
