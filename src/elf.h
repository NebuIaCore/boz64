// Loader for 32-bit ARM ELF shared libraries into guest memory.
#pragma once
#include <string>
#include <vector>

#include "common.h"

struct GuestLib {
    std::string name, path;
    u32 base = 0, size = 0;  // the guest pages holding the image
    u32 bias = 0;            // guest address = bias + ELF virtual address
    u32 symtab = 0, strtab = 0, nsyms = 0;
    u32 nbucket = 0, bucket = 0, chain = 0;
    u32 exidx = 0, exidx_count = 0;
    u32 init = 0, init_array = 0, init_array_count = 0;
    std::vector<std::string> needed;

    u32 lookup(const char* sym) const;  // address of a symbol this library defines, or 0
};

// Loads, relocates and runs the initialisers. Returns nullptr (and logs) on failure.
GuestLib* elf_load(const std::string& path, const std::string& name);
GuestLib* elf_find(const std::string& name);
GuestLib* elf_find_addr(u32 addr);
u32 elf_lookup_global(const char* sym);  // first definition across loaded libraries
std::string elf_describe(u32 addr);      // "libfoo.so+0x1234" for logs
