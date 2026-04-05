#include "elf.h"

#include <stdio.h>
#include <string.h>
#include "log.h"


static int elf_bit(const void *base) {
    return *(char *)(base + 4);
}

int validate_elf(const void *base) {
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)base;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        LOGE("invalid ELF magic at %p", base);
        return -1;
    }

    if (elf_bit(base) != ELFCLASS64) {
        LOGE("unsupported ELF class %d at %p", elf_bit(base), base);
        return -1;
    }
    return 0;
}

unsigned long find_module_base(const char *maps_path, const char *module_name) {
    FILE *f = fopen(maps_path, "r");
    if (!f)
        return 0;

    char line[512];
    unsigned long base = 0;
    while (fgets(line, sizeof(line), f)) {
        char *slash = strrchr(line, '/');
        if (slash) {
            char *fname = slash + 1;
            char *nl = strchr(fname, '\n');
            if (nl) *nl = '\0';
            if (strcmp(fname, module_name) == 0) {
                sscanf(line, "%lx-", &base);
                break;
            }
        }
    }
    fclose(f);
    LOGD("find_module_base('%s') = 0x%lx [from %s]", module_name, base, maps_path);
    return base;
}

Elf64_Dyn *find_dynamic_section(unsigned long base)
{
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)base;
    if (validate_elf((const void *)base) != 0)
        return NULL;

    Elf64_Phdr *phdrs = (Elf64_Phdr *)(base + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            Elf64_Dyn *dyn = (Elf64_Dyn *)(base + phdrs[i].p_vaddr);
            LOGD("PT_DYNAMIC found at phdr[%d], dyn=%p", i, (void *)dyn);
            return dyn;
        }
    }
    LOGE("PT_DYNAMIC not found in %u phdrs", ehdr->e_phnum);
    return NULL;
}

unsigned long get_dyn_entry(Elf64_Dyn *dyn, int tag)
{
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == tag) {
            LOGD("get_dyn_entry(%d) = 0x%llx", tag, (unsigned long long)d->d_un.d_val);
            return d->d_un.d_val;
        }
    }
    LOGE("get_dyn_entry(%d): tag not found", tag);
    return 0;
}

int find_got_slot(unsigned long base, Elf64_Dyn *dyn,
                  const char *sym_name, void ***out_slot)
{
    Elf64_Rela *rela = (Elf64_Rela *)(base + get_dyn_entry(dyn, DT_JMPREL));
    size_t rela_size = get_dyn_entry(dyn, DT_PLTRELSZ);
    Elf64_Sym *symtab = (Elf64_Sym *)(base + get_dyn_entry(dyn, DT_SYMTAB));
    const char *strtab = (const char *)(base + get_dyn_entry(dyn, DT_STRTAB));

    LOGD("JMPREL=%p PLTRELSZ=%zu SYMTAB=%p STRTAB=%p",
         (void *)rela, rela_size, (void *)symtab, (void *)strtab);

    size_t count = rela_size / sizeof(Elf64_Rela);
    LOGD("scanning %zu rela entries for '%s'", count, sym_name);

    for (size_t i = 0; i < count; i++) {
        Elf64_Rela *rela_entry = &rela[i];
        size_t sym_idx = ELF64_R_SYM(rela_entry->r_info);
        const char *name = strtab + symtab[sym_idx].st_name;
        if (strcmp(name, sym_name) == 0) {
            *out_slot = (void **)(base + rela_entry->r_offset);
            LOGD("matched '%s' at rela[%zu], GOT slot=%p", name, i, (void *)*out_slot);
            return 0;
        }
    }
    LOGE("'%s' not found in %zu rela entries", sym_name, count);
    return -1;
}
