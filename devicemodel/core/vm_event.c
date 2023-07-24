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

#include "vm_event.h"
#include "hsm_ioctl_defs.h"
#include "sbuf.h"
#include "log.h"
#include <cjson/cJSON.h>
#include "monitor.h"

#define VM_EVENT_ELE_SIZE (sizeof(struct vm_event))

#define HV_VM_EVENT_TUNNEL 0
#define DM_VM_EVENT_TUNNEL 1
#define MAX_VM_EVENT_TUNNELS 2
#define MAX_EPOLL_EVENTS MAX_VM_EVENT_TUNNELS

typedef void (*vm_event_handler)(struct vmctx *ctx, struct vm_event *event);

static int epoll_fd;
static bool started = false;
static char hv_vm_event_page[4096] __aligned(4096);
static char dm_vm_event_page[4096] __aligned(4096);
static pthread_t vm_event_tid;

enum event_source_type {
	EVENT_SOURCE_TYPE_HV,
	EVENT_SOURCE_TYPE_DM,
};

struct vm_event_tunnel {
	enum event_source_type type;
	struct shared_buf *sbuf;
	uint32_t sbuf_size;
	int kick_fd;
	pthread_mutex_t mtx;
	bool enabled;
};

enum vm_event_data_element_type {
	VE_INVALID = 0,
	VE_U8_TYPE,
	VE_U32_TYPE,
	VE_U64_TYPE,
	VE_STR_TYPE,
	VE_BOOL_TYPE,
	VE_TIMET_TYPE,
};

#define MAX_ELEMENT_NUM 32

struct ve_element {
	enum vm_event_data_element_type el_type;
	char *name;
	uint64_t offset_in_struct;
}; 

struct vm_event_json_trans_table {
	uint32_t type;
	struct ve_element ele[MAX_ELEMENT_NUM];
};

#define OFFSET_IN_STRUCT(s, e)  ((uint64_t)&((struct s *)0)->e)
#define VM_EVENT_DATA_ELE(s, e, t) {t, #e, OFFSET_IN_STRUCT(s, e)}

static struct vm_event_json_trans_table trans[] = {

};

static char *generate_vm_event_message(struct vm_event *event)
{
	char *event_msg = NULL;
	cJSON *val;
	cJSON *event_obj = cJSON_CreateObject();
	int i, j;

	if (event_obj == NULL)
		return NULL;
	val = cJSON_CreateNumber(event->type);
	if (val == NULL)
		return NULL;
	cJSON_AddItemToObject(event_obj, "vm_event", val);

	for (i = 0; i < ARRAY_SIZE(trans); i++){
		if (event->type == trans[i].type) {
			break;
		}
	}

	if(i >= ARRAY_SIZE(trans)) {
		goto out;
	}

	for (j = 0; j < MAX_ELEMENT_NUM; j ++) {
		struct ve_element *pve = &(trans[i].ele[j]);
		if (pve->el_type == VE_INVALID) {
			break;
		}
		switch(pve->el_type) {
			case VE_U8_TYPE:
			{
				uint8_t num = *(uint8_t *)(((uint8_t *)event->event_data) + pve->offset_in_struct);
				cJSON *obj = cJSON_CreateNumber((double)num);
				if (obj) {
					cJSON_AddItemToObject(event_obj, pve->name, obj);
				}					
				break;
			}
			case VE_TIMET_TYPE:
			{
				time_t num = *(time_t *)(((time_t *)event->event_data) + pve->offset_in_struct);
				cJSON *obj = cJSON_CreateNumber((double)num);
				if (obj) {
					cJSON_AddItemToObject(event_obj, pve->name, obj);
				}					
				break;
			}
			case VE_STR_TYPE:
				break;
			case VE_BOOL_TYPE:
				break;
			default:
				break;
		}

	}
out:
	event_msg = cJSON_Print(event_obj);
	if (event_msg == NULL)
		fprintf(stderr, "Failed to generate vm_event message.\n");

	cJSON_Delete(event_obj);
	return event_msg;
}

static void general_event_handler(struct vmctx *ctx, struct vm_event *event)
{
	char *msg = generate_vm_event_message(event);
	if (msg != NULL) {
		vm_monitor_send_vm_event(msg);
		free(msg);
	}
}

static vm_event_handler ve_handler[VM_EVENT_COUNT] = {
	[VM_EVENT_SET_RTC] = general_event_handler,
	[VM_EVENT_POWEROFF] = general_event_handler,
	[VM_EVENT_TRIPPLE_FAULT] = general_event_handler,
};

static void *vm_event_thread(void *param)
{
	int n, i;
	struct vm_event ve;
    eventfd_t val;
	struct vm_event_tunnel *tunnel;
	struct vmctx *ctx = param;

	struct epoll_event eventlist[MAX_EPOLL_EVENTS];

	pr_notice("vm event thread running");

	while (1) {
		n = epoll_wait(epoll_fd, eventlist, MAX_EPOLL_EVENTS, -1);
		if (n < 0) {
			if (errno != EINTR) {
				pr_err("%s: epoll failed %d\n", __func__, errno);				
			}
			continue;
		}
		for (i = 0; i < n; i++) {
			if (i < MAX_EPOLL_EVENTS) {

			}
			tunnel = eventlist[i].data.ptr;
			if (tunnel && tunnel->enabled) {
				//pr_notice("vm event kicked from %d\n", tunnel->type);
				while (!sbuf_is_empty(tunnel->sbuf)) {
					sbuf_get(tunnel->sbuf, (uint8_t*)&ve);
					eventfd_read(tunnel->kick_fd, &val);
					pr_notice("%ld vm event from%d %d\n", val, tunnel->type,
						ve.type);
					if (ve.type < VM_EVENT_COUNT && ve_handler[ve.type] != NULL) {
						(ve_handler[ve.type])(ctx, &ve);
					} else {
						pr_err("%s: unhandled vm event type %d\n", __func__, ve.type);
					}

				}
			}
		}
	}
	return NULL;
}

