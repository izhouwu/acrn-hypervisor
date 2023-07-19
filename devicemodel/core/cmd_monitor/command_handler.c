/*
 * Copyright (C) 2022 Intel Corporation.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <libgen.h>
#include <cjson/cJSON.h>
#include "command.h"
#include "socket.h"
#include "command_handler.h"
#include "dm.h"
#include "pm.h"
#include "vmmapi.h"
#include "log.h"
#include "monitor.h"

#define SUCCEEDED 0
#define FAILED -1

static char *generate_ack_message(int ret_val)
{
	char *ack_msg;
	cJSON *val;
	cJSON *ret_obj = cJSON_CreateObject();

	if (ret_obj == NULL)
		return NULL;
	val = cJSON_CreateNumber(ret_val);
	if (val == NULL)
		return NULL;
	cJSON_AddItemToObject(ret_obj, "ack", val);
	ack_msg = cJSON_Print(ret_obj);
	if (ack_msg == NULL)
		fprintf(stderr, "Failed to generate ACK message.\n");
	cJSON_Delete(ret_obj);
	return ack_msg;
}
static int send_socket_ack(struct socket_dev *sock, int fd, bool normal)
{
	int ret = 0, val;
	char *ack_message;
	struct socket_client *client = NULL;

	client = find_socket_client(sock, fd);
	if (client == NULL)
		return -1;
	val = normal ? SUCCEEDED : FAILED;
	ack_message = generate_ack_message(val);

	if (ack_message != NULL) {
		memset(client->buf, 0, CLIENT_BUF_LEN);
		memcpy(client->buf, ack_message, strlen(ack_message));
		client->len = strlen(ack_message);
		ret = write_socket_char(client);
		free(ack_message);
		pr_notice("ack send to fd %d\n", client->fd);
	} else {
		pr_err("Failed to generate ACK message.\n");
		ret = -1;
	}
	return ret;
}

static struct socket_client *vm_event_client = NULL;
static pthread_mutex_t vm_event_client_mutex = PTHREAD_MUTEX_INITIALIZER;

static void vm_event_free_cb(struct socket_client *self)
{
	vm_event_client = NULL;
}

static int set_vm_event_client(struct socket_client *client)
{
	if (vm_event_client != NULL) {
		pr_err("vm event client already registerred.\n");
		return -1;
	} else {
		vm_event_client = client;
		client->per_client_mutex = &vm_event_client_mutex;
		client->free_client_cb = vm_event_free_cb;
		return 0;
	}
}

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
	{
		VM_EVENT_SET_RTC,
		{
			VM_EVENT_DATA_ELE(set_rtc_event_data, time, VE_TIMET_TYPE),
			VM_EVENT_DATA_ELE(set_rtc_event_data, yy, VE_U8_TYPE),
			VM_EVENT_DATA_ELE(set_rtc_event_data, mm, VE_U8_TYPE),
			VM_EVENT_DATA_ELE(set_rtc_event_data, dm, VE_U8_TYPE),
			VM_EVENT_DATA_ELE(set_rtc_event_data, dw, VE_U8_TYPE),
			VM_EVENT_DATA_ELE(set_rtc_event_data, hh, VE_U8_TYPE),
			VM_EVENT_DATA_ELE(set_rtc_event_data, mi, VE_U8_TYPE),
			VM_EVENT_DATA_ELE(set_rtc_event_data, ss, VE_U8_TYPE),
			VM_EVENT_DATA_ELE(set_rtc_event_data, century, VE_U8_TYPE),
		},

	},

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

int vm_monitor_send_vm_event(struct vm_event *event)
{
	int ret = -1;
	struct socket_client *client;
	char *msg = generate_vm_event_message(event);
	if (msg == NULL) {
		pr_err("%s: failed to generate vm_event msg\n", __func__);
		return -1;
	}

	pthread_mutex_lock(&vm_event_client_mutex);
	client = vm_event_client;
	if (client == NULL) {
		goto fail_out;
	}
	memset(client->buf, 0, CLIENT_BUF_LEN);
	memcpy(client->buf, msg, strlen(msg));
	client->len = strlen(msg);
	ret = write_socket_char(client);

fail_out:
	free(msg);
	pthread_mutex_unlock(&vm_event_client_mutex);
	return ret;
}

int user_vm_register_vm_event_client_handler(void *arg, void *command_para)
{
	int ret;
	struct command_parameters *cmd_para = (struct command_parameters *)command_para;
	struct handler_args *hdl_arg = (struct handler_args *)arg;
	struct socket_dev *sock = (struct socket_dev *)hdl_arg->channel_arg;
	struct socket_client *client = NULL;
	bool cmd_completed = false;

	client = find_socket_client(sock, cmd_para->fd);
	if (client == NULL)
		return -1;

	if (set_vm_event_client(client) == 0) {
		cmd_completed = true;
	}
	pr_err("client with fd %d registerred\n", client->fd);
	
	ret = send_socket_ack(sock, cmd_para->fd, cmd_completed);
	if (ret < 0) {
		pr_err("Failed to send ACK message by socket.\n");
	}
	return ret;
}

int user_vm_destroy_handler(void *arg, void *command_para)
{
	int ret;
	struct command_parameters *cmd_para = (struct command_parameters *)command_para;
	struct handler_args *hdl_arg = (struct handler_args *)arg;
	struct socket_dev *sock = (struct socket_dev *)hdl_arg->channel_arg;
	struct socket_client *client = NULL;
	bool cmd_completed = false;

	client = find_socket_client(sock, cmd_para->fd);
	if (client == NULL)
		return -1;

	if (!is_rtvm) {
		pr_info("%s: setting VM state to %s.\n", __func__, vm_state_to_str(VM_SUSPEND_POWEROFF));
		vm_set_suspend_mode(VM_SUSPEND_POWEROFF);
		cmd_completed = true;
	} else {
		pr_err("Failed to destroy post-launched RTVM.\n");
		ret = -1;
	}

	ret = send_socket_ack(sock, cmd_para->fd, cmd_completed);
	if (ret < 0) {
		pr_err("Failed to send ACK message by socket.\n");
	}
	return ret;
}

int user_vm_blkrescan_handler(void *arg, void *command_para)
{
	int ret = 0;
	struct command_parameters *cmd_para = (struct command_parameters *)command_para;
	struct handler_args *hdl_arg = (struct handler_args *)arg;
	struct socket_dev *sock = (struct socket_dev *)hdl_arg->channel_arg;
	struct socket_client *client = NULL;
	bool cmd_completed = false;

	client = find_socket_client(sock, cmd_para->fd);
	if (client == NULL)
		return -1;

	ret = vm_monitor_blkrescan(hdl_arg->ctx_arg, cmd_para->option);
	if (ret >= 0) {
		cmd_completed = true;
	} else {
		pr_err("Failed to rescan virtio-blk device.\n");
	}

	ret = send_socket_ack(sock, cmd_para->fd, cmd_completed);
	if (ret < 0) {
		pr_err("Failed to send ACK by socket.\n");
	}
	return ret;
}
