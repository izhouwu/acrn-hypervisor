/*
 * Copyright (C) 2019-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef DM_VM_EVENT_H
#define DM_VM_EVENT_H

#include <types.h>
#include <acrn_common.h>
#include "vmmapi.h"

int vm_init_vm_event(struct vmctx *ctx, uint64_t base);
int vm_event_deinit(void);

#endif /* DM_VM_EVENT_H */
