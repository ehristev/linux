// SPDX-License-Identifier: GPL-2.0-only
/*
 * crash.c - kernel crash support code.
 * Copyright (C) 2002-2004 Eric Biederman  <ebiederm@xmission.com>
 */

#include <linux/buildid.h>
#include <linux/init.h>
#include <linux/utsname.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>
#include <linux/kexec.h>
#include <linux/memory.h>
#include <linux/cpuhotplug.h>
#include <linux/memblock.h>
#include <linux/kmemleak.h>
#include <linux/kmemdump.h>
#include <linux/sched/stat.h>

#include <asm/page.h>
#include <asm/sections.h>

#include <crypto/sha1.h>

#include "kallsyms_internal.h"
#include "kexec_internal.h"

void sched_get_runqueues_area(void **start, size_t *size);

extern unsigned int nr_irqs;
extern unsigned long tainted_mask;
extern unsigned int nr_swapfiles;

#ifdef CONFIG_IKCONFIG_PROC
extern char kernel_config_data;
extern char kernel_config_data_end;
#endif

/* vmcoreinfo stuff */
unsigned char *vmcoreinfo_data;
size_t vmcoreinfo_size;
u32 *vmcoreinfo_note;

/* trusted vmcoreinfo, e.g. we can make a copy in the crash memory */
static unsigned char *vmcoreinfo_data_safecopy;

Elf_Word *append_elf_note(Elf_Word *buf, char *name, unsigned int type,
			  void *data, size_t data_len)
{
	struct elf_note *note = (struct elf_note *)buf;

	note->n_namesz = strlen(name) + 1;
	note->n_descsz = data_len;
	note->n_type   = type;
	buf += DIV_ROUND_UP(sizeof(*note), sizeof(Elf_Word));
	memcpy(buf, name, note->n_namesz);
	buf += DIV_ROUND_UP(note->n_namesz, sizeof(Elf_Word));
	memcpy(buf, data, data_len);
	buf += DIV_ROUND_UP(data_len, sizeof(Elf_Word));

	return buf;
}

void final_note(Elf_Word *buf)
{
	memset(buf, 0, sizeof(struct elf_note));
}

static void update_vmcoreinfo_note(void)
{
	u32 *buf = vmcoreinfo_note;

	if (!vmcoreinfo_size)
		return;
	buf = append_elf_note(buf, VMCOREINFO_NOTE_NAME, 0, vmcoreinfo_data,
			      vmcoreinfo_size);
	final_note(buf);
}

void crash_update_vmcoreinfo_safecopy(void *ptr)
{
	if (ptr)
		memcpy(ptr, vmcoreinfo_data, vmcoreinfo_size);

	vmcoreinfo_data_safecopy = ptr;
}

void crash_save_vmcoreinfo(void)
{
	if (!vmcoreinfo_note)
		return;

	/* Use the safe copy to generate vmcoreinfo note if have */
	if (vmcoreinfo_data_safecopy)
		vmcoreinfo_data = vmcoreinfo_data_safecopy;

	vmcoreinfo_append_str("CRASHTIME=%lld\n", ktime_get_real_seconds());
	update_vmcoreinfo_note();
}

void vmcoreinfo_append_str(const char *fmt, ...)
{
	va_list args;
	char buf[0x50];
	size_t r;

	va_start(args, fmt);
	r = vscnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	r = min(r, (size_t)VMCOREINFO_BYTES - vmcoreinfo_size);

	memcpy(&vmcoreinfo_data[vmcoreinfo_size], buf, r);

	vmcoreinfo_size += r;

	WARN_ONCE(vmcoreinfo_size == VMCOREINFO_BYTES,
		  "vmcoreinfo data exceeds allocated size, truncating");
}

/*
 * provide an empty default implementation here -- architecture
 * code may override this
 */
void __weak arch_crash_save_vmcoreinfo(void)
{}

phys_addr_t __weak paddr_vmcoreinfo_note(void)
{
	return __pa(vmcoreinfo_note);
}
EXPORT_SYMBOL(paddr_vmcoreinfo_note);

static void vmcoreinfo_kmemdump(void)
{
	void *start;
	size_t size;
	int i;

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_VMCOREINFO,
			     (void *)vmcoreinfo_data, vmcoreinfo_size);
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_linux_banner,
			     (void *)&linux_banner, banner_len);
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_init_uts_ns,
			     (void *)&init_uts_ns, sizeof(init_uts_ns));

	sched_get_runqueues_area(&start, &size);
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_runqueues,
			     (void *)start, size);

#ifdef CONFIG_IKCONFIG_PROC
	/* Register 8 bytes before and after, to catch the marker too */
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_CONFIG,
			     (void *)&kernel_config_data - 8,
			     &kernel_config_data_end - &kernel_config_data + 16);
