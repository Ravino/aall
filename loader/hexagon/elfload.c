/*
 * See elfload.h. Freestanding: no libc, no <elf.h> (not available in this
 * environment for hexagon-unknown-linux-musl), so
 * the handful of ELF32 structures we need are defined locally below.
 */
#include "elfload.h"
#include "hx_syscall.h"
#include "hx_string.h"

#define EI_NIDENT 16

typedef struct {
	uint8_t e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint32_t e_entry;
	uint32_t e_phoff;
	uint32_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
	uint32_t p_type;
	uint32_t p_offset;
	uint32_t p_vaddr;
	uint32_t p_paddr;
	uint32_t p_filesz;
	uint32_t p_memsz;
	uint32_t p_flags;
	uint32_t p_align;
} Elf32_Phdr;

typedef struct {
	uint32_t sh_name;
	uint32_t sh_type;
	uint32_t sh_flags;
	uint32_t sh_addr;
	uint32_t sh_offset;
	uint32_t sh_size;
	uint32_t sh_link;
	uint32_t sh_info;
	uint32_t sh_addralign;
	uint32_t sh_entsize;
} Elf32_Shdr;

typedef struct {
	uint32_t st_name;
	uint32_t st_value;
	uint32_t st_size;
	uint8_t st_info;
	uint8_t st_other;
	uint16_t st_shndx;
} Elf32_Sym;

typedef struct {
	uint32_t r_offset;
	uint32_t r_info;
	int32_t r_addend;
} Elf32_Rela;

typedef struct {
	int32_t d_tag;
	uint32_t d_val;
} Elf32_Dyn;

#define PT_LOAD 1
#define PT_DYNAMIC 2

#define SHT_SYMTAB 2
#define SHT_STRTAB 3

#define DT_NULL 0
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_PLTRELSZ 2
#define DT_JMPREL 23

#define R_HEX_GLOB_DAT 33
#define R_HEX_JMP_SLOT 34
#define R_HEX_RELATIVE 35

#define ELF32_R_SYM(i) ((i) >> 8)
#define ELF32_R_TYPE(i) ((i) & 0xff)

