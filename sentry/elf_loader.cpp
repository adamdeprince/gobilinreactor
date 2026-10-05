#include "elf_loader.h"

#include <algorithm>
#include <cstring>

#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

#include "guest_layout.h"

namespace goblin {
namespace {

size_t PageSize() {
    static const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return ps;
}

uintptr_t AlignDown(uintptr_t v, size_t a) { return v & ~static_cast<uintptr_t>(a - 1); }
uintptr_t AlignUp(uintptr_t v, size_t a) { return AlignDown(v + a - 1, a); }

int ProtOf(Elf64_Word flags) {
    int prot = 0;
    if (flags & PF_R) prot |= PROT_READ;
    if (flags & PF_W) prot |= PROT_WRITE;
    if (flags & PF_X) prot |= PROT_EXEC;
    return prot;
}

bool InFile(size_t size, Elf64_Off off, Elf64_Xword len) {
    return off <= size && len <= size - off;
}

}  // namespace

bool LoadElf(const uint8_t* file, size_t size, LoadedImage* out,
             std::string* err, uintptr_t placement, uint64_t memory_limit) {
    auto fail = [&](const char* why) {
        if (err) *err = why;
        return false;
    };

    if (size < sizeof(Elf64_Ehdr)) return fail("file is too small to be an ELF");
    Elf64_Ehdr eh;
    memcpy(&eh, file, sizeof(eh));

    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) return fail("not an ELF file");
    if (eh.e_ident[EI_CLASS] != ELFCLASS64) return fail("not ELF64");
    if (eh.e_ident[EI_DATA] != ELFDATA2LSB) return fail("not little endian");
    if (eh.e_machine != EM_AARCH64) return fail("not an aarch64 image");
    if (eh.e_type != ET_EXEC && eh.e_type != ET_DYN) {
        return fail("not an executable or PIE image");
    }
    if (eh.e_phentsize != sizeof(Elf64_Phdr)) return fail("odd program header size");
    if (!InFile(size, eh.e_phoff,
                static_cast<Elf64_Xword>(eh.e_phnum) * sizeof(Elf64_Phdr))) {
        return fail("program headers fall outside the file");
    }

    std::vector<Elf64_Phdr> phdrs(eh.e_phnum);
    memcpy(phdrs.data(), file + eh.e_phoff, phdrs.size() * sizeof(Elf64_Phdr));
    std::string interpreter;

