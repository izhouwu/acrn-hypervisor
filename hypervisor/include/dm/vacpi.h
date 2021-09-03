/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VACPI_H
#define VACPI_H

#include <acpi.h>
#include <asm/guest/vm.h>

#define ACPI_OEM_ID           "ACRN  "

/* Use a pre-loaded multiboot module as pre-launched VM ACPI table.
 * The module file size is fixed to 1MB and loaded to GPA 0x7fe00000.
 * A hardcoded RSDP table at GPA 0x000f2400 will point to the XSDT
 * table which at GPA 0x7fe00080;
 * The module file should be generated by acrn-config tool;
 */
#define VIRT_ACPI_DATA_LEN	MEM_1M
#define VIRT_ACPI_NVS_LEN	MEM_1M
/* Currently ACPI NVS start addr is presumed to be consecutive with ACPI DATA area right below 0x80000000 */
#define VIRT_ACPI_NVS_ADDR	(0x80000000UL - VIRT_ACPI_NVS_LEN)
#define VIRT_ACPI_DATA_ADDR	(VIRT_ACPI_NVS_ADDR - VIRT_ACPI_DATA_LEN)
#define VIRT_RSDP_ADDR		0x000f2400UL
#define VIRT_XSDT_ADDR		0x7fe00080UL

/* virtual PCI MMCFG address base for pre/post-launched VM. */
#define USER_VM_VIRT_PCI_MMCFG_BASE		0xE0000000UL
#define USER_VM_VIRT_PCI_MMCFG_START_BUS	0x0U
#define USER_VM_VIRT_PCI_MMCFG_END_BUS	0xFFU
#define USER_VM_VIRT_PCI_MEMBASE32      0x80000000UL    /* 2GB */
#define USER_VM_VIRT_PCI_MEMLIMIT32     0xE0000000UL    /* 3.5GB */
#define USER_VM_VIRT_PCI_MEMBASE64      0x4000000000UL   /* 256GB */
#define USER_VM_VIRT_PCI_MEMLIMIT64     0x8000000000UL   /* 512GB */

void build_vrsdp(struct acrn_vm *vm);

#endif /* VACPI_H */
