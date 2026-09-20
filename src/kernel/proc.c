#include "proc.h"
#include "assert.h"
#include "kprint.h"
#include "lib.h"
#include "pmm.h"
#include "thread.h"
#include "vmm.h"

#define USTACK_VA   0x7fff0000ULL
#define USTACK_SIZE (4 * 4096)
#define PT_LOAD     1
#define PF_X        1
#define PF_W        2

struct elf64_hdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t off, vaddr, paddr, filesz, memsz, align;
};

int proc_exec(const void *img)
{
    const struct elf64_hdr *eh = img;
    if (eh->ident[0] != 0x7f || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F') {
        kprint("exec: not an ELF\n");
        return -1;
    }

    uint64_t pgd = vmm_new_pgd();
    if (!pgd) {
        kprint("exec: no pgd\n");
        return -1;
    }

    const struct elf64_phdr *ph =
        (const struct elf64_phdr *)((const char *)img + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0)
            continue;
        uint64_t va = ph[i].vaddr & ~4095ULL;
        uint64_t end = (ph[i].vaddr + ph[i].memsz + 4095) & ~4095ULL;
        for (uint64_t p = va; p < end; p += 4096) {
            uint64_t frame = pmm_alloc(0);
            if (!frame)
                goto oom;
            uint64_t *dst = pmm_to_virt(frame);
            memset(dst, 0, 4096);
            /* copy the file-backed slice that overlaps this page */
            uint64_t seg_lo = p > ph[i].vaddr ? p - ph[i].vaddr : 0;
            uint64_t seg_hi = p + 4096 - ph[i].vaddr;
            if (seg_hi > ph[i].filesz)
                seg_hi = ph[i].filesz;
            if (seg_lo < seg_hi)
                memcpy((char *)dst + (seg_lo + ph[i].vaddr - p),
                       (const char *)img + ph[i].off + seg_lo,
                       seg_hi - seg_lo);
            unsigned f = VMM_USER | VMM_WRITE | VMM_EXEC;
            if (!(ph[i].flags & PF_W))
                f &= ~VMM_WRITE;
            if (!(ph[i].flags & PF_X))
                f &= ~VMM_EXEC;
            if (vmm_map_pages(pgd, p, frame, 4096, f) != 0)
                goto oom;
        }
    }

    for (uint64_t p = USTACK_VA - USTACK_SIZE; p < USTACK_VA; p += 4096) {
        uint64_t frame = pmm_alloc(0);
        if (!frame)
            goto oom;
        memset(pmm_to_virt(frame), 0, 4096);
        if (vmm_map_pages(pgd, p, frame, 4096, VMM_USER | VMM_WRITE) != 0)
            goto oom;
    }

    int id = thread_spawn_user(eh->entry, USTACK_VA, pgd);
    kprint("exec: entry=%p pgd=%p -> thread %d\n",
           (void *)eh->entry, (void *)pgd, id);
    return id;

oom:
    kprint("exec: out of memory\n");
    return -1;
}
