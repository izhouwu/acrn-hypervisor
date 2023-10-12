/*
 * Copyright (C) 2019-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <arpa/inet.h>
#include <sys/param.h>
#include <sys/uio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>

#include "dm.h"
#include "pci_core.h"
#include "virtio.h"
#include "acpi.h"

/* SPI controller virtualization architecture
 *
 *                        +-----------------------------+
 *                        | ACRN DM                     |
 *  +----------------+    |  +----------------------+   |  virtqueue
 *  |  spi device    |    |  |                      |<--+---+
 *  |    emulator    |    |  | virtio spi controler |   |   |
 *  |    @port9000   |    |  |                      |   |   |
 *  +---------+------+    |  +-+------+-----+-------+   |   |
 *            |           +----+------+-----+-----------+   |
 * User space | +--------------+   +--+     +--+            |
 *            v v                  v           v            |
 *    +-------+-+----+  +--------------+  +----+---------+  |  +--------------+  +--------------+  +--------------+
 * ---+ tcp@port9000 +--+/dev/spidevC.D+--+/dev/spidevX.Y+--+--+  User VM:    |--+  User VM:    |--+  User VM:    |
 *    |              |  |              |  |              |  |  |/dev/spidev0.0|  |/dev/spidev0.1|  |/dev/spidev0.2|
 *    +--------------+  +--------------+  +----+---------+  |  +----------+---+  +-----+--------+  ++-------------+
 * Kernel space                    +           v            |             v            v            v
 *                         +-------+-----+ +---+---------+  |	       +--+------------+------------++
 *                         |spi device 1 | |spi device n |  +--------->|          User VM:           |
 *                         |             |               |	       |    virtio spi controller    |
 *                         +-------+-----+ +---+---------+	       +-----------------------------+
 * --------------------------------+-----------+----------
 * Hardware                        +           +
 *                                 |           |
 *                                 v           v
 *                          +------+---+  +----+-----+
 *                          |spi device|  |spi device|
 *                          +----------+  +----------+
 */

/*
 * Cmdline to add a Virtio SPI controller and attached SPI devices:
 *
 * virtio-spi,<type>:<type specific>,[<type>:<type specific>]
 * type and specific configs:
 * "physical": <bus>:<chipselect>
 *   a pair of bus and chipselect of a physical spi device which will
 *   be connected to the virtual spi controller
 *     e.g. 1:0 for /dev/spidev1.0
 * "tcp": <port>
 *   Create a SPI device based on TCP socket
 * "loopback": <none>
 *   Create a loopback device for test.
 *
 * Note: virtual chipselects are determined according to the argument
 * index.
 */