#endif

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE___cpu_possible_mask,
			     (void *)&__cpu_possible_mask,
			     sizeof(__cpu_possible_mask));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE___cpu_active_mask,
			     (void *)&__cpu_active_mask,
			     sizeof(__cpu_active_mask));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE___cpu_online_mask,
			     (void *)&__cpu_online_mask,
			     sizeof(__cpu_online_mask));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE___cpu_present_mask,
			     (void *)&__cpu_present_mask,
			     sizeof(__cpu_present_mask));

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_nr_irqs,
			     (void *)&nr_irqs, sizeof(nr_irqs));

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_tainted_mask,
			     (void *)&tainted_mask, sizeof(tainted_mask));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_taint_flags,
			     (void *)&taint_flags, sizeof(taint_flags));

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_jiffies_64,
			     (void *)&jiffies_64, sizeof(jiffies_64));

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_nr_threads,
			     (void *)&nr_threads, sizeof(nr_threads));

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_node_states,
			     (void *)&node_states, sizeof(node_states));

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_init_mm,
			     (void *)&init_mm, sizeof(init_mm));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_init_mm_pgd,
			     (void *)&init_mm.pgd, sizeof(*init_mm.pgd));

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE__totalram_pages,
			     (void *)&_totalram_pages, sizeof(_totalram_pages));

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_nr_swapfiles,
			     (void *)&nr_swapfiles, sizeof(nr_swapfiles));

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE___per_cpu_offset,
			     (void *)&__per_cpu_offset, sizeof(__per_cpu_offset));

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_high_memory,
			     (void *)&high_memory, sizeof(high_memory));
#ifdef CONFIG_NUMA
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_node_data,
			     (void *)&node_data,
			     MAX_NUMNODES * sizeof(struct pglist_data));

	for (i = 0; i < MAX_NUMNODES; i++) {
		if (!NODE_DATA(i))
			continue;
		kmemdump_register((void *)NODE_DATA(i),
				  roundup(sizeof(pg_data_t), SMP_CACHE_BYTES));
	}
#endif

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_mem_section,
			     (void *)&mem_section, sizeof(mem_section));
	for (i = 0; i < NR_SECTION_ROOTS; i++) {
		if (!mem_section[i])
			continue;
		kmemdump_register((void *)mem_section[i],
				  SECTIONS_PER_ROOT * sizeof(struct mem_section));
	}
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_MEMSECT,
			     (void *)mem_section,
			     sizeof(struct mem_section *) * NR_SECTION_ROOTS);

	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_kallsyms_num_syms,
			     (void *)&kallsyms_num_syms,
			     sizeof(kallsyms_num_syms));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_kallsyms_relative_base,
			     (void *)&kallsyms_relative_base,
			     sizeof(kallsyms_relative_base));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_kallsyms_offsets,
			     (void *)&kallsyms_offsets,
			     sizeof(&kallsyms_offsets));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_kallsyms_names,
			     (void *)&kallsyms_names,
			     sizeof(&kallsyms_names));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_kallsyms_token_table,
			     (void *)&kallsyms_token_table,
			     sizeof(&kallsyms_token_table));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_kallsyms_token_index,
			     (void *)&kallsyms_token_index,
			     sizeof(&kallsyms_token_index));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_kallsyms_markers,
			     (void *)&kallsyms_markers,
			     sizeof(&kallsyms_markers));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_kallsyms_seqs_of_names,
			     (void *)&kallsyms_seqs_of_names,
			     sizeof(&kallsyms_seqs_of_names));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE__sinittext,
			     (void *)&_sinittext, sizeof(&_sinittext));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE__einittext,
			     (void *)&_einittext, sizeof(&_einittext));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE__end,
			     (void *)&_end, sizeof(&_end));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE__text,
			     (void *)&_text, sizeof(&_text));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE__stext,
			     (void *)&_stext, sizeof(&_stext));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE__etext,
			     (void *)&_etext, sizeof(&_etext));
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_swapper_pg_dir,
			     (void *)&swapper_pg_dir, sizeof(&swapper_pg_dir));
}

