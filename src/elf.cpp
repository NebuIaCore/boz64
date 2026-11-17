#include "elf.h"

#include <elf.h>

#include <cstdio>
#include <mutex>

#include "cpu.h"

namespace {
constexpr u32 R_ARM_ABS32_ = 2, R_ARM_REL32_ = 3, R_ARM_GLOB_DAT_ = 21, R_ARM_JUMP_SLOT_ = 22,
              R_ARM_RELATIVE_ = 23;
constexpr u32 PT_ARM_EXIDX_ = 0x70000001;

std::recursive_mutex libs_lock;
std::vector<GuestLib*> libs;

u32 elf_hash(const char* name) {
    u32 h = 0;
    for (auto p = reinterpret_cast<const u8*>(name); *p; p++) {
        h = (h << 4) + *p;
        u32 gbits = h & 0xf0000000;
        if (gbits) h ^= gbits >> 24;
        h &= ~gbits;
    }
    return h;
}

const Elf32_Sym* sym_at(const GuestLib& l, u32 i) { return g::p<Elf32_Sym>(l.symtab + i * sizeof(Elf32_Sym)); }
const char* sym_name(const GuestLib& l, const Elf32_Sym* s) { return g::str(l.strtab + s->st_name); }

bool read_file(const std::string& path, std::vector<u8>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    bool ok = fread(out.data(), 1, n, f) == static_cast<size_t>(n);
    fclose(f);
    return ok;
}

// Where an undefined symbol of `lib` comes from: host replacements first, then other guest libraries.
u32 resolve_import(const GuestLib& lib, const char* name, bool weak) {
    if (u32 d = find_data_symbol(name)) return d;
    if (u32 t = find_thunk(name)) return t;
    {
        std::lock_guard lk(libs_lock);
        for (auto* other : libs)
            if (other != &lib)
                if (u32 a = other->lookup(name)) return a;
    }
    if (weak) return 0;
    return thunk_for_missing(name);
}
}  // namespace

u32 GuestLib::lookup(const char* sym) const {
    auto defined = [&](const Elf32_Sym* s) {
        return s->st_shndx != SHN_UNDEF && ELF32_ST_BIND(s->st_info) != STB_LOCAL && strcmp(sym_name(*this, s), sym) == 0;
    };
    if (nbucket) {
        u32 h = elf_hash(sym) % nbucket;
        for (u32 i = g::rd32(bucket + 4 * h); i; i = g::rd32(chain + 4 * i)) {
            const Elf32_Sym* s = sym_at(*this, i);
            if (defined(s)) return bias + s->st_value;
        }
        return 0;
    }
    for (u32 i = 1; i < nsyms; i++) {
        const Elf32_Sym* s = sym_at(*this, i);
        if (defined(s)) return bias + s->st_value;
    }
    return 0;
}

GuestLib* elf_find(const std::string& name) {
    std::lock_guard lk(libs_lock);
    for (auto* l : libs)
        if (l->name == name) return l;
    return nullptr;
}

GuestLib* elf_find_addr(u32 addr) {
    std::lock_guard lk(libs_lock);
    for (auto* l : libs)
        if (addr >= l->base && addr < l->base + l->size) return l;
    return nullptr;
}

u32 elf_lookup_global(const char* sym) {
    std::lock_guard lk(libs_lock);
    for (auto* l : libs)
        if (u32 a = l->lookup(sym)) return a;
    return 0;
}

std::string elf_describe(u32 addr) {
    char buf[160];
    if (const char* tn = thunk_name_at(addr & ~1u)) {
        snprintf(buf, sizeof buf, "[%s]", tn);
        return buf;
    }
    if (GuestLib* l = elf_find_addr(addr)) {
        snprintf(buf, sizeof buf, "%s+0x%x", l->name.c_str(), addr - l->bias);
        return buf;
    }
    snprintf(buf, sizeof buf, "0x%08x", addr);
    return buf;
}

