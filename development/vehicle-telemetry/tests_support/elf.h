// Minimal ELF32 wire declarations for the macOS test of the unchanged donor
// gate. Production ARM compilation uses its real glibc <elf.h>, not this file.
#pragma once
#include <stdint.h>
#define EI_CLASS 4
#define EI_DATA 5
#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define EM_ARM 40
#define PT_NOTE 4
#define NT_GNU_BUILD_ID 3
#define ELFMAG "\177ELF"
#define SELFMAG 4
typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;
typedef struct {
    uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr;
static_assert(sizeof(Elf32_Ehdr) == 52, "ELF32 header wire size");
static_assert(sizeof(Elf32_Phdr) == 32, "ELF32 program header wire size");
