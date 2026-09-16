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
    // A segment with no permissions at all would make the page unreadable and is
    // almost certainly a malformed header; give it read so faults are legible.
    return prot == 0 ? PROT_READ : prot;
}

bool InFile(size_t size, Elf64_Off off, Elf64_Xword len) {
    return off <= size && len <= size - off;
}

}  // namespace

bool LoadElf(const uint8_t* file, size_t size, LoadedImage* out,
             std::string* err) {
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

    const auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(file + eh.e_phoff);

    // First pass: validate, find the span, and reject what phase 1 cannot run.
    bool any = false;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < eh.e_phnum; ++i) {
        const Elf64_Phdr& ph = phdrs[i];
        if (ph.p_type == PT_INTERP) {
            return fail("image is dynamically linked; a guest interpreter is phase 2");
        }
        if (ph.p_type != PT_LOAD) continue;
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
        eh.e_type == ET_DYN ? GuestWindow::pie_base() - AlignDown(lo, ps) : 0;

    const uintptr_t start = AlignDown(lo + bias, ps);
    const uintptr_t end = AlignUp(hi + bias, ps);
    if (!GuestWindow::Contains(start) || !GuestWindow::Contains(end - 1) ||
        end > GuestWindow::stack_bottom()) {
        return fail("image does not fit in the guest window");
    }

    LoadedImage img;
    img.bias = bias;
    img.entry = eh.e_entry + bias;
    img.image_start = start;
    img.image_end = end;
    img.brk = end;

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

    // Third pass: protections. Segments routinely share a page; when they do the
    // more permissive mapping wins, which is the same bargain the kernel makes
    // and the reason linkers align segments in the first place.
    for (int i = 0; i < eh.e_phnum; ++i) {
        const Elf64_Phdr& ph = phdrs[i];
        if (ph.p_type != PT_LOAD) continue;
        const uintptr_t vaddr = ph.p_vaddr + bias;
        const uintptr_t seg_start = AlignDown(vaddr, ps);
        const uintptr_t seg_end = AlignUp(vaddr + ph.p_memsz, ps);
        if (!img.storage.MapRange(seg_start, seg_end - seg_start,
                                  ProtOf(ph.p_flags), err)) {
            return false;
        }
        if (ph.p_flags & PF_X) {
            img.exec_start = img.exec_start == 0
                                 ? seg_start
                                 : std::min(img.exec_start, seg_start);
            img.exec_end = std::max(img.exec_end, seg_end);
        }
    }
    if (img.exec_start == img.exec_end) return fail("image has no executable segment");
    if (img.entry < img.exec_start || img.entry >= img.exec_end) {
        return fail("entry point is not in an executable segment");
    }

    *out = std::move(img);
    return true;
}

}  // namespace goblin