static struct vm_event_tunnel ve_tunnel[MAX_VM_EVENT_TUNNELS] = {
	{
		.type = EVENT_SOURCE_TYPE_HV,
		.sbuf = (struct shared_buf *)hv_vm_event_page,
		.sbuf_size = 4096,
		.enabled = false,
	},
	{
		.type = EVENT_SOURCE_TYPE_DM,
		.sbuf = (struct shared_buf *)dm_vm_event_page,
		.sbuf_size = 4096,
		.enabled = false,
	},	
};

static int create_event_tunnel(struct vmctx *ctx, struct vm_event_tunnel *tunnel, int epoll_fd)
{
	struct epoll_event ev;
	enum event_source_type type = tunnel->type;
	struct shared_buf *sbuf = tunnel->sbuf;
	int kick_fd = -1;
	int error;

	sbuf_init(sbuf, tunnel->sbuf_size, VM_EVENT_ELE_SIZE);

	if (type == EVENT_SOURCE_TYPE_HV) {
		error = ioctl(ctx->fd, ACRN_IOCTL_SETUP_VM_EVENT_RING, sbuf);
		if (error) {
			pr_err("%s: Setting vm_event ring failed %d\n", __func__, error);
			goto out;
		}
	}

	kick_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (kick_fd < 0) {
		pr_err("%s: eventfd failed %d\n", __func__, errno);
		goto out;
	}

	if (type == EVENT_SOURCE_TYPE_HV) {
		error = ioctl(ctx->fd, ACRN_IOCTL_SETUP_VM_EVENT_FD, kick_fd);
		if (error) {
			pr_err("%s: Setting vm_event fd failed %d\n", __func__, error);
			goto out;
		}
	}

	ev.events = EPOLLIN;
	ev.data.ptr = tunnel;
	error = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, kick_fd, &ev);
	if (error < 0) {
		pr_err("%s: failed to add fd, error is %d\n", __func__, errno);
		goto out;
	}

	tunnel->kick_fd = kick_fd;
	pthread_mutex_init(&tunnel->mtx, NULL);
	tunnel->enabled = true;

	return 0;

out:
	if (kick_fd >= 0) {
		close(kick_fd);
	}
	return -1;
}

void destory_event_tunnel(struct vm_event_tunnel *tunnel)
{
	if (tunnel->enabled) {
		close(tunnel->kick_fd);
		tunnel->enabled = false;
		pthread_mutex_destroy(&tunnel->mtx);
	}
}

int vm_init_vm_event(struct vmctx *ctx)
{
	int error;

	epoll_fd = epoll_create1(0);
	if (epoll_fd < 0) {
		pr_err("%s: failed to create epoll %d\n", __func__, errno);
		goto out;
	}

	error = create_event_tunnel(ctx, &ve_tunnel[HV_VM_EVENT_TUNNEL], epoll_fd);
	if (error) {
		goto out;
	}

	error = create_event_tunnel(ctx, &ve_tunnel[DM_VM_EVENT_TUNNEL], epoll_fd);
	if (error) {
		goto out;
	}

	error = pthread_create(&vm_event_tid, NULL, vm_event_thread, ctx);
	if (error) {
		pr_err("%s: vm_event create failed %d\n", __func__, errno);
		goto out;
	}

	started = true;
	return 0;

out:
	if (epoll_fd >= 0) {
		close(epoll_fd);
	}
	destory_event_tunnel(&ve_tunnel[HV_VM_EVENT_TUNNEL]);
	destory_event_tunnel(&ve_tunnel[DM_VM_EVENT_TUNNEL]);
	return -1;
}

int vm_event_deinit(void)
{
	void *jval;

	if (started) {
		pthread_kill(vm_event_tid, SIGCONT);
		pthread_join(vm_event_tid, &jval);
		close(epoll_fd);
		destory_event_tunnel(&ve_tunnel[HV_VM_EVENT_TUNNEL]);
		destory_event_tunnel(&ve_tunnel[DM_VM_EVENT_TUNNEL]);
		started = false;	
	}
	return 0;
}

/* Send a dm generated vm_event by putting it to sbuf
 * We have a dedicated thread in dm to receive those events.
 */
int dm_send_vm_event(struct vm_event *event)
{
	struct vm_event_tunnel *tunnel = &ve_tunnel[DM_VM_EVENT_TUNNEL];
	struct shared_buf *sbuf;
	int32_t ret = -ENODEV;
	uint32_t size_sent;

	if (!tunnel->enabled) {
		return -1;
	}
	sbuf = tunnel->sbuf;

	if (sbuf != NULL) {
		pthread_mutex_lock(&tunnel->mtx);
		size_sent = sbuf_put(sbuf, (uint8_t *)event);
		pthread_mutex_unlock(&tunnel->mtx);
		if (size_sent == VM_EVENT_ELE_SIZE) {
			eventfd_write(tunnel->kick_fd, 1UL);
			ret = 0;
		}
	}
	return ret;
}
