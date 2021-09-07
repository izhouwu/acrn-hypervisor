/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <asm/e820.h>
#include <asm/mmu.h>
#include <asm/guest/vm.h>
#include <asm/guest/ept.h>
#include <reloc.h>
#include <vacpi.h>
#include <logmsg.h>
#include <asm/rtcm.h>
#include <ptdev.h>

#define ENTRY_HPA1_LOW_PART1	2U
#define ENTRY_HPA1_LOW_PART2	4U
#define ENTRY_HPA1_HI		9U

static struct e820_entry sos_vm_e820[E820_MAX_ENTRIES];
static struct e820_entry pre_vm_e820[PRE_VM_NUM][E820_MAX_ENTRIES];

uint64_t find_space_from_ve820(struct acrn_vm *vm, uint32_t size, uint64_t min_addr, uint64_t max_addr)
{
	int32_t i;
	uint64_t gpa = INVALID_GPA;
	uint64_t round_min_addr = round_page_up(min_addr);
	uint64_t round_max_addr = round_page_down(max_addr);
	uint32_t round_size = round_page_up(size);

	for (i = (int32_t)(vm->e820_entry_num - 1U); i >= 0; i--) {
		struct e820_entry *entry = vm->e820_entries + i;
		uint64_t start, end, length;

		start = round_page_up(entry->baseaddr);
		end = round_page_down(entry->baseaddr + entry->length);
		length = (end > start) ? (end - start) : 0UL;

		if ((entry->type == E820_TYPE_RAM) && (length >= round_size)
				&& (end > round_min_addr) && (start < round_max_addr)) {
			if (((start >= min_addr) && ((start + round_size) <= min(end, round_max_addr)))
				|| ((start < min_addr) && ((min_addr + round_size) <= min(end, round_max_addr)))) {
				gpa = (end > round_max_addr) ? (round_max_addr - round_size) : (end - round_size);
				break;
			}
		}
	}
	return gpa;
}

/* a sorted VM e820 table is critical for memory allocation or slot location,
 * for example, put ramdisk at the end of TOLUD(Top of LOW Usable DRAM) and
 * put kernel at its begining so that provide largest load capicity for them.
 */
static void sort_vm_e820(struct acrn_vm *vm)
{
	uint32_t i,j;
	struct e820_entry tmp_entry;

	/* Bubble sort */
	for (i = 0U; i < (vm->e820_entry_num - 1U); i++) {
		for (j = 0U; j < (vm->e820_entry_num - i - 1U); j++) {
			if (vm->e820_entries[j].baseaddr > vm->e820_entries[j + 1U].baseaddr) {
				tmp_entry = vm->e820_entries[j];
				vm->e820_entries[j] = vm->e820_entries[j + 1U];
				vm->e820_entries[j + 1U] = tmp_entry;
			}
		}
	}
}

static void filter_mem_from_sos_e820(struct acrn_vm *vm, uint64_t start_pa, uint64_t end_pa)
{
	uint32_t i;
	uint64_t entry_start;
	uint64_t entry_end;
	uint32_t entries_count = vm->e820_entry_num;
	struct e820_entry *entry, new_entry = {0};

	for (i = 0U; i < entries_count; i++) {
		entry = &sos_vm_e820[i];
		entry_start = entry->baseaddr;
		entry_end = entry->baseaddr + entry->length;

		/* No need handle in these cases*/
		if ((entry->type != E820_TYPE_RAM) || (entry_end <= start_pa) || (entry_start >= end_pa)) {
			continue;
		}

		/* filter out the specific memory and adjust length of this entry*/
		if ((entry_start < start_pa) && (entry_end <= end_pa)) {
			entry->length = start_pa - entry_start;
			continue;
		}

		/* filter out the specific memory and need to create a new entry*/
		if ((entry_start < start_pa) && (entry_end > end_pa)) {
			entry->length = start_pa - entry_start;
			new_entry.baseaddr = end_pa;
			new_entry.length = entry_end - end_pa;
			new_entry.type = E820_TYPE_RAM;
			continue;
		}

		/* This entry is within the range of specific memory
		 * change to E820_TYPE_RESERVED
		 */
		if ((entry_start >= start_pa) && (entry_end <= end_pa)) {
			entry->type = E820_TYPE_RESERVED;
			continue;
		}

		if ((entry_start >= start_pa) && (entry_start < end_pa) && (entry_end > end_pa)) {
			entry->baseaddr = end_pa;
			entry->length = entry_end - end_pa;
			continue;
		}
	}

	if (new_entry.length > 0UL) {
		entries_count++;
		ASSERT(entries_count <= E820_MAX_ENTRIES, "e820 entry overflow");
		entry = &sos_vm_e820[entries_count - 1U];
		entry->baseaddr = new_entry.baseaddr;
		entry->length = new_entry.length;
		entry->type = new_entry.type;
		vm->e820_entry_num = entries_count;
	}

}

