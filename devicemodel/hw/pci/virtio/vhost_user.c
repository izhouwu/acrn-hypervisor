/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */


#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stddef.h>
#include <pthread.h>

#include "pci_core.h"
#include "vmmapi.h"
#include "vhost.h"

static int vhost_user_debug;
#define LOG_TAG "vhost_user: "
#define DPRINTF(fmt, args...) \
	do { if (vhost_user_debug) pr_err(LOG_TAG fmt, ##args); } while (0)
#define WPRINTF(fmt, args...) pr_err(LOG_TAG fmt, ##args)

#define MAX_VM_MEM_REGION 32
#define MAX_FS_SLAVE_ENTRIES 8

/* socket msgs sent from acrn-dm to vhost-user daemon */
enum VHOST_USER_REQUEST {
	/* NONE operation */
	NONE_REQUEST = 0,
	/* Get the underlying vhost implementation feature bits */
	GET_FEATURE_BITS = 1,
	/* Set the underlying vhost implementation feature bits */
	SET_FEATURE_BITS = 2,
	/* Set the sender as the owner of this session */
	SET_OWNER = 3,
	/* No longer used */
	RESET_OWNER = 4,
	/* Set the memory map regions on the back-end so it can translate the vring addresses. */
	SET_MEM_TABLE = 5,
	/* Set logging shared memory space */
	SET_LOG_BASE = 6,
	/* Set the logging fd */
	SET_LOG_FD = 7,
	/* Set the size of the queue */
	SET_VIRTQ_NUM = 8,
	/* Set the addresses of the vring */
	SET_VIRTQ_ADDR = 9,
	/* Set the base offset in the available vring */
	SET_VIRTQ_BASE = 10,
	/* Get the available vring base offset */
	GET_VIRTQ_BASE = 11,
	/* Set the event fd for adding buffers to the vring */
	SET_VIRTQ_KICKFD = 12,
	/* Set the event fd to signal when buffers are used */
	SET_VIRTQ_CALLFD = 13,
	/* Set the event fd to signal when error occurs. */
	SET_VIRTQ_ERRFD = 14,
	/* Get vhost-user protocol feature */
	GET_PROTOCOL_FEATURE_BITS = 15,
	/* Set vhost-user protocol feature */
	SET_PROTOCOL_FEATURE_BITS = 16,
	/* Query how many queues the back-end supports */
	GET_QUEUE_NUM = 17,
	/* Signal the back-end to enable or disable corresponding vring. */
	SET_VRING_ENABLE = 18,
	/* Ask back-end to broadcast a fake RARP to notify the migration is terminated for guest */
	SEND_RAPP = 19,
	/* Set host MTU value exposed to the guest */
	SET_NET_MTU = 20,
	/* Set the socket fd for back-end initiated requests */
	SET_BACKEND_REQ_FD = 21,
	/* Update device IOTLB */
	SEND_IOTLB_MSG = 22,
	/* Set vring endian  */
	SET_VRING_ENDIAN = 23,
	/* Get device's virtio config */
	GET_CONFIG = 24,
	/* Set device's virtio config */
	SET_CONFIG = 25,
	/* Create a session for crypto operation */
	CREATE_CRYPTO_SESSION = 26,
	/* Close a crypto session */
	CLOSE_CRYPTO_SESSION = 27,
	/* Advise slave that a migration with postcopy enabled is underway */
	POSTCOPY_ADVISE = 28,
	/* Advise slave that a transition to postcopy mode has happened */
	POSTCOPY_LISTEN = 29,
	/* The front-end advises that postcopy migration has now completed */
	POSTCOPY_END = 30,
	/* Get a shared buffer from slave */
	GET_INFLIGHT_FD = 31,
	/* Send the shared inflight buffer back to slave */
	SET_INFLIGHT_FD = 32,
	/* Sets the GPU protocol socket fd */
	GPU_SET_SOCKET = 33,
	/* Reset device to initial state */
	RESET_DEVICE = 34,
	/* Kick back_end with msg, not using the vring’s kick fd */
	VRING_KICK = 35,
	/* Maximum number of memory slots */
	GET_MAX_MEM_SLOTS = 36,
	/* Add a memory region */
	ADD_MEM_REG = 37,
	/* Delete a memory region */
	DEL_MEM_REG = 38,
	/* Set device status */
	SET_STATUS = 39,
	/* Get device status */
	GET_STATUS = 40,

	VHOST_USER_REQUEST_MAX = 41,
};

/* socket msg sent from daemon to acrn-dm */
enum SLAVE_REQUEST {
	/* Send IOTLB messages */
	IOTLB_MSG = 1,
	/* Notify that the virtio device’s configuration space has changed */
	CONFIG_CHANGE_MSG = 2,
	/* Set host notifier for a specified queue */
	VRING_HOST_NOTIFIER_MSG = 3,
	/* Vring call mag instead of signalling call-fd */
	VRING_CALL = 4,
	/* Err mag instead of signalling err-fd */
	VRING_ERR = 5,
	/* Virtio-fs: MAP file to pagecache */
	FS_MAP = 6,
	/* Virtio-fs: UNMAP file to pagecache */
	FS_UNMAP = 7,
	/* Virtio-fs: file content sync */
	FS_SYNC = 8,
	/* Virtio-fs: perform a read/write from an fd to GPA directly */
	FS_IO = 9,

	BACKEND_REQ_MAX = 10,
};

/* vhost-user protocol feature bits */
enum PROTOCOL_FEATURE_BIT {
	/* Support multiple queues */
	PROTOCOL_MQ = 0,
	/* Support logging fd */
	PROTOCOL_LOG_FD = 1,
	/* Support RARP */
	PROTOCOL_RARP = 2,
	/* Support replying msg when set NEED_REPLY */
	PROTOCOL_REPLY_ACK = 3,
	/* Support setting MTU for virtio-net devices */
	PROTOCOL_MTU = 4,
	/* Support the daemon to send msg to acrn-dm */
	PROTOCOL_BACKEND_REQ = 5,
	/* Support setting daemon endian by SET_VRING_ENDIAN */
	PROTOCOL_CROSS_ENDIAN = 6,
	/* Support cropto session */
	PROTOCOL_CRYPTO_SESSION = 7,
	/* Deal pagefault, to support the POSTCOPY request*/
	PROTOCOL_PAGEFAULT = 8,
	/* Support virtio device config */
	PROTOCOL_CONFIG = 9,
	/* Support daemon to send fds to acrn-dm */
	PROTOCOL_BACKEND_SEND_FD = 10,
	/* Allow the daemon to register a host notifier */
	PROTOCOL_HOST_NOTIFIER = 11,
	/* Support inflight shmfd */
	PROTOCOL_INFLIGHT_SHMFD = 12,
	/* Support resetting the device */
	PROTOCOL_RESET_DEVICE = 13,
	/* Allow to send vring_kick/vring_call msg instead of event fd */
	PROTOCOL_INBAND_NOTIFICATIONS = 14,
	/* Support configuring memory slots */
	PROTOCOL_CONFIGURE_MEM_SLOTS = 15,
	/* Support get/set device status */
	PROTOCOL_STATUS = 16,
	/* Support Xen */
	PROTOCOL_XEN_MMAP = 17,
};

/* the socket msg flag bits */
enum VHOST_USER_MSG_FLAG {
	/* Lower 2 bits are the version (currently 0x01) */
	VHOST_USER_VERSION = 0x1,
	/* bit 2 marks the msg is a reply from the daemon */
	VHOST_USER_REPLY_ACK = (1 << 2),
	/* bit 3 is the need_reply flag */
	VHOST_USER_NEED_REPLY = (1 << 3),
	/*bits 4 and above are reserved */
	VHOST_USER_RESERVED_BITS = ~(0xf),
};

/** a mem region struct, which is similar the 'struct vhost_memory_region',
 *  except that the last member is the fd_offset
 */
struct vhost_user_mem_region {
	uint64_t gpa_start;
	uint64_t length;
	uint64_t hva_start;
	uint64_t fd_offset;
};

struct vhost_user_single_mem_region {
	uint64_t paddings;
	struct vhost_user_mem_region mem_region;
};

/* mem_table struct, contain multiple mem regions */
struct vhost_user_mem_table {
	uint32_t nr_regions;
	uint32_t paddings;
	struct vhost_user_mem_region mem_regions[MAX_VM_MEM_REGION];
};

/* virtio-fs: the msg initiated by daemon and sent to acrn-dm */
struct virtio_fs_slave_msg {
	/* offsets of the fd being mapped */
	uint64_t fd_offset[MAX_FS_SLAVE_ENTRIES];
	/* offsets of the shared mem cache */
	uint64_t cache_offset[MAX_FS_SLAVE_ENTRIES];
	/* lengths of the fd map */
	uint64_t len[MAX_FS_SLAVE_ENTRIES];
	/* flag, prot of fd map, read/write/exec etc*/
	uint64_t flags[MAX_FS_SLAVE_ENTRIES];
};

/* the socket msg struct */
struct vhost_user_socket_msg {
	/* request type, filled by enum VHOST_USER_REQUEST or enum SLAVE_REQUEST */
	uint32_t request;
	/* msg flag, filled by enum VHOST_USER_MSG_FLAG */
	uint32_t flag;
	/* payload data size */
	uint32_t size;
	/* payload data */
	union {
		uint64_t u64;
		struct vhost_vring_state vring_state;
		struct vhost_vring_addr vring_addr;
		struct vhost_user_mem_table mem_table;
		struct vhost_user_single_mem_region single_mem_region;
		struct virtio_fs_slave_msg fs_msg;
	} payload;
	/* number of fd passed in this socket msg */
	int fd_num;
	/* the fd array */
	int fds[MAX_VM_MEM_REGION];
} __packed;

#define VHOST_USER_HDR_SIZE offsetof(struct vhost_user_socket_msg, payload)

/* Supported the vhost-user protocol
 */
#define VHOST_USER_PROTOCOL_SOPPORTED                                                                                  \
	((1 << PROTOCOL_CONFIGURE_MEM_SLOTS) | (1 << PROTOCOL_MQ) | (1 << PROTOCOL_REPLY_ACK) |                        \
		(1 << PROTOCOL_BACKEND_REQ) | (1 << PROTOCOL_RESET_DEVICE) | (1 << PROTOCOL_STATUS))


/* vhost-user dev struct, similar to struct vhost
 */
struct vhost_user_dev {
	/**
	 * vhost dev pointer. since vhost-user is similar to
	 * vhost, based on struct vhost_dev to implement vhost-user
	 */
	struct vhost_dev *vhost_dev;
	/**
	 * vhost-user socket fd, this fd is connected to vhost-user daemon
	 */
	int slave_fd;
	/**
	 * vhost-user slave pid, this thread
	 is created to listen the slave_fd
	 */
	pthread_t slave_pid;
	/**
	 * vhost-user protocol features bits determines what kinds of
	 * socket msgs are allowed
	 */
	uint64_t protocol_features;
};

#ifdef VHOST_USER_DEBUG
static char *req_to_str[] = {
	"NULL",
	"get_feature",
	"set_feature",
	"set_owner",
	"reset_owner",
	"set_mem_table",
	"set_log_base",
	"set_log_fd",
	"set_vring_num",
	"set_vring_addr",
	"set_vring_base",
	"get_vring_base",
	"set_vring_kick",
	"set_vring_call",
	"set_vring_err",
	"get_protocol",
	"set_protocol",
	"get_que_num",
	"set_vring_enable",
	"send_rapp",
	"set_virtio_net_MTU",
	"set_slave_fd",
	"send_iotlb_msg",
	"set_vring_endian",
	"get_config",
	"set_config",
	"create_crypt_session",
	"close_crypt_session",
	"postcopy_advice",
	"postcopy_listen",
	"postcopy_end",
	"get_flight_fd",
	"set_flight_fd",
	"gpu_set_socket",
	"reset_device",
	"vring_kick",
	"get_max_mem_slots",
	"add_mem_region",
	"delete_mem_region",
	"set_device_status",
	"get_device_status",
	"NULL",
};

static void print_debug_vhost_user_msg(struct vhost_user_socket_msg *vu_msg)
{
	struct vhost_vring_state *vring_state;
	struct vhost_vring_addr *vring_addr;
	struct vhost_user_mem_region *region;

	if (vu_msg->request >= 0 && vu_msg->request < VHOST_USER_REQUEST_MAX)
		DPRINTF("vu_msg.request is %s\n", req_to_str[vu_msg->request]);
	DPRINTF("vu_msg.flag is %d, need_reply? %d, reply_ack? %d\n", vu_msg->flag,
		vu_msg->flag & VHOST_USER_NEED_REPLY, vu_msg->flag & VHOST_USER_REPLY_ACK);
	DPRINTF("vu_msg.fd_num is %d\n", vu_msg->fd_num);
	for (int i = 0; i < vu_msg->fd_num; i++) {
		DPRINTF("vu_msg.fd [%d] is %d\n", i, vu_msg->fds[0]);
	}
	DPRINTF("vu_msg payload size is %d\n", vu_msg->size);

	switch (vu_msg->request) {
	case GET_FEATURE_BITS:
	case SET_FEATURE_BITS:
	case SET_VIRTQ_KICKFD:
	case SET_VIRTQ_CALLFD:
	case SET_VIRTQ_ERRFD:
	case GET_PROTOCOL_FEATURE_BITS:
	case SET_PROTOCOL_FEATURE_BITS:
	case GET_QUEUE_NUM:
		DPRINTF("vu_msg payload u64 is %lld\n", vu_msg->payload.u64);
		break;
	case SET_MEM_TABLE:
		for (int i = 0; i < vu_msg->payload.mem_table.nr_regions; i++) {
			region = &vu_msg->payload.mem_table.mem_regions[i];

			DPRINTF("vu_msg payload memtable gpa_start is %lld\n", region->gpa_start);
			DPRINTF("vu_msg payload memtable length is %lld\n", region->length);
			DPRINTF("vu_msg payload memtable hva_base is %lld\n", region->hva_start);
			DPRINTF("vu_msg payload memtable fd_offset is %lld\n", region->fd_offset);
		}
		break;
	case SET_VIRTQ_NUM:
	case SET_VIRTQ_BASE:
	case GET_VIRTQ_BASE:
		vring_state = &vu_msg->payload.vring_state;

		DPRINTF("vu_msg payload vring_state->num is %lld\n", vring_state->num);
		DPRINTF("vu_msg payload vring_state->index is %lld\n", vring_state->index);
		break;
	case SET_VIRTQ_ADDR:
		vring_addr = &vu_msg->payload.vring_addr;

		DPRINTF("vu_msg payload vring_addr avail_user_addr is %lld\n", vring_addr->avail_user_addr);
		DPRINTF("vu_msg payload vring_addr desc_user_addr is %lld\n", vring_addr->desc_user_addr);
		DPRINTF("vu_msg payload vring_addr flags is %lld\n", vring_addr->flags);
		DPRINTF("vu_msg payload vring_addr index is %lld\n", vring_addr->index);
		DPRINTF("vu_msg payload vring_addr used_user_addr is %lld\n", vring_addr->used_user_addr);
		break;
	default:
		break;
	}
}
#endif

static int vhost_user_send_message(int socket_fd, struct vhost_user_socket_msg *vu_msg)
{
	char ancillary_fds[CMSG_SPACE(MAX_VM_MEM_REGION * sizeof(int))];
	struct iovec iov;
	struct msghdr msg;
	struct cmsghdr *ancillary_msg;
	void *ptr = (void *)vu_msg;
	int rc, fds_len;

	iov.iov_base = (char *)vu_msg;
	iov.iov_len = VHOST_USER_HDR_SIZE;

	memset(&msg, 0, sizeof(msg));
	memset(ancillary_fds, -1, sizeof(ancillary_fds));

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ancillary_fds;

	if (vu_msg->fd_num > MAX_VM_MEM_REGION) {
		WPRINTF("%s: too many fds, the fd_num is %d\n", __func__, vu_msg->fd_num);
		return -1;
	}

	if (vu_msg->fd_num > 0) {
		fds_len = vu_msg->fd_num * sizeof(int);
		msg.msg_controllen = CMSG_SPACE(fds_len);
		ancillary_msg = CMSG_FIRSTHDR(&msg);
		ancillary_msg->cmsg_len = CMSG_LEN(fds_len);
		ancillary_msg->cmsg_level = SOL_SOCKET;
		ancillary_msg->cmsg_type = SCM_RIGHTS;
		memcpy(CMSG_DATA(ancillary_msg), vu_msg->fds, fds_len);
	} else {
		msg.msg_controllen = 0;
	}

	do {
		rc = sendmsg(socket_fd, &msg, 0);
	} while (rc < 0 && (errno == EINTR || errno == EAGAIN));

	if (vu_msg->size) {
		do {
			rc = write(socket_fd, ptr + VHOST_USER_HDR_SIZE, vu_msg->size);
		} while (rc < 0 && (errno == EINTR || errno == EAGAIN));
	}

	if (rc <= 0) {
		WPRINTF("Error while writing: %s\n", strerror(errno));
		return -1;
	}

#ifdef VHOST_USER_DEBUG
	DPRINTF("===============================send a msg; fd is %d========================\n", socket_fd);
	print_debug_vhost_user_msg(vu_msg);
#endif

	return 0;
}

static int vhost_user_receive_message(int socket_fd, struct vhost_user_socket_msg *vu_msg)
{
	char ancillary_fds[CMSG_SPACE(MAX_VM_MEM_REGION * sizeof(int))];
	struct iovec iov;
	struct msghdr msg;
	struct cmsghdr *ancillary_msg;
	int rc;

	iov.iov_base = (char *)vu_msg;
	iov.iov_len = VHOST_USER_HDR_SIZE;

	memset(&msg, 0, sizeof(msg));
	memset(ancillary_fds, -1, sizeof(ancillary_fds));

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ancillary_fds;
	msg.msg_controllen = sizeof(ancillary_fds);

	do {
		rc = recvmsg(socket_fd, &msg, 0);
	} while (rc < 0 && (errno == EINTR || errno == EAGAIN));

	if (rc != VHOST_USER_HDR_SIZE) {
		WPRINTF("%s: receive a wrong msg hdr, received size is: %d\n", __func__, rc);
		return -1;
	}

	vu_msg->fd_num = 0;

	ancillary_msg = CMSG_FIRSTHDR(&msg);
	if (ancillary_msg && ancillary_msg->cmsg_level == SOL_SOCKET && ancillary_msg->cmsg_type == SCM_RIGHTS) {
		if (ancillary_msg->cmsg_len == CMSG_LEN(sizeof(int))) {
			vu_msg->fd_num = 1;
			vu_msg->fds[0] = *((int *)CMSG_DATA(ancillary_msg));
		} else {
			WPRINTF("%s: received too many fds, cannot support\n", __func__);
			return -1;
		}
	}

	if (vu_msg->size > sizeof(vu_msg->payload)) {
		WPRINTF("Error: received msg too big, request is %d, received size is: %d, max payload size is = %d\n",
			vu_msg->request, vu_msg->size, sizeof(vu_msg->payload));
		return -1;
	}

	if (vu_msg->size) {
		do {
			rc = read(socket_fd, &vu_msg->payload, vu_msg->size);
		} while (rc < 0 && (errno == EINTR || errno == EAGAIN));

		if (rc != vu_msg->size) {
			WPRINTF("Error: not receive the entire msg, received size is: %d, whole size is = %d\n", rc,
				vu_msg->size);
			return -1;
		}
	}

#ifdef VHOST_USER_DEBUG
	DPRINTF("=============================receive a msg; fd is %d=========================\n", socket_fd);
	print_debug_vhost_user_msg(vu_msg);
#endif

	return 0;
}

static int __vhost_u_set_mem_table(struct vhost_dev *vdev,
	struct vhost_user_mem_region *mem, int nr_regions, int *fds)
{
	struct vhost_user_socket_msg vu_msg;
	int i;

	memset(&vu_msg, 0, sizeof(vu_msg));
	vu_msg.request = SET_MEM_TABLE;
	vu_msg.flag = VHOST_USER_VERSION;
	vu_msg.size = sizeof(struct vhost_memory) + sizeof(*mem) * nr_regions;

	vu_msg.payload.mem_table.nr_regions = nr_regions;
	memcpy(vu_msg.payload.mem_table.mem_regions, mem, sizeof(*mem) * nr_regions);

	vu_msg.fd_num = nr_regions;
	for (i = 0; i < nr_regions; i++)
		vu_msg.fds[i] = fds[i];

	return vhost_user_send_message(vdev->fd, &vu_msg);
}

static int vhost_u_set_vring_addr(struct vhost_dev *vdev, struct vhost_vring_addr *addr)
{
	struct vhost_user_socket_msg vu_msg;

	memset(&vu_msg, 0, sizeof(vu_msg));
	vu_msg.request = SET_VIRTQ_ADDR;
	vu_msg.flag = VHOST_USER_VERSION;
	vu_msg.payload.vring_addr = *addr;
	vu_msg.size = sizeof(vu_msg.payload.vring_addr);

	return vhost_user_send_message(vdev->fd, &vu_msg);
}

static int vhost_u_set_vring_num(struct vhost_dev *vdev, struct vhost_vring_state *ring)
{
	struct vhost_user_socket_msg vu_msg;

	memset(&vu_msg, 0, sizeof(vu_msg));
	vu_msg.request = SET_VIRTQ_NUM;
	vu_msg.flag = VHOST_USER_VERSION;
	vu_msg.payload.vring_state = *ring;
	vu_msg.size = sizeof(vu_msg.payload.vring_state);

	return vhost_user_send_message(vdev->fd, &vu_msg);
}

static int vhost_u_set_vring_base(struct vhost_dev *vdev, struct vhost_vring_state *ring)
{
	struct vhost_user_socket_msg vu_msg;

	memset(&vu_msg, 0, sizeof(vu_msg));
	vu_msg.request = SET_VIRTQ_BASE;
	vu_msg.flag = VHOST_USER_VERSION;
	vu_msg.payload.vring_state = *ring;
	vu_msg.size = sizeof(vu_msg.payload.vring_state);

	return vhost_user_send_message(vdev->fd, &vu_msg);
}

static int vhost_u_get_vring_base(struct vhost_dev *vdev, struct vhost_vring_state *ring)
{
	struct vhost_user_socket_msg vu_msg;
	int rc;

	memset(&vu_msg, 0, sizeof(vu_msg));
	vu_msg.request = GET_VIRTQ_BASE;
	vu_msg.flag = VHOST_USER_VERSION | VHOST_USER_NEED_REPLY;
	vu_msg.payload.vring_state = *ring;
	vu_msg.size = sizeof(vu_msg.payload.vring_state);

	rc = vhost_user_send_message(vdev->fd, &vu_msg);
	if (rc) {
		WPRINTF(" get vring base error, send msg error\n");
		return rc;
	}

	rc = vhost_user_receive_message(vdev->fd, &vu_msg);
	if (rc) {
		WPRINTF(" get vring base error, receive msg error\n");
		return rc;
	}

	*ring = vu_msg.payload.vring_state;
	return rc;
}

#define INVAILD_EVENTFD (1 << 8)

static int vhost_u_set_vring_kick(struct vhost_dev *vdev, struct vhost_vring_file *file)
{
	struct vhost_user_socket_msg vu_msg;

	memset(&vu_msg, 0, sizeof(vu_msg));
	vu_msg.request = SET_VIRTQ_KICKFD;
	vu_msg.flag = VHOST_USER_VERSION;
	vu_msg.payload.u64 = file->index;
	vu_msg.size = sizeof(vu_msg.payload.u64);

	if (file->fd < 0) {
		vu_msg.payload.u64 |= INVAILD_EVENTFD;

	} else {
		vu_msg.fd_num = 1;
		vu_msg.fds[0] = file->fd;
	}
	return vhost_user_send_message(vdev->fd, &vu_msg);
}

static int vhost_u_set_vring_call(struct vhost_dev *vdev, struct vhost_vring_file *file)
{
	struct vhost_user_socket_msg vu_msg;

	memset(&vu_msg, 0, sizeof(vu_msg));
	vu_msg.request = SET_VIRTQ_CALLFD;
	vu_msg.flag = VHOST_USER_VERSION;
	vu_msg.payload.u64 = file->index;
	vu_msg.size = sizeof(vu_msg.payload.u64);

	if (file->fd < 0) {
		vu_msg.payload.u64 |= INVAILD_EVENTFD;

	} else {
		vu_msg.fd_num = 1;
		vu_msg.fds[0] = file->fd;
	}

	return vhost_user_send_message(vdev->fd, &vu_msg);
}

static int vhost_u_set_features(struct vhost_dev *vdev, uint64_t features)
{
	struct vhost_user_socket_msg vu_msg;

	memset(&vu_msg, 0, sizeof(vu_msg));
	vu_msg.request = SET_FEATURE_BITS;
	vu_msg.flag = VHOST_USER_VERSION;
	vu_msg.payload.u64 = features;
	vu_msg.size = sizeof(vu_msg.payload.u64);

	return vhost_user_send_message(vdev->fd, &vu_msg);
}

static int vhost_u_get_features(struct vhost_dev *vdev, uint64_t *features)
{
	int rc;
	struct vhost_user_socket_msg vu_msg;

	memset(&vu_msg, 0, sizeof(vu_msg));
	vu_msg.request = GET_FEATURE_BITS;
	vu_msg.flag = VHOST_USER_VERSION | VHOST_USER_NEED_REPLY;

	rc = vhost_user_send_message(vdev->fd, &vu_msg);
	if (rc) {
		WPRINTF("%s error: send msg err\n", __func__);
		return rc;
	}

	rc = vhost_user_receive_message(vdev->fd, &vu_msg);
	if (rc) {
		WPRINTF("%s error: receive msg err\n", __func__);
		return rc;
	}

	*features = vu_msg.payload.u64;
	return rc;
}

static int vhost_user_set_protocol_features(struct vhost_dev *vdev, uint64_t features)
{
	struct vhost_user_socket_msg vu_msg;

	memset(&vu_msg, 0, sizeof(vu_msg));
	vu_msg.request = SET_PROTOCOL_FEATURE_BITS;
	vu_msg.flag = VHOST_USER_VERSION;
	vu_msg.payload.u64 = features;
	vu_msg.size = sizeof(vu_msg.payload.u64);

	return vhost_user_send_message(vdev->fd, &vu_msg);
}

static int vhost_user_get_protocol_features(struct vhost_dev *vdev, uint64_t *features)
{
	int rc;
	struct vhost_user_socket_msg vu_msg;

	memset(&vu_msg, 0, sizeof(vu_msg));
	vu_msg.request = GET_PROTOCOL_FEATURE_BITS;
	vu_msg.flag = VHOST_USER_VERSION | VHOST_USER_NEED_REPLY;

	rc = vhost_user_send_message(vdev->fd, &vu_msg);
	if (rc) {
		WPRINTF("%s error: send msg err\n", __func__);
		return rc;
	}

	rc = vhost_user_receive_message(vdev->fd, &vu_msg);
	if (rc) {
		WPRINTF("%s error: receive msg err\n", __func__);
		return rc;
	}

	*features = vu_msg.payload.u64;
	return rc;
}

static int vhost_u_set_owner(struct vhost_dev *vdev)
{
	struct vhost_user_socket_msg vu_msg;

	memset(&vu_msg, 0, sizeof(vu_msg));
	vu_msg.request = SET_OWNER;
	vu_msg.flag = VHOST_USER_VERSION;

	return vhost_user_send_message(vdev->fd, &vu_msg);
}

static int vhost_u_reset_device(struct vhost_dev *vdev)
{
	struct vhost_user_socket_msg vu_msg;
	struct vhost_user_dev *vhost_user;

	memset(&vu_msg, 0, sizeof(vu_msg));
	vhost_user = vdev->priv;
	if (vhost_user->protocol_features & (1 << PROTOCOL_RESET_DEVICE)) {
		vu_msg.request = RESET_DEVICE;
	} else {
		DPRINTF("The vhost-user RESET_OWNER may outdate\n");
		vu_msg.request = RESET_OWNER;
	}

	vu_msg.flag = VHOST_USER_VERSION;

	return vhost_user_send_message(vdev->fd, &vu_msg);
}

static int vhost_u_set_mem_table(struct vhost_dev *vdev)
{
	struct vmctx *ctx;
	struct vhost_user_mem_region *vu_mem_regs;
	struct vm_mmap_mem_region *vm_mem_reg;
	uint64_t next_gpa = 0;
	int *fds, rc, nregions = 0;

	vu_mem_regs = calloc(1, sizeof(*vu_mem_regs) * MAX_VM_MEM_REGION);
	fds = calloc(1, sizeof(*fds) * MAX_VM_MEM_REGION);
	vm_mem_reg = calloc(1, sizeof(*vm_mem_reg));

	if (!vu_mem_regs || !fds || !vm_mem_reg) {
		WPRINTF("vhost-user: out of memory\n");
		return -1;
	}

	ctx = vdev->base->dev->vmctx;

	if (ctx->lowmem > 0) {
		next_gpa = (uintptr_t)0;
		while (nregions < MAX_VM_MEM_REGION &&
			vm_get_mem_region(ctx, next_gpa, vm_mem_reg)) {
			vu_mem_regs[nregions].gpa_start = vm_mem_reg->gpa_start;
			vu_mem_regs[nregions].length = vm_mem_reg->gpa_end - vm_mem_reg->gpa_start;
			vu_mem_regs[nregions].hva_start = (uint64_t)vm_mem_reg->hva_base;
			vu_mem_regs[nregions].fd_offset = vm_mem_reg->fd_offset;
			fds[nregions] = vm_mem_reg->fd;

			next_gpa += vu_mem_regs[nregions++].length;
		}
	}

	if (ctx->highmem > 0) {
		next_gpa = ctx->highmem_gpa_base;
		while (nregions < MAX_VM_MEM_REGION &&
			vm_get_mem_region(ctx, next_gpa, vm_mem_reg)) {
			vu_mem_regs[nregions].gpa_start = vm_mem_reg->gpa_start;
			vu_mem_regs[nregions].length = vm_mem_reg->gpa_end - vm_mem_reg->gpa_start;
			vu_mem_regs[nregions].hva_start = (uint64_t)vm_mem_reg->hva_base;
			vu_mem_regs[nregions].fd_offset = vm_mem_reg->fd_offset;
			fds[nregions] = vm_mem_reg->fd;

			next_gpa += vu_mem_regs[nregions++].length;
		}
	}

	rc = __vhost_u_set_mem_table(vdev, vu_mem_regs, nregions, fds);

	free(vu_mem_regs);
	free(fds);
	free(vm_mem_reg);
	if (rc < 0) {
		WPRINTF("set_mem_table failed, errno is (%s)\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int vhost_u_init(struct vhost_dev *vdev, struct virtio_base *base,
		  int fd, int vq_idx, uint32_t busyloop_timeout)
{
	struct vhost_user_dev *vhost_user;
	uint64_t protocol_features;
	int rc;

	vhost_user = calloc(1, sizeof(*vhost_user));
	vdev->priv = vhost_user;
	vdev->base = base;
	vdev->fd = fd;
	vdev->vq_idx = vq_idx;
	vhost_user->slave_fd = -1;

	rc = vhost_user_get_protocol_features(vdev, &protocol_features);
	if (rc < 0) {
		WPRINTF("vhost_user_get_protocol_features failed\n");
		return -1;
	}

	vhost_user->protocol_features = protocol_features & VHOST_USER_PROTOCOL_SOPPORTED;

	rc = vhost_user_set_protocol_features(vdev, vhost_user->protocol_features);
	if (rc < 0) {
		WPRINTF("vhost_user_set_protocol_features failed\n");
		return -1;
	}

	return 0;
}

static int vhost_u_deinit(struct vhost_dev *vdev)
{
	struct vhost_user_dev *vhost_user;

	vhost_user = vdev->priv;
	vdev->base = NULL;
	vdev->vq_idx = 0;

	if (vhost_user->slave_fd > 0) {
		close(vhost_user->slave_fd);
		vhost_user->slave_fd = -1;
	}

	free(vhost_user);
	vdev->priv = NULL;

	return 0;
}

const struct vhost_dev_ops vhost_user_ops = {
	.vhost_init = vhost_u_init,
	.vhost_deinit = vhost_u_deinit,
	.vhost_set_mem_table = vhost_u_set_mem_table,
	.vhost_set_vring_addr = vhost_u_set_vring_addr,
	.vhost_set_vring_num = vhost_u_set_vring_num,
	.vhost_set_vring_base = vhost_u_set_vring_base,
	.vhost_get_vring_base = vhost_u_get_vring_base,
	.vhost_set_vring_kick = vhost_u_set_vring_kick,
	.vhost_set_vring_call = vhost_u_set_vring_call,
	.vhost_set_features = vhost_u_set_features,
	.vhost_get_features = vhost_u_get_features,
	.vhost_set_owner = vhost_u_set_owner,
	.vhost_reset_device = vhost_u_reset_device
};