GuestLib* elf_load(const std::string& path, const std::string& name) {
    std::lock_guard lk(libs_lock);
    if (GuestLib* l = elf_find(name)) return l;

    std::vector<u8> f;
    if (!read_file(path, f)) {
        LOGE("cannot read %s", path.c_str());
        return nullptr;
    }
    if (f.size() < sizeof(Elf32_Ehdr) || memcmp(f.data(), ELFMAG, SELFMAG) != 0 || f[EI_CLASS] != ELFCLASS32) {
        LOGE("%s is not a 32-bit ELF file", path.c_str());
        return nullptr;
    }
    auto* eh = reinterpret_cast<const Elf32_Ehdr*>(f.data());
    if (eh->e_machine != EM_ARM) {
        LOGE("%s is not ARM code", path.c_str());
        return nullptr;
    }
    auto* ph = reinterpret_cast<const Elf32_Phdr*>(f.data() + eh->e_phoff);

    u32 lo = 0xffffffff, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        lo = std::min(lo, ph[i].p_vaddr & ~(g::PAGE - 1));
        hi = std::max(hi, ph[i].p_vaddr + ph[i].p_memsz);
    }
    if (lo >= hi) {
        LOGE("%s has nothing to load", path.c_str());
        return nullptr;
    }
    auto* lib = new GuestLib;
    lib->name = name;
    lib->path = path;
    lib->size = ((hi - lo) + g::PAGE - 1) & ~(g::PAGE - 1);
    lib->base = g::page_alloc(lib->size);
    if (!lib->base) fatal("no guest memory for %s", name.c_str());
    lib->bias = lib->base - lo;

    u32 dyn = 0, dyn_count = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr& p = ph[i];
        if (p.p_type == PT_LOAD) {
            if (p.p_offset + p.p_filesz > f.size()) fatal("%s is truncated", name.c_str());
            memcpy(g::mem + lib->bias + p.p_vaddr, f.data() + p.p_offset, p.p_filesz);
        } else if (p.p_type == PT_DYNAMIC) {
            dyn = lib->bias + p.p_vaddr;
            dyn_count = p.p_memsz / sizeof(Elf32_Dyn);
        } else if (p.p_type == PT_ARM_EXIDX_) {
            lib->exidx = lib->bias + p.p_vaddr;
            lib->exidx_count = p.p_memsz / 8;
        }
    }
    if (!dyn) fatal("%s has no dynamic section", name.c_str());

    u32 rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0, init_array_sz = 0, hash = 0;
    std::vector<u32> needed_off;
    for (u32 i = 0; i < dyn_count; i++) {
        auto* d = g::p<Elf32_Dyn>(dyn + i * sizeof(Elf32_Dyn));
        u32 v = d->d_un.d_val;
        switch (d->d_tag) {
        case DT_NULL: i = dyn_count; break;
        case DT_NEEDED: needed_off.push_back(v); break;
        case DT_STRTAB: lib->strtab = lib->bias + v; break;
        case DT_SYMTAB: lib->symtab = lib->bias + v; break;
        case DT_HASH: hash = lib->bias + v; break;
        case DT_REL: rel = lib->bias + v; break;
        case DT_RELSZ: relsz = v; break;
        case DT_JMPREL: jmprel = lib->bias + v; break;
        case DT_PLTRELSZ: pltrelsz = v; break;
        case DT_INIT: lib->init = lib->bias + v; break;
        case DT_INIT_ARRAY: lib->init_array = lib->bias + v; break;
        case DT_INIT_ARRAYSZ: init_array_sz = v; break;
        case DT_RELA:
        case DT_RELASZ: fatal("%s uses RELA relocations, which ARM32 never does", name.c_str());
        default: break;
        }
    }
    lib->init_array_count = init_array_sz / 4;
    for (u32 off : needed_off) lib->needed.push_back(g::str(lib->strtab + off));
    if (hash) {
        lib->nbucket = g::rd32(hash);
        lib->nsyms = g::rd32(hash + 4);
        lib->bucket = hash + 8;
        lib->chain = lib->bucket + 4 * lib->nbucket;
    }
    libs.push_back(lib);  // visible to lookups from here on (its own symbols resolve to itself)

    auto apply = [&](u32 table, u32 bytes) {
        for (u32 o = 0; o < bytes; o += 8) {
            u32 where = lib->bias + g::rd32(table + o);
            u32 info = g::rd32(table + o + 4);
            u32 type = ELF32_R_TYPE(info), symi = ELF32_R_SYM(info);
            u32 S = 0;
            if (symi) {
                const Elf32_Sym* s = sym_at(*lib, symi);
                const char* sn = sym_name(*lib, s);
                if (s->st_shndx != SHN_UNDEF) {
                    // Defined here, unless a host replacement exists (e.g. operator new in a static libstdc++).
                    u32 t = find_thunk(sn);
                    S = (t && ELF32_ST_TYPE(s->st_info) == STT_FUNC) ? 0 : lib->bias + s->st_value;
                    if (!S) S = t;
                } else {
                    S = resolve_import(*lib, sn, ELF32_ST_BIND(s->st_info) == STB_WEAK);
                }
            }
            switch (type) {
            case R_ARM_RELATIVE_: g::wr32(where, g::rd32(where) + lib->bias); break;
            case R_ARM_ABS32_: g::wr32(where, g::rd32(where) + S); break;
            case R_ARM_REL32_: g::wr32(where, g::rd32(where) + S - where); break;
            case R_ARM_GLOB_DAT_:
            case R_ARM_JUMP_SLOT_: g::wr32(where, S); break;
            case 0: break;
            default: fatal("%s: unsupported relocation type %u", name.c_str(), type);
            }
        }
    };
    apply(rel, relsz);
    apply(jmprel, pltrelsz);
    LOGI("loaded %s at 0x%08x (%u KB)", name.c_str(), lib->base, lib->size / 1024);

    if (lib->init) call_guest(lib->init, {});
    for (u32 i = 0; i < lib->init_array_count; i++) {
        u32 fn = g::rd32(lib->init_array + 4 * i);
        if (fn && fn != 0xffffffff) call_guest(fn, {});
    }
    return lib;
}