static int __init crash_save_vmcoreinfo_init(void)
{
	vmcoreinfo_data = (unsigned char *)get_zeroed_page(GFP_KERNEL);
	if (!vmcoreinfo_data) {
		pr_warn("Memory allocation for vmcoreinfo_data failed\n");
		return -ENOMEM;
	}

	vmcoreinfo_note = alloc_pages_exact(VMCOREINFO_NOTE_SIZE,
						GFP_KERNEL | __GFP_ZERO);
	if (!vmcoreinfo_note) {
		free_page((unsigned long)vmcoreinfo_data);
		vmcoreinfo_data = NULL;
		pr_warn("Memory allocation for vmcoreinfo_note failed\n");
		return -ENOMEM;
	}

	VMCOREINFO_OSRELEASE(init_uts_ns.name.release);
	VMCOREINFO_BUILD_ID();
	VMCOREINFO_PAGESIZE(PAGE_SIZE);

	VMCOREINFO_SYMBOL(init_uts_ns);
	VMCOREINFO_OFFSET(uts_namespace, name);
	VMCOREINFO_SYMBOL(node_online_map);
#ifdef CONFIG_MMU
	VMCOREINFO_SYMBOL_ARRAY(swapper_pg_dir);
#endif
	VMCOREINFO_SYMBOL(_stext);
	vmcoreinfo_append_str("NUMBER(VMALLOC_START)=0x%lx\n", (unsigned long) VMALLOC_START);

#ifndef CONFIG_NUMA
	VMCOREINFO_SYMBOL(mem_map);
	VMCOREINFO_SYMBOL(contig_page_data);
#endif
#ifdef CONFIG_SPARSEMEM_VMEMMAP
	VMCOREINFO_SYMBOL_ARRAY(vmemmap);
#endif
#ifdef CONFIG_SPARSEMEM
	VMCOREINFO_SYMBOL_ARRAY(mem_section);
	VMCOREINFO_LENGTH(mem_section, NR_SECTION_ROOTS);
	VMCOREINFO_STRUCT_SIZE(mem_section);
	VMCOREINFO_OFFSET(mem_section, section_mem_map);
	VMCOREINFO_NUMBER(SECTION_SIZE_BITS);
	VMCOREINFO_NUMBER(MAX_PHYSMEM_BITS);
#endif
	VMCOREINFO_STRUCT_SIZE(page);
	VMCOREINFO_STRUCT_SIZE(pglist_data);
	VMCOREINFO_STRUCT_SIZE(zone);
	VMCOREINFO_STRUCT_SIZE(free_area);
	VMCOREINFO_STRUCT_SIZE(list_head);
	VMCOREINFO_SIZE(nodemask_t);
	VMCOREINFO_OFFSET(page, flags);
	VMCOREINFO_OFFSET(page, _refcount);
	VMCOREINFO_OFFSET(page, mapping);
	VMCOREINFO_OFFSET(page, lru);
	VMCOREINFO_OFFSET(page, _mapcount);
	VMCOREINFO_OFFSET(page, private);
	VMCOREINFO_OFFSET(page, compound_head);
	VMCOREINFO_OFFSET(pglist_data, node_zones);
	VMCOREINFO_OFFSET(pglist_data, nr_zones);
#ifdef CONFIG_FLATMEM
	VMCOREINFO_OFFSET(pglist_data, node_mem_map);
#endif
	VMCOREINFO_OFFSET(pglist_data, node_start_pfn);
	VMCOREINFO_OFFSET(pglist_data, node_spanned_pages);
	VMCOREINFO_OFFSET(pglist_data, node_id);
	VMCOREINFO_OFFSET(zone, free_area);
	VMCOREINFO_OFFSET(zone, vm_stat);
	VMCOREINFO_OFFSET(zone, spanned_pages);
	VMCOREINFO_OFFSET(free_area, free_list);
	VMCOREINFO_OFFSET(list_head, next);
	VMCOREINFO_OFFSET(list_head, prev);
	VMCOREINFO_LENGTH(zone.free_area, NR_PAGE_ORDERS);
	log_buf_vmcoreinfo_setup();
	VMCOREINFO_LENGTH(free_area.free_list, MIGRATE_TYPES);
	VMCOREINFO_NUMBER(NR_FREE_PAGES);
	VMCOREINFO_NUMBER(PG_lru);
	VMCOREINFO_NUMBER(PG_private);
	VMCOREINFO_NUMBER(PG_swapcache);
	VMCOREINFO_NUMBER(PG_swapbacked);
#define PAGE_SLAB_MAPCOUNT_VALUE	(PGTY_slab << 24)
	VMCOREINFO_NUMBER(PAGE_SLAB_MAPCOUNT_VALUE);
#ifdef CONFIG_MEMORY_FAILURE
	VMCOREINFO_NUMBER(PG_hwpoison);
#endif
	VMCOREINFO_NUMBER(PG_head_mask);
#define PAGE_BUDDY_MAPCOUNT_VALUE	(PGTY_buddy << 24)
	VMCOREINFO_NUMBER(PAGE_BUDDY_MAPCOUNT_VALUE);
#define PAGE_HUGETLB_MAPCOUNT_VALUE	(PGTY_hugetlb << 24)
	VMCOREINFO_NUMBER(PAGE_HUGETLB_MAPCOUNT_VALUE);
#define PAGE_OFFLINE_MAPCOUNT_VALUE	(PGTY_offline << 24)
	VMCOREINFO_NUMBER(PAGE_OFFLINE_MAPCOUNT_VALUE);
#ifdef CONFIG_UNACCEPTED_MEMORY
#define PAGE_UNACCEPTED_MAPCOUNT_VALUE	(PGTY_unaccepted << 24)
	VMCOREINFO_NUMBER(PAGE_UNACCEPTED_MAPCOUNT_VALUE);
#endif

#ifdef CONFIG_KALLSYMS
	VMCOREINFO_SYMBOL(kallsyms_names);
	VMCOREINFO_SYMBOL(kallsyms_num_syms);
	VMCOREINFO_SYMBOL(kallsyms_token_table);
	VMCOREINFO_SYMBOL(kallsyms_token_index);
	VMCOREINFO_SYMBOL(kallsyms_offsets);
	VMCOREINFO_SYMBOL(kallsyms_relative_base);
#endif /* CONFIG_KALLSYMS */

	arch_crash_save_vmcoreinfo();
	update_vmcoreinfo_note();

	vmcoreinfo_kmemdump();
	return 0;
}

subsys_initcall(crash_save_vmcoreinfo_init);
