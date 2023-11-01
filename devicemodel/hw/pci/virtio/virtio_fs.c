#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>

#include "atomic.h"
#include "dm.h"
#include "pci_core.h"
#include "virtio.h"
#include "dm_string.h"
#include "vhost.h"

static int virtio_fs_debug;

#define DPRINTF(params) do { if (virtio_fs_debug) pr_err params; } while (0)
#define WPRINTF(params) (pr_err params)

#define VIRTIO_FS_S_VHOSTCAPS (1UL << VIRTIO_F_VERSION_1)
/* Queue definitions.
 */
#define VIRTIO_FS_HIPRIQ       0
#define VIRTIO_FS_REQUESTQ     1
#define VIRTIO_FS_QNUM         2

/*
 * Virtqueue size.
 */
#define VIRTIO_FS_RINGSZ       256
#define VIRTIO_FS_MAXSEGS      256

struct virtio_fs_config {
	/* Filesystem name (UTF-8, not NUL-terminated, padded with NULs) */
	__u8 tag[36];

	/* Number of request queues */
	__le32 num_request_queues;
} __packed;

/*
 * vhost device struct
 */
struct vhost_fs {
	struct vhost_dev vhost_dev;
	struct vhost_vq vqs[VIRTIO_FS_QNUM];
	int socket_fd;
	bool vhost_started;
};

/*
 * Per-device struct
 */
struct virtio_fs {
	struct virtio_base base;
	struct virtio_vq_info queues[VIRTIO_FS_QNUM];
	pthread_mutex_t mtx;

	struct virtio_fs_config config;
	struct vhost_fs *vhost_fs;
	uint64_t features; /* negotiated features */
};

static int g_socket_fd = -1;

static void virtio_fs_reset(void *vdev);
static int virtio_fs_cfgread(void *vdev, int offset, int size, uint32_t *retval);
static int virtio_fs_cfgwrite(void *vdev, int offset, int size, uint32_t value);
static void virtio_fs_neg_features(void *vdev, uint64_t negotiated_features);
static void virtio_fs_set_status(void *vdev, uint64_t status);
static void virtio_fs_teardown(void *param);
static struct vhost_fs *vhost_fs_init(struct virtio_base *base, int vq_idx, int socket_fd);
static int vhost_fs_deinit(struct vhost_fs *vhost_fs);
static int vhost_fs_start(struct vhost_fs *vhost_fs);
static int vhost_fs_stop(struct vhost_fs *vhost_fs);

static struct virtio_ops virtio_fs_ops = {
	"virtio_fs", /* our name */
	VIRTIO_FS_QNUM, /* supported virtqueue number */
	sizeof(struct virtio_fs_config), /* config reg size */
	virtio_fs_reset, /* reset */
	NULL, /* device-wide qnotify */
	virtio_fs_cfgread, /* read virtio config */
	virtio_fs_cfgwrite, /* write virtio config */
	virtio_fs_neg_features, /* apply negotiated features */
	virtio_fs_set_status, /* called on guest set status */
};

static int vhost_user_socket_connect(char *socket_path)
{
	int socket_fd, rc;
	struct sockaddr_un s_un;

	socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (socket_fd < 0) {
		WPRINTF(("Create socket fd failed\n"));
		return -1;
	}

	memset(&s_un, 0, sizeof(s_un));
	s_un.sun_family = AF_UNIX;

	/*s_un.sun_path is a 108 char array*/
	rc = snprintf(s_un.sun_path, sizeof(s_un.sun_path), "%s", socket_path);
	if (rc < 0 || rc >= sizeof(s_un.sun_path))
		return -1;

	if (connect(socket_fd, (struct sockaddr *)&s_un, sizeof(s_un)) < 0) {
		WPRINTF(("Socket connect failed errno (%s), socket path is  %s\n", strerror(errno), socket_path));
		return -1;
	}

	DPRINTF(("socket fd is  %d\n", socket_fd));
	return socket_fd;
}

static void vhost_fs_handle_output(void *vdev, struct virtio_vq_info *vq)
{
	WPRINTF(("virtio_fs: get the virtqueue notify in acrn-dm, should not happen\n"));
	/* do nothing */
}

