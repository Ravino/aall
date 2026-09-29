/*
 * Minimal ELF32 loader for the Hexagon aptX Adaptive CAPI module.
 *
 * The module is a small ET_DYN shared object with exactly three relocation
 * types observed in practice: R_HEX_RELATIVE (35),
 * R_HEX_GLOB_DAT (33), R_HEX_JMP_SLOT (34). No lazy PLT resolution, no
 * TLS, no other DT_NEEDED beyond libc.so/libgcc.so (both fully stubbed by
 * runtime.c). This loader does not implement a general-purpose ELF dynamic
 * linker: it is deliberately scoped to this one file's needs.
 */
#ifndef APTX_ADAPTIVE_ELFLOAD_H
#define APTX_ADAPTIVE_ELFLOAD_H

#include <stdint.h>

struct hx_import {
	const char *name;
	uint32_t addr; /* runtime address of our stub, 0 = unresolved */
};

struct hx_loaded_module {
	uint8_t *base;        /* load bias: base of the mmap'd image */
	uint32_t image_size;
	const uint8_t *symtab;
	uint32_t symtab_count;
	const char *strtab;
};

/* Loads `file` (already read fully into `file_data`, `file_size` bytes),
 * applies relocations, and resolves imports against `imports`
 * (NULL-name-terminated array). Returns 0 on success. On success `*out`
 * describes the loaded image. */
int hx_elf_load(const uint8_t *file_data, uint32_t file_size,
		struct hx_import *imports, struct hx_loaded_module *out);

/* Looks up a global/local symbol by exact name in the module's (non-
 * stripped) .symtab. Returns the runtime address, or 0 if not found. */
uint32_t hx_elf_find_symbol(const struct hx_loaded_module *mod, const char *name);

#endif
