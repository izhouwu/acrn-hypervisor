/*
 * Copyright (C) 2020-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <util.h>
#include <acrn_hv_defs.h>
#include <asm/guest/vm.h>
#include <debug/logmsg.h>
#include <vm_event.h>
#include <sbuf.h>

/////////////////////
#include <sprintf.h>

int32_t init_vm_event(struct acrn_vm *vm, uint64_t *hva)
{
	struct shared_buf *sbuf = (struct shared_buf *)hva;
	int ret = -1;
	
	pr_acrnlog("vm%d %lx", vm->vm_id, (uint64_t)hva);

	stac();
	if (sbuf != NULL) {
		if (sbuf->magic == SBUF_MAGIC) {
			vm->sw.vm_event_sbuf = sbuf;
			pr_acrnlog("vm event sbuf %lx", (uint64_t)hva);
			spinlock_init(&vm->vm_event_lock);
			ret = 0;
		}
	}
	clac();

	return ret;
}

int32_t send_vm_event(struct acrn_vm *vm, struct vm_event *event)
{
	struct shared_buf *sbuf = (struct shared_buf *)vm->sw.vm_event_sbuf;
	int32_t ret = -ENODEV;
    uint32_t size_sent;

	if (sbuf != NULL) {
		spinlock_obtain(&vm->vm_event_lock);
		size_sent = sbuf_put(sbuf, (uint8_t *)event);
		spinlock_release(&vm->vm_event_lock);
        if (size_sent == sizeof(struct vm_event)) {
            arch_fire_hsm_interrupt();
			ret = 0;
        }		
	}
    return ret;
}

void send_test_msg(void *data)
{
	struct acrn_vm *vm = (struct acrn_vm *)data;
	struct vm_event ve;
	static int index=0;
	ve.type = 7;
	snprintf((char*)ve.event_data, 28, "hello from vm%d i%d", vm->vm_id, index++);
	send_vm_event(vm, &ve);
	pr_acrnlog("msg sent for %d", vm->vm_id);
}