/**
 * before boot sos_vm(service OS), call it to hide HV and prelaunched VM memory in e820 table from sos_vm
 *
 * @pre vm != NULL
 */
void create_sos_vm_e820(struct acrn_vm *vm)
{
	uint16_t vm_id, i;
	uint64_t hv_start_pa = hva2hpa((void *)(get_hv_image_base()));
	uint64_t hv_end_pa  = hv_start_pa + CONFIG_HV_RAM_SIZE;
	uint32_t entries_count = get_e820_entries_count();
	struct acrn_vm_config *sos_vm_config = get_vm_config(vm->vm_id);

	(void)memcpy_s((void *)sos_vm_e820, entries_count * sizeof(struct e820_entry),
			(const void *)get_e820_entry(), entries_count * sizeof(struct e820_entry));

	vm->e820_entry_num = entries_count;
	vm->e820_entries = sos_vm_e820;
	/* filter out hv memory from e820 table */
	filter_mem_from_sos_e820(vm, hv_start_pa, hv_end_pa);

	/* filter out prelaunched vm memory from e820 table */
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct acrn_vm_config *vm_config = get_vm_config(vm_id);

		if (vm_config->load_order == PRE_LAUNCHED_VM) {
			filter_mem_from_sos_e820(vm, vm_config->memory.start_hpa,
					vm_config->memory.start_hpa + vm_config->memory.size);
		}
	}

	for (i = 0U; i < vm->e820_entry_num; i++) {
		struct e820_entry *entry = &sos_vm_e820[i];
		if ((entry->type == E820_TYPE_RAM)) {
			sos_vm_config->memory.size += entry->length;
		}
	}
	sort_vm_e820(vm);
}

static const struct e820_entry pre_ve820_template[E820_MAX_ENTRIES] = {
	{	/* usable RAM under 1MB */
		.baseaddr = 0x0UL,
		.length   = 0xA0000UL,		/* 640KB */
		.type     = E820_TYPE_RAM
	},
	{	/* Video/BIOS extentions */
		.baseaddr = 0xA0000UL,
		.length   = 0x60000UL,		/* 384KB */
		.type     = E820_TYPE_RESERVED
	},
	/* Software SRAM segment splits the lowmem into two parts */
	{	/* part1 of lowmem of hpa1*/
		.baseaddr = MEM_1M,		/* 1MB */
		.length   = PRE_RTVM_SW_SRAM_BASE_GPA - MEM_1M,
		.type     = E820_TYPE_RAM
	},
	{	/* Software SRAM */
		.baseaddr = PRE_RTVM_SW_SRAM_BASE_GPA,
		.length   = PRE_RTVM_SW_SRAM_MAX_SIZE,
		.type     = E820_TYPE_RESERVED
	},
	{	/* part2 of lowmem of hpa1*/
		.baseaddr = PRE_RTVM_SW_SRAM_BASE_GPA + PRE_RTVM_SW_SRAM_MAX_SIZE,
		.length   = GPU_OPREGION_GPA - (PRE_RTVM_SW_SRAM_BASE_GPA + PRE_RTVM_SW_SRAM_MAX_SIZE),
		.type     = E820_TYPE_RAM
	},
	{	/* GPU OpRegion for pre-launched VM */
		.baseaddr = GPU_OPREGION_GPA,
		.length   = GPU_OPREGION_SIZE,
		.type     = E820_TYPE_RESERVED
	},
	{	/* ACPI Reclaim */
		.baseaddr = VIRT_ACPI_DATA_ADDR, /* consecutive after the last area */
		.length   = VIRT_ACPI_DATA_LEN,
		.type	  = E820_TYPE_ACPI_RECLAIM
	},
	{	/* ACPI NVS */
		.baseaddr = VIRT_ACPI_NVS_ADDR,	/* consecutive after ACPI Reclaim */
		.length   = VIRT_ACPI_NVS_LEN,
		.type	  = E820_TYPE_ACPI_NVS
	},
	{	/* 32bit PCI hole */
		.baseaddr = 0x80000000UL,	/* 2048MB */
		.length   = MEM_2G,
		.type     = E820_TYPE_RESERVED
	},
};

