/*
 * Copyright (C) 2018-2022 Intel Corporation.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/queue.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <acrn_common.h>

#include "dm_vm_event.h"
#include "hsm_ioctl_defs.h"
#include "sbuf.h"
#include "log.h"

static struct shared_buf *ve_sbuf = NULL;
static int kick_fd;
static int epoll_fd;
static pthread_t vm_event_tid;
static bool started = false;

#define MAX_EPOLL_EVENTS 1

static void *vm_event_thread(void *param)
{
	int n;
	struct vm_event ve;
    eventfd_t val;

	struct epoll_event eventlist[1];

	pr_notice("vm event thread running");

	while (1) {
		n = epoll_wait(epoll_fd, eventlist, MAX_EPOLL_EVENTS, -1);
		if (n < 0) {
			if (errno != EINTR) {
				pr_err("%s: epoll failed %d\n", __func__, errno);				
			}
			continue;
		}
		pr_notice("vm event kicked %x %x\n", ve_sbuf->head, ve_sbuf->tail);
		if (eventlist[0].data.ptr) {
        	eventfd_read(*(int *)(eventlist[0].data.ptr), &val);
		}

		while (sbuf_get(ve_sbuf, (uint8_t*)&ve) > 0) {
			pr_notice("vm event %d %s\n", ve.type, (char*)ve.event_data);
		}		
	}
	return NULL;
}

int vm_init_vm_event(struct vmctx *ctx, uint64_t base)
{
	struct shared_buf *sbuf = (struct shared_buf *)base;
	int ret = -1, error;
	struct epoll_event ev;

	sbuf->magic = SBUF_MAGIC;
	sbuf->ele_size = sizeof(struct vm_event);
	sbuf->ele_num = (4096 - SBUF_HEAD_SIZE) / sbuf->ele_size;
	sbuf->size = sbuf->ele_size * sbuf->ele_num;
	sbuf->flags = 0;
	sbuf->overrun_cnt = 0;
	sbuf->head = 0;
	sbuf->tail = 0;
	ve_sbuf = sbuf;

	pr_notice("vm event sbuf %lx\n", base);
	error = ioctl(ctx->fd, ACRN_IOCTL_SETUP_VM_EVENT_RING, base);
	if (error) {
		pr_err("%s: Setting vm_event ring failed %d\n", __func__, error);
		goto out;
	}

	kick_fd = eventfd(0, 0);
	if (kick_fd < 0) {
		pr_err("%s: eventfd failed %d\n", __func__, errno);
		goto out;
	}

	error = ioctl(ctx->fd, ACRN_IOCTL_SETUP_VM_EVENT_FD, kick_fd);
	if (error) {
		pr_err("%s: Setting vm_event fd failed %d\n", __func__, error);
		goto err1;
	}

	epoll_fd = epoll_create1(0);
	if (epoll_fd < 0) {
		pr_err("%s: failed to create epoll %d\n", __func__, errno);
		goto err1;
	}
	pr_notice("epoll fd %d %d\n", epoll_fd, kick_fd);
	ev.events = EPOLLIN;
	ev.data.ptr = &kick_fd;
	error = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, kick_fd, &ev);
	if (error < 0) {
		pr_err("%s: failed to add fd, error is %d\n", __func__, errno);
		return ret;
	}

	error = pthread_create(&vm_event_tid, NULL, vm_event_thread, NULL);
	if (error) {
		pr_err("%s: vm_event create failed %d\n", __func__, errno);
		goto err2;
	}

	ret = 0;
	started = true;
	return 0;

err2:
	close(epoll_fd);
err1:
	close(kick_fd);
out:
	return ret;
}

int vm_event_deinit(void)
{
	void *jval;

	if (started) {
		pthread_kill(vm_event_tid, SIGCONT);
		pthread_join(vm_event_tid, &jval);
		close(epoll_fd);
		close(kick_fd);	
		started = false;	
	}
	return 0;
}

