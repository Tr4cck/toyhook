#ifndef TOYHOOK_UTILS_ELF_H
#define TOYHOOK_UTILS_ELF_H

#include <elf.h>

int validate_elf(const void *base);
unsigned long find_module_base(const char *maps_path, const char *module_name);
Elf64_Dyn *find_dynamic_section(unsigned long base);
unsigned long get_dyn_entry(Elf64_Dyn *dyn, int tag);
int find_got_slot(unsigned long base, Elf64_Dyn *dyn,
                  const char *sym_name, void ***out_slot);

#endif