static bool virtio_fs_parse_opts(struct virtio_fs *fs, char *opts)
{
	char *devopts = NULL;
	char *vtopts = NULL;
	char *opt = NULL;
	bool filled_tag = false;
	bool filled_socket = false;
	int socket_fd;

	if (opts == NULL) {
		WPRINTF(("virtio_fs: the launch opts is NULL\n"));
		return -1;
	}

	devopts = vtopts = strdup(opts);
	if (!devopts) {
		WPRINTF(("virtio_fs: strdup returns NULL\n"));
		return -1;
	}

	while ((opt = strsep(&vtopts, ",")) != NULL) {
		if (!strncmp(opt, "tag", 3)) {
			(void)strsep(&opt, "=");
			if (!opt)
				goto opts_err;
			if (strlen(opt) > 36) {
				WPRINTF(("virtio-fs: tag string is too long, pls less than 36\n"));
				goto opts_err;
			}
			memcpy(fs->config.tag, opt, strlen(opt));
			filled_tag = true;
		} else if (!strncmp(opt, "socket", 6)) {
			(void)strsep(&opt, "=");
			if (!opt)
				goto opts_err;


			if (g_socket_fd > 0) {
				socket_fd = g_socket_fd;
			} else {
				//@todo: this fd should be created in other function
				socket_fd = vhost_user_socket_connect(opt);
				g_socket_fd = socket_fd;
			}
			DPRINTF(("virtio_fs: socket fd is %d\n", socket_fd));
			if (socket_fd < 0) {
				WPRINTF(("virtio_fs: socket connection failed\n"));
				goto opts_err;
			}
			filled_socket = true;
		} else {
			WPRINTF(("virtio_fs: unknown args %s\n", opt));
		}
	};

	if (filled_socket && filled_tag) {
		free(devopts);
		return 0;
	}

opts_err:
	WPRINTF(("virtio_fs usage: socket=socket_path,tag=xxx\n"));
	free(devopts);
	return -1;
}

static int virtio_fs_init(struct vmctx *ctx, struct pci_vdev *dev, char *opts)
{
	struct virtio_fs *fs = NULL;
	pthread_mutexattr_t attr;
	int rc;

	fs = calloc(1, sizeof(struct virtio_fs));
	if (!fs) {
		WPRINTF(("virtio_fs: calloc returns NULL\n"));
		return -1;
	}

	rc = virtio_fs_parse_opts(fs, opts);
	if (rc) {
		return -1;
	}
	/* init mutex attribute properly to avoid deadlock */
	rc = pthread_mutexattr_init(&attr);
	if (rc)
		WPRINTF(("mutexattr init failed with erro %d!\n", rc));
	rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	if (rc)
		WPRINTF(("virtio_fs: mutexattr_settype failed with error %d!\n", rc));

	rc = pthread_mutex_init(&fs->mtx, &attr);
	if (rc)
		WPRINTF(("virtio_fs: pthread_mutex_init failed with error %d!\n", rc));

	virtio_linkup(&fs->base, &virtio_fs_ops, fs, dev, fs->queues, BACKEND_VHOST_USER);
	fs->base.mtx = &fs->mtx;

	fs->base.device_caps = VIRTIO_FS_S_VHOSTCAPS;

	fs->queues[VIRTIO_FS_HIPRIQ].qsize = VIRTIO_FS_RINGSZ;
	fs->queues[VIRTIO_FS_HIPRIQ].notify = vhost_fs_handle_output;
	fs->queues[VIRTIO_FS_REQUESTQ].qsize = VIRTIO_FS_RINGSZ;
	fs->queues[VIRTIO_FS_REQUESTQ].notify = vhost_fs_handle_output;

	fs->config.num_request_queues = 1;

	/* initialize config space */
	pci_set_cfgdata16(dev, PCIR_DEVICE, VIRTIO_TYPE_FS + 0x1040);
	pci_set_cfgdata16(dev, PCIR_VENDOR, VIRTIO_VENDOR);
	pci_set_cfgdata8(dev, PCIR_CLASS, PCIC_STORAGE);
	pci_set_cfgdata8(dev, PCIR_SUBCLASS, PCIS_STORAGE_OTHER);
	pci_set_cfgdata16(dev, PCIR_SUBDEV_0, VIRTIO_TYPE_FS);
	if (is_winvm == true)
		pci_set_cfgdata16(dev, PCIR_SUBVEND_0, ORACLE_VENDOR_ID);
	else
		pci_set_cfgdata16(dev, PCIR_SUBVEND_0, VIRTIO_VENDOR);
	pci_set_cfgdata16(dev, PCIR_REVID, 1);

	if (virtio_set_modern_bar(&fs->base, false)) {
		WPRINTF(("vtfs: set modern bar error\n"));
		return -1;
	}

	fs->vhost_fs = vhost_fs_init(&fs->base, 0, g_socket_fd);
	if (!fs->vhost_fs) {
		WPRINTF(("vhost user fs init failed."));
		free(fs);
		return -1;
	}

	/* use BAR 1 to map MSI-X table and PBA, if we're using MSI-X */
	if (virtio_interrupt_init(&fs->base, virtio_uses_msix())) {
		WPRINTF(("vtfs interrupt init failed.\n"));
		vhost_fs_deinit(fs->vhost_fs);
		free(fs);
		return -1;
	}

	return 0;
}

static int virtio_fs_cfgwrite(void *vdev, int offset, int size, uint32_t value)
{
	/* silently ignore other writes */
	WPRINTF(("vtfs: write to readonly reg %d\n\r", offset));
	return 0;
}

static int virtio_fs_cfgread(void *vdev, int offset, int size, uint32_t *retval)
{
	struct virtio_fs *fs = vdev;
	void *ptr;

	ptr = (uint8_t *)&fs->config + offset;
	memcpy(retval, ptr, size);
	return 0;
}

