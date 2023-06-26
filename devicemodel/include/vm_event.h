/*
 * Copyright (C) 2019-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VM_EVENT_H
#define VM_EVENT_H

#include <types.h>
#include <acrn_common.h>
#include "vmmapi.h"

int vm_init_vm_event(struct vmctx *ctx);
int vm_event_deinit(void);
/* Put vm event to sbuf. For vm event thread to transmit out. */
int send_dm_vm_event(struct vm_event *event);

#endif /* VM_EVENT_H */