static int virtio_spi_debug=0;
#define VIRTIO_SPI_PREF "virtio_spi: "
#define DPRINTF(fmt, args...) \
       do { if (virtio_spi_debug) pr_info(VIRTIO_SPI_PREF fmt, ##args); } while (0)
#define WPRINTF(fmt, args...) pr_err(VIRTIO_SPI_PREF fmt, ##args)

#define MAX_SPIDEVS		16
#define MAX_NODE_NAME_LEN	40

#define VIRTIO_SPI_HOSTCAPS   (1UL << VIRTIO_F_VERSION_1)

/* Same encoding with linux/spi/spi.h */
#define MODE_CPHA		BIT(0)
#define MODE_CPOL		BIT(1)
#define MODE_CS_HIGH		BIT(2)
#define MODE_LSB_FIRST		BIT(3)
#define MODE_LOOP		BIT(4)
// more can be added if required in the future.

struct virtio_spi_out_hdr {
	uint8_t slave_id;
	uint8_t bits_per_word;
	uint8_t cs_change;	/* deassert cs before next transfer ? */
	uint8_t tx_nbits;	/* single, dual, quad, octal */
	uint8_t rx_nbits;
	uint8_t paddings[3];
	uint32_t mode;
	uint32_t freq;
	uint32_t word_delay_ns;	/* delay between words of a transfer */
	uint32_t cs_setup_ns;	/* delay between cs assert and data start */
	uint32_t cs_delay_hold_ns;	/* delay between data end and cs deassert */
	uint32_t cs_change_delay_inactive_ns;	/* delay between cs deassert and next assert */
};

/* result code definitions */
#define VIRTIO_SPI_TRANS_OK	0
#define VIRTIO_SPI_TRANS_ERR	1

struct virtio_spi_in_hdr {
	uint8_t result;
};

struct virtio_spi_transfer_req {
	struct virtio_spi_out_hdr *head;
	uint8_t *tx_buf;
	uint32_t tx_buf_size;
	uint8_t *rx_buf;
	uint32_t rx_buf_size;
};

enum vspidev_type {
	VSPIDEV_TYPE_NULL = 0,
	VSPIDEV_TYPE_LOOPBACK,
	VSPIDEV_TYPE_PHYSICAL,
	VSPIDEV_TYPE_TCP,
};

struct vspidev;

struct vspidev_be {
	enum vspidev_type type;
	char *name;
	int (*init)(struct vspidev *vspidev, char *opts);
	void (*deinit)(struct vspidev *vspidev);
	uint8_t (*transfer)(struct vspidev *vspidev, struct virtio_spi_transfer_req *req);
};

/*
 * SPI device attached a Virtio SPI controller
 */
struct vspidev {
	struct virtio_spi *vspi;
	int cs;			/* virtual cs */
	int type;
	struct vspidev_be *be;
	void *priv;
};

static int acpi_spi_controller_num;

/* Virtual SPI device be drivers */

static int spidev_init_noop(struct vspidev *vspidev, char *opts)
{
	vspidev->priv = NULL;
	return 0;
}

static uint8_t spidev_transfer_noop(struct vspidev *vspidev, struct virtio_spi_transfer_req *req)
{
	return VIRTIO_SPI_TRANS_OK;
}

static struct vspidev_be vspidev_null = {
	.type = VSPIDEV_TYPE_LOOPBACK,
	.name = "null",
	.init = spidev_init_noop,
	.transfer = spidev_transfer_noop,
};

static uint8_t spidev_transfer_loopback(struct vspidev *vspidev, struct virtio_spi_transfer_req *req)
{
	uint32_t len = req->tx_buf_size < req->rx_buf_size ? req->tx_buf_size : req->rx_buf_size;
	memcpy(req->rx_buf, req->tx_buf, len);
	return 0;
}

static struct vspidev_be vspidev_loopback = {
	.type = VSPIDEV_TYPE_LOOPBACK,
	.name = "loopback",
	.init = spidev_init_noop,
	.transfer = spidev_transfer_loopback,
};

struct vspidev_physical_data {
	struct vspidev *vspidev;
	int 		bus;	/* physical bus */
	int		cs;	/* physical chipselect */
	int 		fd;
};

static int spidev_init_physical(struct vspidev *vspidev, char *opts)
{
	struct vspidev_physical_data *data = calloc(1, sizeof(*data));
	char *cp, *t;
	char devname[MAX_NODE_NAME_LEN];
	int ret = 0;

	if (data == NULL) {
		WPRINTF("memory allocation failed\n");
		return -1;
	}

	data->vspidev = vspidev;
	cp = strsep(&opts, ":");
	if (cp == NULL || opts == NULL) {
		WPRINTF("%s@%d: Bad options\n", vspidev->be->name, vspidev->cs);
		ret = -1;
		goto err;
	}

	if (dm_strtoi(cp, &t, 10, &data->bus) || data->bus < 0) {
		ret = -1;
		goto err;
	}
	if (dm_strtoi(opts, &t, 10, &data->cs) || data->cs < 0) {
		ret = -1;
		goto err;
	}

	snprintf(devname, MAX_NODE_NAME_LEN, "/dev/spidev%d.%d", data->bus, data->cs);
	data->fd = open(devname, O_RDWR);
	if (data->fd < 0) {
		WPRINTF("fail to open physical %s\n", devname);
		ret = -1;
		goto err;
	}
	vspidev->priv = data;

	return 0;
err:
	free(data);
	return ret;
}

static void spidev_deinit_physical(struct vspidev *vspidev)
{
	struct vspidev_physical_data *data = vspidev->priv;

	if (data && data->fd > 0) {
		close(data->fd);
	}
	free(data);
	vspidev->priv = NULL;
}

static uint8_t spidev_transfer_physical(struct vspidev *vspidev, struct virtio_spi_transfer_req *req)
{
	int ret = 0;
	struct vspidev_physical_data *data = vspidev->priv;
	int fd = data->fd;

	if (fd <= 0) {
		WPRINTF("Not a valid fd to access spidev%d.%d",
				data->bus, data->cs);
		ret = -1;
		goto err;
	}
	DPRINTF("%s: fd %d\n", __func__, fd);

	ret = ioctl(fd, SPI_IOC_WR_MODE32, &req->head->mode);
	if (ret == -1) {
		WPRINTF("can't set spi mode\n");
		goto err;
	}

	ret = ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &req->head->bits_per_word);
	if (ret == -1) {
		WPRINTF("can't set bits per word\n");
		goto err;
	}

	ret = ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &req->head->freq);
	if (ret == -1) {
		WPRINTF("can't set max speed hz");
		goto err;
	}

	DPRINTF("spi mode: 0x%x\n", req->head->mode);
	DPRINTF("bits per word: %u\n", req->head->bits_per_word);
	DPRINTF("max speed: %u Hz\n", req->head->freq);
	DPRINTF("tx nbits: %u\n", req->head->tx_nbits);
	DPRINTF("rx nbits: %u\n", req->head->rx_nbits);

	struct spi_ioc_transfer tr = {
		.tx_buf = (unsigned long)req->tx_buf,
		.rx_buf = (unsigned long)req->rx_buf,
		.len = req->tx_buf_size,
		.speed_hz = req->head->freq,
		.delay_usecs = req->head->cs_delay_hold_ns,
		.bits_per_word = req->head->bits_per_word,
		.cs_change = req->head->cs_change,
		.tx_nbits = req->head->tx_nbits,
		.rx_nbits = req->head->rx_nbits,
		.word_delay_usecs = req->head->word_delay_ns,
	};

	/* return 1 on success */
	ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
	if (ret < 1) {
		WPRINTF("fail to send spi message to spidev%d.%d",
				data->bus, data->cs);
		ret = -1;
		goto err;
	}
	ret = 0;

err:
	return ret ? VIRTIO_SPI_TRANS_ERR : VIRTIO_SPI_TRANS_OK;
}