static uint32_t align_up(uint32_t v, uint32_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static uint32_t resolve_import(struct hx_import *imports, const char *name)
{
	for (int i = 0; imports[i].name; i++)
		if (hx_streq(imports[i].name, name))
			return imports[i].addr;
	return 0;
}

static int apply_rela_table(uint8_t *base, const Elf32_Rela *rela, uint32_t count,
		const Elf32_Sym *symtab, const char *strtab,
		struct hx_import *imports, uint32_t unresolved_stub)
{
	for (uint32_t i = 0; i < count; i++) {
		uint32_t type = ELF32_R_TYPE(rela[i].r_info);
		uint32_t *target = (uint32_t *)(base + rela[i].r_offset);

		if (type == R_HEX_RELATIVE) {
			*target = (uint32_t)(uintptr_t)base + (uint32_t)rela[i].r_addend;
			continue;
		}
		if (type == R_HEX_GLOB_DAT || type == R_HEX_JMP_SLOT) {
			uint32_t sym_idx = ELF32_R_SYM(rela[i].r_info);
			const char *name = strtab + symtab[sym_idx].st_name;
			uint32_t addr = resolve_import(imports, name);

			if (addr == 0)
				addr = unresolved_stub;
			*target = addr;
			continue;
		}
		/* Unknown relocation type: this module only uses the three
		 * above (confirmed by disassembly); bail out loudly rather than
		 * silently producing a half-loaded image. */
		return -1;
	}
	return 0;
}

int hx_elf_load(const uint8_t *file_data, uint32_t file_size,
		struct hx_import *imports, struct hx_loaded_module *out)
{
	if (file_size < sizeof(Elf32_Ehdr))
		return -1;
	const Elf32_Ehdr *eh = (const Elf32_Ehdr *)file_data;

	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
			eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
		return -1;
	if (eh->e_ident[4] != 1 /* ELFCLASS32 */)
		return -1;

	const Elf32_Phdr *ph = (const Elf32_Phdr *)(file_data + eh->e_phoff);
	uint32_t image_size = 0;
	const Elf32_Phdr *dyn_ph = 0;

	for (int i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type == PT_LOAD) {
			uint32_t end = ph[i].p_vaddr + ph[i].p_memsz;
			if (end > image_size)
				image_size = end;
		} else if (ph[i].p_type == PT_DYNAMIC) {
			dyn_ph = &ph[i];
		}
	}
	if (image_size == 0 || dyn_ph == 0)
		return -1;
	image_size = align_up(image_size, 4096);

	uint8_t *base = (uint8_t *)hx_mmap_anon(image_size,
			HX_PROT_READ | HX_PROT_WRITE | HX_PROT_EXEC);
	if (!base)
		return -1;

	for (int i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		hx_memcpy(base + ph[i].p_vaddr, file_data + ph[i].p_offset,
				ph[i].p_filesz);
		if (ph[i].p_memsz > ph[i].p_filesz)
			hx_memset(base + ph[i].p_vaddr + ph[i].p_filesz, 0,
					ph[i].p_memsz - ph[i].p_filesz);
	}

	const Elf32_Dyn *dyn = (const Elf32_Dyn *)(base + dyn_ph->p_vaddr);
	uint32_t rela_off = 0, rela_sz = 0, relaent = 12;
	uint32_t jmprel_off = 0, jmprel_sz = 0;
	uint32_t symtab_off = 0, strtab_off = 0;

	for (int i = 0; dyn[i].d_tag != DT_NULL; i++) {
		switch (dyn[i].d_tag) {
		case DT_RELA: rela_off = dyn[i].d_val; break;
		case DT_RELASZ: rela_sz = dyn[i].d_val; break;
		case DT_RELAENT: relaent = dyn[i].d_val; break;
		case DT_JMPREL: jmprel_off = dyn[i].d_val; break;
		case DT_PLTRELSZ: jmprel_sz = dyn[i].d_val; break;
		case DT_SYMTAB: symtab_off = dyn[i].d_val; break;
		case DT_STRTAB: strtab_off = dyn[i].d_val; break;
		default: break;
		}
	}
	if (relaent != sizeof(Elf32_Rela) || symtab_off == 0 || strtab_off == 0)
		return -1;

	const Elf32_Sym *dynsym = (const Elf32_Sym *)(base + symtab_off);
	const char *dynstr = (const char *)(base + strtab_off);

	/* Unresolved-import trap: jump target that is guaranteed mapped
	 * (points at ourselves) but whose body just traps; installed by
	 * runtime.c before calling hx_elf_load() via imports[] itself, so
	 * we look it up by the sentinel name "*unresolved*". */
	uint32_t unresolved_stub = resolve_import(imports, "*unresolved*");

	if (rela_sz > 0) {
		const Elf32_Rela *rela = (const Elf32_Rela *)(base + rela_off);
		if (apply_rela_table(base, rela, rela_sz / relaent, dynsym, dynstr,
				imports, unresolved_stub) != 0)
			return -1;
	}
	if (jmprel_sz > 0) {
		const Elf32_Rela *rela = (const Elf32_Rela *)(base + jmprel_off);
		if (apply_rela_table(base, rela, jmprel_sz / relaent, dynsym, dynstr,
				imports, unresolved_stub) != 0)
			return -1;
	}

	out->base = base;
	out->image_size = image_size;
	out->symtab = 0;
	out->symtab_count = 0;
	out->strtab = 0;

	/* Locate the full (non-stripped) .symtab/.strtab via the section
	 * header table, which lives in file_data (not part of any PT_LOAD,
	 * so it is not in the mmap'd image) -- the CAPI encoder module ships
	 * with its symbol table intact. This is best-effort: absence
	 * only disables hx_elf_find_symbol(), the module still loads. */
	if (eh->e_shoff != 0 && eh->e_shnum > 0 &&
			eh->e_shoff + (uint32_t)eh->e_shnum * eh->e_shentsize <= file_size) {
		const Elf32_Shdr *sh = (const Elf32_Shdr *)(file_data + eh->e_shoff);
		const Elf32_Shdr *symtab_sh = 0, *strtab_sh = 0;

		for (int i = 0; i < eh->e_shnum; i++) {
			if (sh[i].sh_type == SHT_SYMTAB) {
				symtab_sh = &sh[i];
				if (sh[i].sh_link < eh->e_shnum)
					strtab_sh = &sh[sh[i].sh_link];
			}
		}
		if (symtab_sh && strtab_sh &&
				symtab_sh->sh_offset + symtab_sh->sh_size <= file_size &&
				strtab_sh->sh_offset + strtab_sh->sh_size <= file_size) {
			out->symtab = file_data + symtab_sh->sh_offset;
			out->symtab_count = symtab_sh->sh_size / sizeof(Elf32_Sym);
			out->strtab = (const char *)(file_data + strtab_sh->sh_offset);
		}
	}

	return 0;
}

uint32_t hx_elf_find_symbol(const struct hx_loaded_module *mod, const char *name)
{
	if (!mod->symtab)
		return 0;
	const Elf32_Sym *sym = (const Elf32_Sym *)mod->symtab;

	for (uint32_t i = 0; i < mod->symtab_count; i++) {
		if (sym[i].st_name == 0 || sym[i].st_value == 0)
			continue;
		if (hx_streq(mod->strtab + sym[i].st_name, name))
			return (uint32_t)(uintptr_t)mod->base + sym[i].st_value;
	}
	return 0;
}