static void virtio_fs_reset(void *vdev)
{
	struct virtio_fs *fs = vdev;

	DPRINTF(("vtfs: device reset requested !\n"));

	/* now reset rings, MSI-X vectors, and negotiated capabilities */
	virtio_reset_dev(&fs->base);
}

static void virtio_fs_neg_features(void *vdev, uint64_t negotiated_features)
{
	struct virtio_fs *fs = vdev;

	fs->features = negotiated_features;
}

static void virtio_fs_set_status(void *vdev, uint64_t status)
{
	struct virtio_fs *fs = vdev;
	int rc;

	if (!fs->vhost_fs) {
		WPRINTF(("virtio_fs_set_status vhost_fs is NULL.\n"));
		return;
	}

	if (!fs->vhost_fs->vhost_started && (status & VIRTIO_CONFIG_S_DRIVER_OK)) {
		rc = vhost_fs_start(fs->vhost_fs);
		if (rc < 0) {
			WPRINTF(("vhost_fs_start failed\n"));
			return;
		}
	} else if (fs->vhost_fs->vhost_started && ((status & VIRTIO_CONFIG_S_DRIVER_OK) == 0)) {
		rc = vhost_fs_stop(fs->vhost_fs);
		if (rc < 0)
			WPRINTF(("vhost_fs_stop failed\n"));
	}
}

static void virtio_fs_teardown(void *param)
{
	struct virtio_fs *fs;

	fs = (struct virtio_fs *)param;
	if (!fs)
		return;

	free(fs);
}

static struct vhost_fs *vhost_fs_init(struct virtio_base *base, int vq_idx, int socket_fd)
{
	struct vhost_fs *vhost_fs = NULL;
	uint64_t dev_features = VIRTIO_FS_S_VHOSTCAPS;
	int rc;

	vhost_fs = calloc(1, sizeof(struct vhost_fs));
	if (!vhost_fs) {
		WPRINTF(("vhost init out of memory\n"));
		goto fail;
	}

	vhost_fs->socket_fd = socket_fd;

	/* pre-init before calling vhost_dev_init */
	vhost_fs->vhost_dev.nvqs = ARRAY_SIZE(vhost_fs->vqs);
	vhost_fs->vhost_dev.vqs = vhost_fs->vqs;

	rc = vhost_dev_init(&vhost_fs->vhost_dev, base, socket_fd, vq_idx, dev_features, 0, 0);
	if (rc < 0) {
		WPRINTF(("vhost_dev_init failed\n"));
		goto fail;
	}

	return vhost_fs;
fail:
	if (vhost_fs)
		free(vhost_fs);
	return NULL;
}

static int vhost_fs_deinit(struct vhost_fs *vhost_fs)
{
	return vhost_dev_deinit(&vhost_fs->vhost_dev);
}

static int vhost_fs_start(struct vhost_fs *vhost_fs)
{
	int rc;

	if (vhost_fs->vhost_started) {
		WPRINTF(("vhost_user fs already started\n"));
		return 0;
	}

	DPRINTF(("vhost-user fs start now\n"));
	rc = vhost_dev_start(&vhost_fs->vhost_dev);
	if (rc < 0) {
		WPRINTF(("vhost_dev_start failed\n"));
		goto fail;
	}

	vhost_fs->vhost_started = true;
	return 0;

fail:
	return -1;
}

static int vhost_fs_stop(struct vhost_fs *vhost_fs)
{
	int rc;

	if (!vhost_fs->vhost_started) {
		WPRINTF(("vhost fs already stopped\n"));
		return 0;
	}

	rc = vhost_dev_stop(&vhost_fs->vhost_dev);
	if (rc < 0)
		WPRINTF(("vhost_dev_stop failed\n"));

	vhost_fs->vhost_started = false;
	return rc;
}

static void virtio_fs_deinit(struct vmctx *ctx, struct pci_vdev *dev, char *opts)
{
	struct virtio_fs *fs;

	if (dev->arg) {
		fs = (struct virtio_fs *)dev->arg;

		if (fs->vhost_fs) {
			vhost_fs_stop(fs->vhost_fs);
			vhost_fs_deinit(fs->vhost_fs);
			/* when closing socket_fd, the daemon will exit automatically
			 * it is not what we want when just rebooting VM
			 * let the acrn-dm releae the socket_fd when it exits
			 */
			free(fs->vhost_fs);
			fs->vhost_fs = NULL;
		}

		virtio_fs_teardown(fs);

		DPRINTF(("%s: done\n", __func__));
	} else
		WPRINTF(("%s: NULL!\n", __func__));
}

struct pci_vdev_ops pci_ops_virtio_fs = {
	.class_name = "virtio-fs",
	.vdev_init = virtio_fs_init,
	.vdev_deinit = virtio_fs_deinit,
	.vdev_barwrite = virtio_pci_write,
	.vdev_barread = virtio_pci_read
};
DEFINE_PCI_DEVTYPE(pci_ops_virtio_fs);