static struct vspidev_be vspidev_physical = {
	.type = VSPIDEV_TYPE_PHYSICAL,
	.name = "physical",
	.init = spidev_init_physical,
	.deinit = spidev_deinit_physical,
	.transfer = spidev_transfer_physical,
};

struct vspidev_tcp_data {
	struct vspidev *vspidev;
	int 		port;	/* TCP socket to connect */
	int 		fd;
};

static int spidev_init_tcp(struct vspidev *vspidev, char *opts)
{
	struct vspidev_tcp_data *data = calloc(1, sizeof(*data));
	char *t;
	int ret = 0;
	struct sockaddr_in server;

	if (data == NULL) {
		WPRINTF("memory allocation failed\n");
		return -1;
	}
	data->vspidev = vspidev;

	if (dm_strtoi(opts, &t, 10, &data->port) || data->port < 0) {
		ret = -1;
		goto err_free;
	}
	server.sin_family = AF_INET;
	server.sin_port = htons(data->port);
	server.sin_addr.s_addr = inet_addr("127.0.0.1");

	data->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (data->fd < 0) {
		WPRINTF("fail to open socket\n");
		ret = -1;
		goto err_free;
	}
	if (connect(data->fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
		WPRINTF("fail to connect to port %d\n", data->port);
		ret = -1;
		goto err_closefd;
	}

	vspidev->priv = data;

	return 0;

err_closefd:
	close(data->fd);
err_free:
	free(data);
	return ret;
}

static inline int64_t get_time_ms(void)
{
	struct timespec tms;
	if (clock_gettime(CLOCK_REALTIME, &tms)) {
		return -1;
	}
	return tms.tv_sec * 1000 + tms.tv_nsec / 1000000;
}

static void spidev_deinit_tcp(struct vspidev *vspidev)
{
	struct vspidev_tcp_data *data = vspidev->priv;

	if (data && data->fd > 0) {
		close(data->fd);
	}
	free(data);
	vspidev->priv = NULL;
}

#define RW_TIMEOUT_MS	200

static int write_all_timeout(int fd, const void *buf, size_t count, size_t to_ms)
{
	ssize_t written = 0, len;
	int64_t start = get_time_ms();

	do {
		len = write(fd, buf + written, count - written);
		if (len == -1) {
			if (errno == EINTR || errno == EAGAIN) {
				len = 0;
			} else {
				return -1;
			}
		}
		written += len;
		if (get_time_ms() > start + to_ms) {
			return -1;
		}
	} while (written < len);

	return 0;
}

static int read_all_timeout(int fd, void *buf, size_t count, size_t to_ms)
{
	ssize_t readlen = 0, len;
	int64_t start = get_time_ms();

	do {
		len = read(fd, buf + readlen, count - readlen);
		if (len == -1) {
			if (errno == EINTR || errno == EAGAIN) {
				len = 0;
			} else {
				return -1;
			}
		}
		readlen += len;
		if (get_time_ms() > start + to_ms) {
			return -1;
		}
	} while (readlen < len);

	return 0;
}

static uint8_t spidev_transfer_tcp(struct vspidev *vspidev, struct virtio_spi_transfer_req *req)
{
	int ret = 0;
	struct vspidev_tcp_data *data = vspidev->priv;
	int fd = data->fd;

	if (fd <= 0) {
		WPRINTF("Not a valid fd to access spidev emulated at port %d\n",
				data->port);
		ret = -1;
		goto err;
	}
	DPRINTF("%s: fd %d\n", __func__, fd);

	uint32_t len = htonl(req->tx_buf_size);
	ret = write_all_timeout(data->fd, &len, sizeof(uint32_t), RW_TIMEOUT_MS);
	ret += write_all_timeout(data->fd, req->tx_buf, req->tx_buf_size, RW_TIMEOUT_MS);
	if (ret < 0) {
		WPRINTF("fail to send data to spi device\n");
		ret = -1;
		goto err;
	}
	ret = read_all_timeout(data->fd, req->rx_buf, req->rx_buf_size, RW_TIMEOUT_MS);
	if (ret < 0) {
		WPRINTF("fail to receive data from spi device\n");
		ret = -1;
		goto err;
	}

err:
	return ret ? VIRTIO_SPI_TRANS_ERR : VIRTIO_SPI_TRANS_OK;
}


static struct vspidev_be vspidev_tcp = {
	.type = VSPIDEV_TYPE_TCP,
	.name = "tcp",
	.init = spidev_init_tcp,
	.deinit = spidev_deinit_tcp,
	.transfer = spidev_transfer_tcp,
};

struct vspidev_be *vspidev_bes[] = {
	&vspidev_null,
	&vspidev_loopback,
	&vspidev_physical,
	&vspidev_tcp,
};

struct vspidev_be *find_vspidev_be_from_name(const char *name)
{
	int i;
	struct vspidev_be *be;

	for(i = 0; i < ARRAY_SIZE(vspidev_bes); i++) {
		be = vspidev_bes[i];
		if (strncmp(be->name, name, strlen(be->name)) == 0) {
			return be;
		}
	}
	return NULL;
}

struct virtio_spi_config {
	uint16_t cs_num;
}__attribute__((packed));

/*
 * Virtio SPI Controller
 */
struct virtio_spi {
	struct virtio_base base;
	struct virtio_vq_info vq;
	struct virtio_spi_config config;
	struct vspidev *vspidevs[MAX_SPIDEVS];
	int spidev_num;
	pthread_mutex_t mtx;
	pthread_t req_tid;
	pthread_mutex_t req_mtx;
	pthread_cond_t req_cond;
	int in_process;
	int closing;
};

static void
virtio_spi_reset(void *vdev)
{
	struct virtio_spi *vspi = vdev;

	DPRINTF("device reset requested !\n");
	virtio_reset_dev(&vspi->base);
}

static void
virtio_spi_notify(void *vdev, struct virtio_vq_info *vq)
{
	struct virtio_spi *vspi = vdev;

	if (!vq_has_descs(vq))
		return;

	pthread_mutex_lock(&vspi->req_mtx);
	if (!vspi->in_process)
		pthread_cond_signal(&vspi->req_cond);
	pthread_mutex_unlock(&vspi->req_mtx);
}

static int
virtio_spi_read_cfg(void *vdev, int offset, int size, uint32_t *retval)
{
	struct virtio_spi *vspi = vdev;
	void *ptr;

	ptr = (uint8_t *)&vspi->config + offset;
	memcpy(retval, ptr, size);

	return 0;
}

static struct virtio_ops virtio_spi_ops = {
	"virtio_spi",		/* our name */
	1,			/* we support 1 virtqueue */
	sizeof(struct virtio_spi_config), /* config reg size */
	virtio_spi_reset,	/* reset */
	virtio_spi_notify,	/* device-wide qnotify */
	virtio_spi_read_cfg,	/* read PCI config */
	NULL,			/* write PCI config */
	NULL,			/* apply negotiated features */
	NULL,			/* called on guest set status */
};

static void
virtio_spi_req_stop(struct virtio_spi *vspi)
{
	void *jval;

	pthread_mutex_lock(&vspi->req_mtx);
	vspi->closing = 1;
	pthread_cond_broadcast(&vspi->req_cond);
	pthread_mutex_unlock(&vspi->req_mtx);
	pthread_join(vspi->req_tid, &jval);
}

static void *
virtio_spi_proc_thread(void *arg)
{
	struct virtio_spi *vspi = arg;
	struct vspidev *vspidev;
	struct virtio_vq_info *vq = &vspi->vq;
	struct iovec iov[4];
	uint16_t idx, flags[4];
	struct virtio_spi_transfer_req req;
	int n;
	struct virtio_spi_out_hdr *out_hdr;
	struct virtio_spi_in_hdr *in_hdr;

	for (;;) {
		pthread_mutex_lock(&vspi->req_mtx);

		vspi->in_process = 0;
		while (!vq_has_descs(vq) && !vspi->closing)
			pthread_cond_wait(&vspi->req_cond, &vspi->req_mtx);

		if (vspi->closing) {
			pthread_mutex_unlock(&vspi->req_mtx);
			return NULL;
		}
		vspi->in_process = 1;
		pthread_mutex_unlock(&vspi->req_mtx);
		do {
			n = vq_getchain(vq, &idx, iov, 4, flags);
			if (n != 4) {
				WPRINTF("virtio_spi_proc: failed to get iov from virtqueue\n");
				continue;
			}
			memset(&req, 0, sizeof(req));
			out_hdr = iov[0].iov_base;
			in_hdr = iov[3].iov_base;
			req.head = out_hdr;
			req.tx_buf = iov[1].iov_base;
			req.tx_buf_size = iov[1].iov_len;
			req.rx_buf = iov[2].iov_base;
			req.rx_buf_size = iov[2].iov_len;

			if (req.head->slave_id >= vspi->spidev_num) {
				in_hdr->result = -1;
			} else {
				vspidev = vspi->vspidevs[req.head->slave_id];
				in_hdr->result = vspidev->be->transfer(vspidev, &req);
			}
			vq_relchain(vq, idx, 1);
		} while (vq_has_descs(vq));
		vq_endchains(vq, 0);
	}
}


static int
virtio_spi_parse(struct virtio_spi *vspi, char *optstr)
{
	char *cp, *type;
	struct vspidev *vspidev;
	struct vspidev_be *vspidev_be;
	int ret;

	while (optstr != NULL) {
		cp = strsep(&optstr, ",");
		if (cp != NULL && *cp !='\0') {
			type = strsep(&cp, ":");
			vspidev_be = find_vspidev_be_from_name(type);
			if (vspidev_be == NULL) {
				WPRINTF("Not supported type %s\n", type);
				return -1;
			}
			vspidev = calloc(1, sizeof(struct vspidev));
			if (!vspidev) {
				WPRINTF("%s: fail to calloc\n", __func__);
				return -1;
			}

			vspi->vspidevs[vspi->spidev_num] = vspidev;
			vspidev->vspi = vspi;
			vspidev->type = vspidev_be->type;
			vspidev->cs = vspi->spidev_num++;
			vspidev->be = vspidev_be;

			ret = vspidev_be->init(vspidev, cp);
			if (ret) {
				WPRINTF("Fail to init SPI device %d, type: %s\n",
						vspidev->cs, vspidev_be->name);
				return -1;
			}
			DPRINTF("init SPI device %s@%d\n", vspidev_be->name, vspidev->cs);
		}
	}
	return 0;
}

static void
vspi_remove_devices(struct virtio_spi *vspi)
{
	int i;
	struct vspidev *vspidev;

	for (i = 0; i < vspi->spidev_num; i++) {
		vspidev = vspi->vspidevs[i];
		if (vspidev && vspidev->be->deinit) {
			vspidev->be->deinit(vspidev);
			free(vspidev);
			vspi->vspidevs[i] = NULL;
		}
	}
	vspi->spidev_num = 0;
}


static int
virtio_spi_init(struct vmctx *ctx, struct pci_vdev *dev, char *opts)
{
	struct virtio_spi *vspi;
	pthread_mutexattr_t attr;
	int rc = -1;

	vspi = calloc(1, sizeof(struct virtio_spi));
	if (!vspi) {
		WPRINTF("calloc returns NULL\n");
		return -ENOMEM;
	}

	if (virtio_spi_parse(vspi, opts)) {
		WPRINTF("failed to parse parameters\n");
		goto mtx_fail;
	}
	vspi->config.cs_num = vspi->spidev_num;

	/* init mutex attribute properly to avoid deadlock */
	rc = pthread_mutexattr_init(&attr);
	if (rc) {
		WPRINTF("mutexattr init failed with erro %d!\n", rc);
		goto mtx_fail;
	}
	rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	if (rc) {
		WPRINTF("mutexattr_settype failed with "
					"error %d!\n", rc);
		goto mtx_fail;
	}

	rc = pthread_mutex_init(&vspi->mtx, &attr);
	if (rc) {
		WPRINTF("pthread_mutex_init failed with "
					"error %d!\n", rc);
		goto mtx_fail;
	}

	/* init virtio struct and virtqueues */
	virtio_linkup(&vspi->base, &virtio_spi_ops, vspi, dev, &vspi->vq, BACKEND_VBSU);
	vspi->base.mtx = &vspi->mtx;
	vspi->base.device_caps = VIRTIO_SPI_HOSTCAPS;
	vspi->vq.qsize = 64;

	pci_set_cfgdata16(dev, PCIR_DEVICE, VIRTIO_DEV_SPI);
	pci_set_cfgdata16(dev, PCIR_VENDOR, VIRTIO_VENDOR);
	pci_set_cfgdata8(dev, PCIR_CLASS, 0);
	pci_set_cfgdata16(dev, PCIR_SUBDEV_0, VIRTIO_TYPE_SPI);
	pci_set_cfgdata16(dev, PCIR_SUBVEND_0, VIRTIO_VENDOR);

	if (virtio_interrupt_init(&vspi->base, virtio_uses_msix())) {
		WPRINTF("failed to init interrupt");
		rc = -1;
		goto fail;
	}
	rc = virtio_set_modern_bar(&vspi->base, false);
	vspi->in_process = 0;
	vspi->closing = 0;
	pthread_mutex_init(&vspi->req_mtx, NULL);
	pthread_cond_init(&vspi->req_cond, NULL);
	pthread_create(&vspi->req_tid, NULL, virtio_spi_proc_thread, vspi);
	pthread_setname_np(vspi->req_tid, "virtio-spi");

	return 0;

fail:
	pthread_mutex_destroy(&vspi->mtx);
mtx_fail:
	vspi_remove_devices(vspi);
	free(vspi);
	return rc;
}

static void
virtio_spi_deinit(struct vmctx *ctx, struct pci_vdev *dev, char *opts)
{
	struct virtio_spi *vspi;

	if (dev->arg) {
		DPRINTF("deinit\n");
		vspi = (struct virtio_spi *) dev->arg;
		virtio_spi_req_stop(vspi);
		vspi_remove_devices(vspi);
		pthread_mutex_destroy(&vspi->req_mtx);
		pthread_mutex_destroy(&vspi->mtx);
		virtio_spi_reset(vspi);
		free(vspi);
		dev->arg = NULL;
	}
}

static void
acpi_add_spi_controller(struct pci_vdev *dev, int spi_bus)
{
	dsdt_line("Device (SPI%d)", spi_bus);
	dsdt_line("{");
	dsdt_line("    Name (_ADR, 0x%04X%04X)", dev->slot, dev->func);
	dsdt_line("");
	dsdt_line("}");
}

static void
acpi_add_spi_dev(int spi_bus, int cs)
{
	dsdt_line("Scope(SPI%d)", spi_bus);
	dsdt_line("{");
	dsdt_line("    Device (TP%d)", cs);
	dsdt_line("    {");
	dsdt_line("    Name (_HID, \"SPT0001\")");
	dsdt_line("    Name (_DDN, \"SPI test device connected to CS%d\")", cs);
	dsdt_line("    Name (_CRS, ResourceTemplate ()  // _CRS: Current Resource Settings");
	dsdt_line("    {");
	dsdt_line("        SpiSerialBusV2 (%d, PolarityLow, FourWireMode, 8,", cs);
	dsdt_line("            ControllerInitiated, 1000000, ClockPolarityLow,");
	dsdt_line("            ClockPhaseFirst, \"\\\\_SB.PCI0.SPI%d\",", spi_bus);
	dsdt_line("            0x00, ResourceConsumer, , Exclusive,");
	dsdt_line("            )");
	dsdt_line("    })");
	dsdt_line("    }");
	dsdt_line("");
	dsdt_line("}");
}

static void
virtio_spi_dsdt(struct pci_vdev *dev)
{
	int i, spi_bus;
	struct virtio_spi *vspi = (struct virtio_spi *) dev->arg;

	spi_bus = acpi_spi_controller_num;
	acpi_add_spi_controller(dev, spi_bus);
	DPRINTF("add dsdt for spi controller #%d@%02x:%02x.%01x\n", spi_bus,
			dev->bus, dev->slot, dev->func);

	for (i = 0; i < vspi->spidev_num; i++) {
		acpi_add_spi_dev(spi_bus, i);
		DPRINTF("add dsdt for %s@spi%d-%d \n",
			vspi->vspidevs[i]->be->name, spi_bus, i);
	}
	acpi_spi_controller_num++;

	return;
}

struct pci_vdev_ops pci_ops_virtio_spi = {
	.class_name		= "virtio-spi",
	.vdev_init		= virtio_spi_init,
	.vdev_deinit		= virtio_spi_deinit,
	.vdev_barwrite		= virtio_pci_write,
	.vdev_barread		= virtio_pci_read,
	.vdev_write_dsdt	= virtio_spi_dsdt,
};
DEFINE_PCI_DEVTYPE(pci_ops_virtio_spi);