    // First pass: validate arithmetic and segment spans before mapping anything.
    bool any = false;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < eh.e_phnum; ++i) {
        const Elf64_Phdr& ph = phdrs[i];
        if (ph.p_type == PT_INTERP) {
            if (!interpreter.empty() || ph.p_filesz < 2 || ph.p_filesz > 4096 ||
                !InFile(size, ph.p_offset, ph.p_filesz) || file[ph.p_offset] != '/' ||
                file[ph.p_offset + ph.p_filesz - 1] != 0) return fail("invalid ELF interpreter");
            interpreter.assign(reinterpret_cast<const char*>(file + ph.p_offset), ph.p_filesz - 1);
            if (interpreter.find('\0') != std::string::npos) return fail("embedded NUL in interpreter");
        }
        if (ph.p_type != PT_LOAD) continue;
        if (ph.p_memsz == 0) continue;
        if (ph.p_vaddr > UINT64_MAX - ph.p_memsz ||
            ph.p_vaddr + ph.p_memsz > UINT64_MAX - 65535) return fail("segment address overflow");
        if (ph.p_align > 1 && ((ph.p_align & (ph.p_align - 1)) ||
            (ph.p_vaddr % ph.p_align != ph.p_offset % ph.p_align))) return fail("invalid segment alignment");
        if (ph.p_filesz > ph.p_memsz) return fail("segment filesz exceeds memsz");
        if (!InFile(size, ph.p_offset, ph.p_filesz)) {
            return fail("segment contents fall outside the file");
        }
        any = true;
        lo = std::min<uintptr_t>(lo, ph.p_vaddr);
        hi = std::max<uintptr_t>(hi, ph.p_vaddr + ph.p_memsz);
    }
    if (!any) return fail("image has no loadable segments");

    const size_t ps = PageSize();
    const uintptr_t bias =
        eh.e_type == ET_DYN ? (placement ? placement : GuestWindow::pie_base()) - AlignDown(lo, ps) : 0;
    if (eh.e_type == ET_DYN && AlignDown(lo, ps) > (placement ? placement : GuestWindow::pie_base()))
        return fail("PIE virtual address is too large");
    if (hi > UINTPTR_MAX - bias || eh.e_entry > UINTPTR_MAX - bias)
        return fail("relocated ELF address overflow");

    const uintptr_t start = AlignDown(lo + bias, ps);
    const uintptr_t end = AlignUp(hi + bias, ps);
    if (end - start > memory_limit) return fail("ELF exceeds the guest memory limit");
    if (!GuestWindow::ContainsRange(start, end - start) ||
        end > GuestWindow::stack_bottom()) {
        return fail("image does not fit in the guest window");
    }

    LoadedImage img;
    img.bias = bias;
    img.entry = eh.e_entry + bias;
    img.image_start = start;
    img.image_end = end;
    img.brk = end;
    img.phnum = eh.e_phnum;
    img.interpreter = interpreter;
    for (const auto& ph : phdrs) {
        if (ph.p_type == PT_LOAD && eh.e_phoff >= ph.p_offset &&
            eh.e_phoff - ph.p_offset <= ph.p_filesz &&
            phdrs.size() * sizeof(Elf64_Phdr) <= ph.p_filesz - (eh.e_phoff - ph.p_offset))
            img.phdr = bias + ph.p_vaddr + eh.e_phoff - ph.p_offset;
    }
    if (!img.phdr) return fail("program headers are not in a load segment");

    if (!img.storage.Create(start, end - start, err)) return false;

    // Second pass: contents. Everything goes in before anything is sealed or
    // mapped, so no page is ever both writable and executable.
    for (int i = 0; i < eh.e_phnum; ++i) {
        const Elf64_Phdr& ph = phdrs[i];
        if (ph.p_type != PT_LOAD || ph.p_filesz == 0) continue;
        const uintptr_t vaddr = ph.p_vaddr + bias;
        if (!img.storage.Write(vaddr - start, file + ph.p_offset, ph.p_filesz,
                               err)) {
            return false;
        }
    }
    if (!img.storage.Seal(err)) return false;

    // Make a page plan before mapping: overlapping PT_LOAD pages must retain
    // all segment permissions, and holes remain inaccessible.
    std::vector<int> pages((end - start) / ps, -1);
    bool entry_executable = false;
    for (const auto& ph : phdrs) {
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        const uintptr_t vaddr = ph.p_vaddr + bias;
        const uintptr_t seg_start = AlignDown(vaddr, ps);
        const uintptr_t seg_end = AlignUp(vaddr + ph.p_memsz, ps);
        const int prot = ProtOf(ph.p_flags);
        for (uintptr_t p = seg_start; p < seg_end; p += ps) {
            int& page = pages[(p - start) / ps];
            page = page < 0 ? prot : page | prot;
        }
        if (ph.p_flags & PF_X) {
            img.exec_start = img.exec_start ? std::min(img.exec_start, seg_start) : seg_start;
            img.exec_end = std::max(img.exec_end, seg_end);
            entry_executable |= img.entry >= vaddr && img.entry < vaddr + ph.p_memsz;
        }
    }
    if (!entry_executable) return fail("entry point is not in an executable segment");
    for (size_t i = 0; i < pages.size();) {
        size_t j = i + 1;
        while (j < pages.size() && pages[j] == pages[i]) ++j;
        const uintptr_t addr = start + i * ps;
        if (!img.storage.MapRange(addr, (j - i) * ps, std::max(0, pages[i]), err)) return false;
        if (pages[i] >= 0) img.segments.push_back({addr, start + j * ps, pages[i]});
        i = j;
    }

    *out = std::move(img);
    return true;
}

}  // namespace goblin