/**
 * @pre entry != NULL
 */
static inline uint64_t add_ram_entry(struct e820_entry *entry, uint64_t gpa, uint64_t length)
{
	entry->baseaddr = gpa;
	entry->length = length;
	entry->type = E820_TYPE_RAM;
	return round_pde_up(entry->baseaddr + entry->length);
}

/**
 * @pre vm != NULL
 *
 * ve820 layout for pre-launched VM:
 *
 *   entry0: usable under 1MB
 *   entry1: reserved for Video/BIOS extentions from 0xa0000 to 0xfffff
 *   entry2: usable, the part1 of hpa1 in lowmem, from 0x100000,
 *           and up to the bottom of Software SRAM area.
 *   entry3: reserved, Software SRAM segment, which will be identically mapped to physical
 *           Software SRAM segment rather than hpa1.
 *   entry4: usable, the part2 of hpa1 in lowmem, from the ceil of Software SRAM segment,
 *           and up to 2G-(size of ACPI and GPU reserved areas).
 *   entry5: reserved, GPU OpRegion.
 *   entry6: ACPI Reclaim from 0x7fe00000 to 0x7fefffff
 *   entry7: ACPI NVS from 0x7fff0000 to 0x7fffffff
 *   entry8: reserved for 32bit PCI hole from 0x80000000 to 0xffffffff
 *   (entry9): usable for
 *             hpa1_hi, if hpa1 > 2GB - PRE_RTVM_SW_SRAM_MAX_SIZE
 */

/*
	The actual memory mapping under 2G looks like below:
	|<--1M-->|
	|<-----hpa1_low_part1--->|
	|<---Software SRAM--->|
	|<-----hpa1_low_part2--->|
	|<---Non-mapped hole (if there is)-->|
	|<---GPU_OpRegion--->|
	|<---(1M + 1M) ACPI NVS/DATA--->|
*/
void create_prelaunched_vm_e820(struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);
	uint64_t gpa_start = 0x100000000UL;
	uint64_t hpa1_hi_size;
	uint64_t lowmem_max_length = MEM_2G - PRE_RTVM_SW_SRAM_MAX_SIZE;
	/* the 'MEM_1M' symble here and below stands for the size of 0-0x100000 area */
	uint64_t hpa1_part1_max_length = PRE_RTVM_SW_SRAM_BASE_GPA - MEM_1M;
	uint32_t entry_idx = ENTRY_HPA1_HI;

	vm->e820_entries = pre_vm_e820[vm->vm_id];
	(void)memcpy_s((void *)vm->e820_entries,  E820_MAX_ENTRIES * sizeof(struct e820_entry),
		(const void *)pre_ve820_template, E820_MAX_ENTRIES * sizeof(struct e820_entry));

	/* sanitize entry for hpa1 */
	if (vm_config->memory.size > lowmem_max_length) {
		/* need to split hpa1 and add an entry for hpa1_hi */
		hpa1_hi_size = vm_config->memory.size - lowmem_max_length;
		gpa_start = add_ram_entry((vm->e820_entries + entry_idx), gpa_start, hpa1_hi_size);
		entry_idx++;
	} else if (vm_config->memory.size - MEM_1M - GPU_OPREGION_SIZE - VIRT_ACPI_DATA_LEN - VIRT_ACPI_NVS_LEN <=
		hpa1_part1_max_length) {
		/*
		 * In this case, hpa1 is only enough for the first
		 * 1M + part1 + last (1M + 1M) (ACPI NVS/DATA), so part2 will be empty.
		 */
		vm->e820_entries[ENTRY_HPA1_LOW_PART1].length =
			vm_config->memory.size - MEM_1M - GPU_OPREGION_SIZE - VIRT_ACPI_DATA_LEN - VIRT_ACPI_NVS_LEN;
		vm->e820_entries[ENTRY_HPA1_LOW_PART2].length = 0;
	} else {
		/* Otherwise, part2 is not empty. */
		vm->e820_entries[ENTRY_HPA1_LOW_PART2].length =
			vm_config->memory.size - PRE_RTVM_SW_SRAM_BASE_GPA - GPU_OPREGION_SIZE - VIRT_ACPI_DATA_LEN - VIRT_ACPI_NVS_LEN;
		/* need to set gpa_start for hpa2 */
	}

	vm->e820_entry_num = entry_idx;
	sort_vm_e820(vm);
}
