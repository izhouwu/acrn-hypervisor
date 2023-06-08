/*
 * Copyright (C) 2018-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "atomic.h"
#include "dm.h"
#include "pci_core.h"
#include "virtio.h"
#include "virtio_camera.h"
#include "virtio_kernel.h"
#include "vmmapi.h"
#include "vICamera.h"

#define CAMERA_WIDTH 1920
#define CAMERA_HEIGHT 1080
#define VIRTIO_CAMERA_MAXSEGS 256

/*
 * Host capabilities
 */
#define VIRTIO_CAMERA_S_HOSTCAPS		(1UL << VIRTIO_F_VERSION_1)

static void virtio_camera_dev_init(int camera_id);
static int virtio_camera_hal_init(int camera_id);
static int virtio_camera_open(int camera_id);
static int virtio_camera_close(int camera_id);
static int virtio_camera_req_bufs(int camera_id);
static int virtio_camera_start_stream(int camera_id);
static int virtio_camera_stop_stream(int camera_id);
static int map_buffer(int camera_id, int i);
static int unmap_buffer(int camera_id, int buffer_index);
struct camera_ops g_hal_ops = {0};
void *g_hal_handle = NULL;

struct camera_info g_camera_thread_param[VIRTIO_CAMERA_NUMQ];

static struct camera_dev camera_devs[] = {
	{
		.id = 0,
		.name = "video0",
		.type = HAL_INTERFACE,
	},
	{
		.id = 1,
		.name = "video1",
		.type = V4L2_INTERFACE,
	},
	{
		.id = 2,
		.name = "video2",
		.type = V4L2_INTERFACE,
	},
	{
		.id = 3,
		.name = "video3",
		.type = V4L2_INTERFACE,
	},
	{
		.id = 4,
		.name = "video4",
		.type = V4L2_INTERFACE,
	},
	{
		.id = 5,
		.name = "video5",
		.type = V4L2_INTERFACE,
	},
	{
		.id = 6,
		.name = "video6",
		.type = V4L2_INTERFACE,
	},
	{
		.id = 7,
		.name = "video7",
		.type = V4L2_INTERFACE,
	},
};

static int virtio_camera_req_bufs(int camera_id)
{
	pr_info("virtio_camera %s Enter\n", __func__);

	if (camera_devs[camera_id].ops.req_bufs)
		return camera_devs[camera_id].ops.req_bufs(camera_id);
	else
		return -1;
}

static int virtio_camera_wrapper_config_streams(int camera_id)
{
	int ret = 0;
	struct camera_dev* p = &camera_devs[camera_id];
	stream_config_t* stream_list = &camera_devs[camera_id].stream_list;

	pr_info("virtio_camera %s Enter \n", __func__);

	if (p->type == HAL_INTERFACE) {
		ret = p->ops.config_streams(camera_id, stream_list);
	} else {
		struct v4l2_format fmt;

		memset(&fmt, 0, sizeof(struct v4l2_format));

		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		fmt.fmt.pix.width = stream_list->streams[0].width;
		fmt.fmt.pix.height = stream_list->streams[0].height;
		fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
		fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

		ret = p->ops.set_parameters(camera_id, &fmt);
	}

	return ret;
};

static int virtio_camera_stream_qbuf(int camera_id, camera_buffer_t **buffer, int num_buffers, void *settings)
{
	pr_info("virtio_camera %s Enter\n", __func__);

	if (camera_devs[camera_id].ops.stream_qbuf)
		return camera_devs[camera_id].ops.stream_qbuf(camera_id, buffer, num_buffers, settings);
	else
		return -1;
};

static int virtio_camera_stream_dqbuf(int camera_id, int stream_id, camera_buffer_t **buffer, void *settings)
{
	pr_info("virtio_camera %s Enter\n", __func__);

	if (camera_devs[camera_id].ops.stream_dqbuf)
		return camera_devs[camera_id].ops.stream_dqbuf(camera_id, stream_id, buffer, settings);
	else
		return -1;
}

static int virtio_camera_hal_init(int camera_id)
{
	struct camera_dev *p = &camera_devs[camera_id];

	pr_info("virtio_camera %s Enter\n", __func__);

	if (p->ops.hal_init)
		return p->ops.hal_init();

	return -1;
};

static int virtio_camera_open(int camera_id)
{
	struct camera_dev *p = &camera_devs[camera_id];

	pr_info("virtio_camera %s Enter\n", __func__);

	if (p->ops.open)
		return p->ops.open(camera_id);

	return -1;
};

static int virtio_camera_close(int camera_id)
{
	struct camera_dev *p = &camera_devs[camera_id];

	pr_info("virtio_camera %s Enter\n", __func__);

	if (p->ops.close)
		p->ops.close(camera_id);

	return 0;
};

static int virtio_camera_start_stream(int camera_id)
{
	pr_info("virtio_camera %s Enter\n", __func__);

	if (camera_devs[camera_id].ops.start_stream)
		return camera_devs[camera_id].ops.start_stream(camera_id);

	return -1;
};

static int virtio_camera_stop_stream(int camera_id)
{
	pr_info("virtio_camera %s Enter\n", __func__);

	if (camera_devs[camera_id].ops.stop_stream)
		return camera_devs[camera_id].ops.stop_stream(camera_id);

	return -1;
};

int iov_from_buf(struct iovec *iov, uint32_t segment, void *pdata, int size)
{
	int i;
	int length = size;
	void* src = pdata;

	for (i = 0; i < segment; i++) {
		int tmp = iov[i].iov_len < length ? iov[i].iov_len : length;
		memcpy(iov[i].iov_base, src, tmp);
		pr_info("virtio_camera iov_from_buf iov[%d].iov_len %ld\n", i, iov[i].iov_len);

		src += tmp;
		length -= tmp;
		if (length <= 0)
			break;
	}

	return 0;
};

static int get_vq_index(struct virtio_camera *vcamera, struct virtio_vq_info *vq)
{
	int i;

	for (i = 0; i < VIRTIO_CAMERA_NUMQ; i++)
	{
		if (vq == &vcamera->queues[i])
			return i;
	}

	return -1;
}

static void virtio_camera_notify(void *vdev, struct virtio_vq_info *vq)
{
	struct virtio_camera *vcamera = vdev;
	int index = get_vq_index(vcamera, vq);

	pr_err("virtio_camera_notify get the vq index is %d\n", index);

	if ((index < 0) || (index > VIRTIO_CAMERA_NUMQ))
		return;

	if (!vq_has_descs(vq))
		return;

	pr_info("vcamera thread index %d vq_has_descs\n", index);
	pthread_mutex_lock(&vcamera->vq_related[index].req_mutex);
	if (!vcamera->vq_related[index].in_process) {
		pr_info("vcamera thread index %d wake up %p\n", index, &vcamera->vq_related[index].req_cond);
		pthread_cond_signal(&vcamera->vq_related[index].req_cond);
	}
	pthread_mutex_unlock(&vcamera->vq_related[index].req_mutex);
}

static int udmabuf_fd(void)
{
	static bool first = true;
	static int udmabuf;

	if (!first)
		return udmabuf;

	first = false;

	udmabuf = open("/dev/udmabuf", O_RDWR);
	if (udmabuf < 0)
		pr_err("Could not open /dev/udmabuf\n");

	return udmabuf;
}

static struct dma_buf_info *
virtio_camera_create_udmabuf(struct virtio_camera *vcamera, struct iovec *entries, int nr_entries)
{
	struct udmabuf_create_list *list;
	int udmabuf, i, dmabuf_fd;
	struct vm_mem_region ret_region;
	bool fail_flag;
	struct dma_buf_info *info;

	udmabuf = udmabuf_fd();
	if (udmabuf < 0)
		return NULL;

	fail_flag = false;
	list = malloc(sizeof(*list) + sizeof(struct udmabuf_create_item) * nr_entries);
	info = malloc(sizeof(*info));
	if ((info == NULL) || (list == NULL)) {
		free(list);
		free(info);
		return NULL;
	}

	for (i = 0; i < nr_entries; i++) {
		if (!vm_find_memfd_region(vcamera->base.dev->vmctx, (vm_paddr_t)entries[i].iov_base, &ret_region)) {
			fail_flag = true;
			pr_err("%s : Failed to find memfd for %llx.\n", __func__, entries[i].iov_base);
			break;
		}
		list->list[i].memfd = ret_region.fd;
		list->list[i].offset = ret_region.fd_offset;
		list->list[i].size = (uint32_t)entries[i].iov_len;
	}

	list->count = nr_entries;
	list->flags = UDMABUF_FLAGS_CLOEXEC;
	if (fail_flag)
		dmabuf_fd = -1;
	else
		dmabuf_fd = ioctl(udmabuf, UDMABUF_CREATE_LIST, list);

	if (dmabuf_fd < 0) {
		free(info);
		info = NULL;
		pr_err("%s : Failed to create the dmabuf. %s\n", __func__, strerror(errno));
	}

	if (info) {
		info->dmabuf_fd = dmabuf_fd;
		atomic_store(&info->ref_count, 1);
	}
	free(list);
	return info;
}

static void virtio_camera_reset(void *vdev)
{
	//DODO, reset the camera device
	pr_info("virtio_camera  reset camera...\n");
}

static int virtio_camera_cfgread(void *vdev, int offset, int size, uint32_t *retval)
{
	pr_info("virtio_camera  camera_cfgread...\n");
	struct virtio_camera *camera = vdev;
	void *ptr;

	ptr = (uint8_t *)&camera->config + offset;
	memcpy(retval, ptr, size);
	return 0;
}

static int virtio_camera_cfgwrite(void *vdev, int offset, int size, uint32_t value)
{
	pr_err("virtio_camera write to read-only registers.\n");
	return 0;
}

static void virtio_camera_neg_features(void *vdev, uint64_t negotiated_features)
{
	pr_info("virtio_camera  camera_neg_features...\n");
}

static void virtio_camera_set_status(void *vdev, uint64_t status) { pr_info("virtio_camera  camera_set_status...\n"); }

static struct virtio_ops virtio_camera_ops = {
	"virtio_camera",					 /* our name */
	VIRTIO_CAMERA_NUMQ,				  /* we support one virtqueue */
	sizeof(struct virtio_camera_config), /* config reg size */
	virtio_camera_reset,				 /* reset */
	virtio_camera_notify,				/* device-wide qnotify */
	virtio_camera_cfgread,			   /* read virtio config */
	virtio_camera_cfgwrite,			  /* write virtio config */
	virtio_camera_neg_features,		  /* apply negotiated features */
	virtio_camera_set_status,			/* called on guest set status */
};

static void *virtio_dqbuf_thread(void *data)
{
	int ret = 0;
	struct camera_info *_info = (struct camera_info *)data;
	struct virtio_camera *vcamera = _info->vcamera;

	int camera_id = _info->camera_id;
	struct virtio_vq_info *vq = &vcamera->queues[camera_id];
	int i = 0;

	pr_info("vcamera virtio_dqbuf_thread is created\n");
	do {
		int stream_id = 0;

		camera_buffer_t buffer;
		camera_buffer_t *buf = &buffer;

		pthread_mutex_lock(&camera_devs[camera_id].capture_list_mutex);
		if (!STAILQ_EMPTY(&camera_devs[camera_id].capture_list)) {
			pr_info("virtio_camera  call virtio_camera_stream_dqbuf\n");

			ret = virtio_camera_stream_dqbuf(camera_id, stream_id, &buf, NULL);

			pr_info("virtio_camera camera %d virtio_camera_stream_dqbuf ret = %d\n", camera_id, ret);
			if (ret == 0 && buf->addr) {
				// (fill req to virtqueue)
				struct capture_buffer *p = STAILQ_FIRST(&camera_devs[camera_id].capture_list);
				pr_info("vcamera %d DQ a buffer p->idx = %d pdata %p uuid %s\n", camera_id, p->idx,
					buf->addr, p->uuid);

				vq_relchain(vq, p->idx, sizeof(struct virtio_camera_request));
				vq_endchains(vq, 0);

				STAILQ_REMOVE(&camera_devs[camera_id].capture_list, p, capture_buffer, link);
			}
			pthread_mutex_unlock(&camera_devs[camera_id].capture_list_mutex);
			pr_info("virtio_camera  virtio_camera_stream_dqbuf ret = %d buf->index %d %p\n",
					ret,
					buf->index,
					buf->addr);
			i++;
		} else {
			pthread_mutex_unlock(&camera_devs[camera_id].capture_list_mutex);
			if (camera_devs[camera_id].stream_state == 0) {
				pr_warn("virtio_camera vcamera EXIT loop\n");
				break;
			} else {
				pr_err("virtio_camera vcamera buffer list empty\n");
				usleep(1000 * 5);
			}
		}

	} while (1);

	return NULL;
}

static int init_streams(int camera_id, int format, int width, int height)
{
	memset(&camera_devs[camera_id].streams[0], 0, sizeof(stream_t));

	camera_devs[camera_id].streams[0].format = V4L2_PIX_FMT_YUYV;
	camera_devs[camera_id].streams[0].width = width;
	camera_devs[camera_id].streams[0].height = height;
	camera_devs[camera_id].streams[0].memType = V4L2_MEMORY_USERPTR;
	camera_devs[camera_id].streams[0].field = 0;
	camera_devs[camera_id].streams[0].size = width * height * 2;
	camera_devs[camera_id].streams[0].stride = width * height * 2;
	camera_devs[camera_id].stream_list.num_streams = 1;
	camera_devs[camera_id].stream_list.streams = camera_devs[camera_id].streams;
	camera_devs[camera_id].stream_list.operation_mode = 2;

	return 0;
};

static int virtio_camera_handle(struct virtio_camera_request *req, struct virtio_camera_request *response,
	struct iovec *buf_describle_vec, struct virtio_camera *vcam, uint16_t idx, int camera_id)
{
	int ret;
	int i;
	int j;
	int buffer_index;
	int buffer_count = camera_devs[camera_id].buffer_count;
	struct capture_buffer* p = NULL;
	struct v4l2_fmtdesc format_desc = {};
	struct dma_buf_info* pdma;
	camera_buffer_t* buf;
	char thread_name[128];

	response->type = VIRTIO_CAMERA_RET_OK;

	switch (req->type) {
	case VIRTIO_CAMERA_OPEN:
	case VIRTIO_CAMERA_CLOSE:
		break;

	case VIRTIO_CAMERA_GET_FORMAT:
		response->u.format.camera_format.width = CAMERA_WIDTH;
		response->u.format.camera_format.height = CAMERA_HEIGHT;
		response->u.format.camera_format.stride = CAMERA_WIDTH * 2;
		response->u.format.pixel_format_type = V4L2_PIX_FMT_YUYV;
		response->u.format.camera_format.sizeimage = CAMERA_WIDTH * CAMERA_HEIGHT * 2;
		break;

	case VIRTIO_CAMERA_SET_FORMAT:
		pr_info("##format.pixel_format_type is: %d \n", req->u.format.pixel_format_type);
		pr_info("##format.camera_format.height is: %d \n", req->u.format.camera_format.height);
		pr_info("##format.width is: %d \n", req->u.format.camera_format.width);
		pr_info("##format.camera_format.sizeimage is: %d \n", req->u.format.camera_format.sizeimage);
		pr_info("##format.camera_format.stride is: %d \n", req->u.format.camera_format.stride);

		response->u.format.pixel_format_type = req->u.format.pixel_format_type;
		response->u.format.camera_format.height = req->u.format.camera_format.height;
		response->u.format.camera_format.width = req->u.format.camera_format.width;
		response->u.format.camera_format.sizeimage =
			req->u.format.camera_format.height * req->u.format.camera_format.width * 2;
		response->u.format.camera_format.stride = req->u.format.camera_format.width * 2;

		init_streams(camera_id,V4L2_PIX_FMT_YUYV, req->u.format.camera_format.width,
					 req->u.format.camera_format.height);
		ret = virtio_camera_wrapper_config_streams(camera_id);
		pr_info("virtio_camera virtio_camera_wrapper_config_streams ret = %d\n", ret);
		break;

	case VIRTIO_CAMERA_TRY_FORMAT:
		response->u.format.camera_format.width = CAMERA_WIDTH;
		response->u.format.camera_format.height = CAMERA_HEIGHT;
		response->u.format.camera_format.stride = CAMERA_WIDTH * 2;
		response->u.format.pixel_format_type = V4L2_PIX_FMT_YUYV;
		response->u.format.camera_format.sizeimage = CAMERA_WIDTH * CAMERA_HEIGHT * 2;
		break;

	case VIRTIO_CAMERA_ENUM_FORMAT:
		pr_info("virtio_camera req->index %d\n", req->index);
		format_desc.index = 0;
		format_desc.pixelformat = V4L2_PIX_FMT_YUYV;
		format_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		response->u.format.pixel_format_type = format_desc.pixelformat;

		if (req->index >= 1) {
			response->type = 555;
			pr_info("virtio_camera return 555\n");
		}
		break;
	case VIRTIO_CAMERA_ENUM_SIZE:
		break;

	case VIRTIO_CAMERA_CREATE_BUFFER:

		pr_info("virtio_camera It is create buffer, has %d segments\n",req->u.buffer.segment);
		if (buffer_count >= MAX_BUFFER_COUNT) {
			pr_err("virtio_camera there is no space, buffer_count %d\n", buffer_count);
			response->type = VIRTIO_CAMERA_RET_OUT_OF_MEMORY;
			break;
		}

		camera_devs[camera_id].capture_buffers[buffer_count].segment = req->u.buffer.segment;

		camera_devs[camera_id].capture_buffers[buffer_count].iov = malloc(
			camera_devs[camera_id].capture_buffers[buffer_count].segment * sizeof(struct iovec));
		memcpy(camera_devs[camera_id].capture_buffers[buffer_count].iov,
			   buf_describle_vec->iov_base,
			   camera_devs[camera_id].capture_buffers[buffer_count].segment *
				   sizeof(struct iovec));
		pr_info("virtio_cameraThe buffer describle iov base is %p, len is %ld\n.",
				buf_describle_vec->iov_base,
				buf_describle_vec->iov_len);

		pdma = virtio_camera_create_udmabuf(
			vcam,
			camera_devs[camera_id].capture_buffers[buffer_count].iov,
			req->u.buffer.segment);

		if(NULL == pdma) {
			pr_err("virtio_camera create udmabuf faild\n");
			response->type = VIRTIO_CAMERA_RET_OUT_OF_MEMORY;
			break;
		}

		camera_devs[camera_id].capture_buffers[buffer_count].dmabuf_fd = pdma->dmabuf_fd;
		pr_info("virtio_camera_create_udmabuf return fd %d count %d\n.", pdma->dmabuf_fd, pdma->ref_count);

		camera_devs[camera_id].capture_buffers[buffer_count].length = 0;
		for (i = 0; i < req->u.buffer.segment; i++) {
			camera_devs[camera_id].capture_buffers[buffer_count].iov[i].iov_len =
				(uint32_t)camera_devs[camera_id]
					.capture_buffers[buffer_count]
					.iov[i]
					.iov_len; // Frontend provide this value is uint32_t, so need convert.
			pr_info("virtio_camera#The %dth segment of buffer, address is %p, len is %ld \n",
					i,
					camera_devs[camera_id].capture_buffers[buffer_count].iov[i].iov_base,
					camera_devs[camera_id].capture_buffers[buffer_count].iov[i].iov_len);
			// There is still GPA, need map to HVA
			camera_devs[camera_id]
				.capture_buffers[buffer_count]
				.iov[i]
				.iov_base = paddr_guest2host(
				vcam->base.dev->vmctx,
				(uintptr_t)camera_devs[camera_id].capture_buffers[buffer_count].iov[i].iov_base,
				camera_devs[camera_id].capture_buffers[buffer_count].iov[i].iov_len);
			pr_info("The %dth hva is  %p  \n",
					i,
					camera_devs[camera_id].capture_buffers[buffer_count].iov[i].iov_base);
			camera_devs[camera_id].capture_buffers[buffer_count].length +=
				camera_devs[camera_id].capture_buffers[buffer_count].iov[i].iov_len;
		}

		sprintf(camera_devs[camera_id].capture_buffers[buffer_count].uuid,
				"cap_buffer_id%d",
				buffer_count);
		memcpy(response->u.buffer.uuid,
			   camera_devs[camera_id].capture_buffers[buffer_count].uuid,
			   16);
		pr_info("virtio_camera response->u.buffer.uuid %s\n", response->u.buffer.uuid);
		ret = map_buffer(camera_id, buffer_count);
		if (ret != 0) {
			pr_err("virtio_camera create buffer faild! \n");
			response->type = VIRTIO_CAMERA_RET_OUT_OF_MEMORY;
		}

		camera_devs[camera_id].buffer_count++;

		break;

	case VIRTIO_CAMERA_DEL_BUFFER:
		pr_info("virtio_camera delete a buffer \n");

		for (buffer_index = 0; buffer_index < buffer_count; buffer_index++) {
			if (!memcmp(camera_devs[camera_id].capture_buffers[buffer_index].uuid,
						req->u.buffer.uuid,
						sizeof(camera_devs[camera_id].capture_buffers[buffer_index].uuid))) {
				break;
			} else if (buffer_index == (buffer_count - 1)) {
				pr_info("virtio_camera can't find the buffer %s\n", req->u.buffer.uuid);
			}
		}

		if (buffer_index < buffer_count) {
			unmap_buffer(camera_id, buffer_index);
			if (NULL != camera_devs[camera_id].capture_buffers[buffer_index].iov) {
				free(camera_devs[camera_id].capture_buffers[buffer_index].iov);
				camera_devs[camera_id].capture_buffers[buffer_index].iov = NULL;
			}
			for (i = buffer_index; i < buffer_count; i++)
				camera_devs[camera_id].capture_buffers[i] = camera_devs[camera_id].capture_buffers[i+1];

			if (buffer_count > 0)
				memset(&camera_devs[camera_id].capture_buffers[buffer_count - 1], 0,
					sizeof(struct capture_buffer));
			camera_devs[camera_id].buffer_count--;
		}

		break;

	case VIRTIO_CAMERA_QBUF:
		pr_info("virtio_camera Queue a buffer \n");

		for (buffer_index = 0; buffer_index < buffer_count; buffer_index++) {
			if (!memcmp(camera_devs[camera_id].capture_buffers[buffer_index].uuid,
						req->u.buffer.uuid,
						sizeof(camera_devs[camera_id].capture_buffers[buffer_index].uuid))) {
				camera_devs[camera_id].capture_buffers[buffer_index].idx = idx;
				camera_devs[camera_id].capture_buffers[buffer_index].response = response;
				pthread_mutex_lock(&camera_devs[camera_id].capture_list_mutex);
				pr_info("virtio_camera STAILQ_EMPTY ? %s\n",
					(STAILQ_EMPTY(&camera_devs[camera_id].capture_list) ? "NULL" : "NOT NULL"));
				if (STAILQ_EMPTY(&camera_devs[camera_id].capture_list)) {
					pr_info("virtio_camera STAILQ_INSERT_HEAD buffer %s\n", req->u.buffer.uuid);
					STAILQ_INSERT_HEAD(&camera_devs[camera_id].capture_list,
								&camera_devs[camera_id].capture_buffers[buffer_index],
								link);
				} else {
					pr_info("virtio_camera STAILQ_INSERT_TAIL buffer %s\n", req->u.buffer.uuid);
					STAILQ_INSERT_TAIL(&camera_devs[camera_id].capture_list,
								&camera_devs[camera_id].capture_buffers[buffer_index],
								link);
				}
				pthread_mutex_unlock(&camera_devs[camera_id].capture_list_mutex);
				break;
			} else if (buffer_index == (buffer_count - 1)) {
				pr_info("virtio_camera can't find the buffer %s\n", req->u.buffer.uuid);
			}
		}

		pr_info("virtio_camera camera_id %d req uuid %s, native addr %p uuid %s\n",
				camera_id,
				req->u.buffer.uuid,
				camera_devs[camera_id].capture_buffers[buffer_index].remapped_addr,
				camera_devs[camera_id].capture_buffers[buffer_index].uuid);

		for (j = 0; j < buffer_count; j++) {
			pr_info("virtio_camera camera_id %d native addr %p uuid %s\n",
					camera_id,
					camera_devs[camera_id].capture_buffers[j].remapped_addr,
					camera_devs[camera_id].capture_buffers[j].uuid);
		}

		buf = &camera_devs[camera_id].capture_buffers[buffer_index].buffer;
		buf->addr = camera_devs[camera_id].capture_buffers[buffer_index].remapped_addr;
		buf->sequence = -1;
		buf->timestamp = 0;
		buf->s = camera_devs[camera_id].streams[0];

		pr_info("virtio_camera camera_devs[camera_id].stream_state = %d\n",
			camera_devs[camera_id].stream_state);

		if (camera_devs[camera_id].stream_state == 0) {
			virtio_camera_req_bufs(camera_id);

			camera_devs[camera_id].stream_state = 1;
		}

		ret = virtio_camera_stream_qbuf(camera_id, &buf, 1, &buffer_index);
		pr_info("virtio_camera camera %d virtio_camera_stream_qbuf capture_buffers[%d].uuid %s ret = "
				"%d\n",
				camera_id,
				buffer_index,
				camera_devs[camera_id].capture_buffers[buffer_index].uuid,
				ret);
		break;

	case VIRTIO_CAMERA_STREAM_ON:
		pr_info("virtio_camera Stream on buffer_count = %d\n", buffer_count);

		virtio_camera_start_stream(camera_id);
		camera_devs[camera_id].stream_state = 1;


		int ret = pthread_create(&camera_devs[camera_id].vtid,NULL, virtio_dqbuf_thread,
								 &g_camera_thread_param[camera_id]);
		if (ret) {
			pr_err("Failed to create the virtio_dqbuf_thread.\n");
		} else {
			sprintf(thread_name, "virtio_dqbuf_thread_%d", camera_id);
			pthread_setname_np(camera_devs[camera_id].vtid, thread_name);
		}

		break;

	case VIRTIO_CAMERA_STREAM_OFF:
		pr_info("virtio_camera Stream off\n");
		camera_devs[camera_id].stream_state = 0;
		// clear all request
		pthread_mutex_lock(&camera_devs[camera_id].capture_list_mutex);

		STAILQ_FOREACH(p, &camera_devs[camera_id].capture_list, link)
		{
			p->response->type = VIRTIO_CAMERA_RET_UNSPEC;
			vq_relchain(&vcam->queues[camera_id], p->idx, sizeof(struct virtio_camera_request));
			vq_endchains(&vcam->queues[camera_id], 0);

			STAILQ_REMOVE(&camera_devs[camera_id].capture_list, p, capture_buffer, link);
		};

		pthread_mutex_unlock(&camera_devs[camera_id].capture_list_mutex);

		virtio_camera_stop_stream(camera_id);

		break;

	default:
		pr_err("virtio-camera: invalid request type\n");
		break;
	};

	return 0;
}

static void *virtio_camera_thread(void *data)
{
	struct camera_info *_info = (struct camera_info *)data;
	struct virtio_camera *vcamera = _info->vcamera;

	int camera_id = _info->camera_id;
	struct virtio_vq_info *vq = &vcamera->queues[camera_id];
	struct iovec iov[VIRTIO_CAMERA_MAXSEGS];
	struct virtio_camera_request req;
	struct virtio_camera_request *response;
	uint16_t idx, flags[VIRTIO_CAMERA_MAXSEGS];

	int n;

	pr_info("vcamera thread is created camera_id %d\n", camera_id);
	for (;;) {
		pthread_mutex_lock(&vcamera->vq_related[camera_id].req_mutex);

		vcamera->vq_related[camera_id].in_process = 0;
		while (!vq_has_descs(vq) && !vcamera->closing) {
			pr_info("vcamera thread camera_id %d wait event %p\n",camera_id,
				&vcamera->vq_related[camera_id].req_cond);
			pthread_cond_wait(&vcamera->vq_related[camera_id].req_cond,
				&vcamera->vq_related[camera_id].req_mutex);
		}
		pr_info("vcamera thread camera_id %d get event\n", camera_id);

		if (vcamera->closing) {
			pthread_mutex_unlock(&vcamera->vq_related[camera_id].req_mutex);
			return NULL;
		}
		vcamera->vq_related[camera_id].in_process = 1;
		pthread_mutex_unlock(&vcamera->vq_related[camera_id].req_mutex);
		do {
			n = vq_getchain(vq, &idx, iov, VIRTIO_CAMERA_MAXSEGS, flags);

			pr_notice("vcamera thread camera_id %d get vq_getchain\n", camera_id);

			if (n < 0) {
				pr_err("virtio-camera: invalid descriptors\n");
				continue;
			}

			if (n == 0) {
				pr_err("virtio-camera: get no available descriptors\n");
				continue;
			}

			memcpy(&req, iov[0].iov_base, sizeof(struct virtio_camera_request));
			response = (struct virtio_camera_request *)iov[n - 1].iov_base;

			pr_notice("virtio_camera the req type is %d vq size is %d\n", req.type, vq->qsize);
			virtio_camera_handle(&req, response, &iov[1], vcamera, idx, camera_id);

			if (req.type != VIRTIO_CAMERA_QBUF)
				vq_relchain(vq, idx, sizeof(struct virtio_camera_request));
			else
				pr_notice("The VIRTIO_CAMERA_QBUF idx is %d \n", idx);

		} while (vq_has_descs(vq));
		vq_endchains(vq, 0);
	}
	return NULL;
}

static void virtio_camera_dev_init(int camera_id)
{
	int ret;
	stream_t input_config;

	pr_info("virtio_camera camera %d type = %d\n", camera_id, camera_devs[camera_id].type);
	if (camera_devs[camera_id].type == HAL_INTERFACE) {
		camera_devs[camera_id].ops = g_hal_ops;
	}

	ret = virtio_camera_hal_init(camera_id);
	pr_info("virtio_camera virtio_camera_hal_init ret = %d\n", ret);

	ret = virtio_camera_open(camera_id);
	pr_info("virtio_camera virtio_camera_open ret = %d\n", ret);

	memset(&input_config, 0, sizeof(stream_t));
	input_config.format = -1;

	if (camera_devs[camera_id].ops.config_sensor_input) {
		ret = camera_devs[camera_id].ops.config_sensor_input(camera_id, &input_config);
		pr_info("virtio_camera config_sensor_input ret = %d\n", ret);
	}

	if (camera_devs[camera_id].ops.set_exposure) {
		ret = camera_devs[camera_id].ops.set_exposure(camera_id, 20);
		pr_info("virtio_camera set_exposure ret = %d\n", ret);
	}
}

static int map_buffer(int camera_id, int buffer_index)
{
	int ret;
	camera_buffer_t *buf = &camera_devs[camera_id].capture_buffers[buffer_index].buffer;

	memset(buf, 0, sizeof(camera_buffer_t));

	buf->s = camera_devs[camera_id].streams[0];
	buf->addr = mmap(NULL,
					 camera_devs[camera_id].capture_buffers[buffer_index].length,
					 PROT_READ | PROT_WRITE,
					 MAP_SHARED | MAP_POPULATE,
					 camera_devs[camera_id].capture_buffers[buffer_index].dmabuf_fd,
					 0);
	memset(buf->addr, 0, camera_devs[camera_id].capture_buffers[buffer_index].length);
	camera_devs[camera_id].capture_buffers[buffer_index].remapped_addr = buf->addr;
	pr_info("The capture_buffers[%d].length = %d memset addr %p no memset\n",
			buffer_index,
			camera_devs[camera_id].capture_buffers[buffer_index].length,
			buf->addr);
	ret = (buf->addr != NULL) ? 0 : -1;
	return ret;
}

static int unmap_buffer(int camera_id, int buffer_index)
{
	int ret;
	void *addr = &camera_devs[camera_id].capture_buffers[buffer_index].remapped_addr;

	ret = munmap(addr, camera_devs[camera_id].capture_buffers[buffer_index].length);
	if (ret != 0) {
		pr_err("unmap_buffer buffer_index %d faild\n", buffer_index);
	} else {
		pr_info("unmap_buffer buffer_index %d success\n", buffer_index);
		camera_devs[camera_id].capture_buffers[buffer_index].remapped_addr = NULL;
		close(camera_devs[camera_id].capture_buffers[buffer_index].dmabuf_fd);
	}

	return ret;
}

static int virtio_camera_init(struct vmctx *ctx, struct pci_vdev *dev, char *opts)
{
	struct virtio_camera *vcamera;
	pthread_mutexattr_t attr;
	int32_t ret;
	int i;

	vcamera = calloc(1, sizeof(struct virtio_camera));

	if (!vcamera) {
		pr_err(("vcamera init: fail to alloc virtio_camera\n"));
		return -1;
	}

	/* init mutex attribute properly */
	int mutexattr_type = virtio_uses_msix() ? PTHREAD_MUTEX_DEFAULT : PTHREAD_MUTEX_RECURSIVE;

	ret = pthread_mutexattr_init(&attr);
	if (ret)
		pr_err("vcamera init: mutexattr init fail, erro %d\n", ret);
	ret = pthread_mutexattr_settype(&attr, mutexattr_type);
	if (ret)
		pr_err("vcamera init: mutexattr_settype fail, erro %d\n", ret);
	ret = pthread_mutex_init(&vcamera->vcamera_mutex, &attr);
	if (ret)
		pr_err("vcamera init: mutexattr_settype fail, erro %d\n", ret);

	virtio_linkup(&vcamera->base, &virtio_camera_ops, vcamera, dev, vcamera->queues, BACKEND_VBSU);
	vcamera->base.mtx = &vcamera->vcamera_mutex;
	vcamera->base.device_caps = VIRTIO_CAMERA_S_HOSTCAPS;

	for (i = 0; i < VIRTIO_CAMERA_NUMQ; i++) {
		char thread_name[128];
		vcamera->queues[i].qsize = VIRTIO_CAMERA_RINGSZ;
		vcamera->queues[i].notify = virtio_camera_notify;
		vcamera->vq_related[i].in_process = 0;
		g_camera_thread_param[i].camera_id = i;
		g_camera_thread_param[i].vcamera = vcamera;

		ret = pthread_create(&vcamera->vcamera_tid[i],NULL,virtio_camera_thread,
			(void*)(&g_camera_thread_param[i]));
		if (ret) {
			pr_err("Failed to create the virtio_camera_thread.\n");
			return 0;
		};

		sprintf(thread_name, "acrn_virtio_camera_%d", i);
		pthread_setname_np(vcamera->vcamera_tid[i], thread_name);

		virtio_camera_dev_init(i);
		ret = pthread_mutex_init(&camera_devs[i].capture_list_mutex, &attr);
		if (ret)
			pr_err("vcamera init: camera_devs[%d].capture_list_mutex fail, erro %d\n", i, ret);
		STAILQ_INIT(&camera_devs[i].capture_list);
	}

	memcpy(vcamera->config.name, "hello_camera\0", 14);

	/* initialize config space */
	pci_set_cfgdata16(dev, PCIR_DEVICE, 0x1040 + VIRTIO_TYPE_CAMERA);
	pci_set_cfgdata16(dev, PCIR_VENDOR, VIRTIO_VENDOR);
	pci_set_cfgdata8(dev, PCIR_CLASS, PCIC_MULTIMEDIA);
	pci_set_cfgdata8(dev, PCIR_SUBCLASS, PCIS_MULTIMEDIA_VIDEO);
	pci_set_cfgdata16(dev, PCIR_SUBDEV_0, VIRTIO_TYPE_CAMERA);
	if (is_winvm == true)
		pci_set_cfgdata16(dev, PCIR_SUBVEND_0, ORACLE_VENDOR_ID);
	else
		pci_set_cfgdata16(dev, PCIR_SUBVEND_0, VIRTIO_VENDOR);
	pci_set_cfgdata16(dev, PCIR_REVID, 1);

	/* use BAR 1 to map MSI-X table and PBA, if we're using MSI-X */
	ret = virtio_interrupt_init(&vcamera->base, virtio_uses_msix());
	if (ret != 0)
		goto Error;

	ret = virtio_set_modern_bar(&vcamera->base, false);
	if (ret != 0)
		goto Error;

	return 0;

Error:
	if (vcamera)
		free(vcamera);
	return -1;
}

static void virtio_camera_req_stop(struct virtio_camera *vcamera, int index)
{
	void* jval;

	pthread_mutex_lock(&vcamera->vq_related[index].req_mutex);
	pthread_cond_broadcast(&vcamera->vq_related[index].req_cond);
	pthread_mutex_unlock(&vcamera->vq_related[index].req_mutex);
	pthread_join(vcamera->vcamera_tid[index], &jval);
}

static void virtio_camera_deinit(struct vmctx* ctx,struct pci_vdev* dev,char* opts)
{
	struct virtio_camera *vcamera;
	int index;

	if (dev->arg) {
		pr_err("virtio_camera_deinit\n");
		vcamera = (struct virtio_camera *)dev->arg;
			  vcamera->closing = 1;

		for (index = 0; index < VIRTIO_CAMERA_NUMQ; index++) {
			virtio_camera_close(index);
			virtio_camera_req_stop(vcamera, index);
			pthread_mutex_destroy(&vcamera->vq_related[index].req_mutex);
		}

		pthread_mutex_destroy(&vcamera->vcamera_mutex);
		free(vcamera);
		dev->arg = NULL;
	}
}

struct pci_vdev_ops pci_ops_virtio_camera = {
	.class_name = "virtio-camera",
	.vdev_init = virtio_camera_init,
	.vdev_deinit = virtio_camera_deinit,
	.vdev_barwrite = virtio_pci_write,
	.vdev_barread = virtio_pci_read
};
DEFINE_PCI_DEVTYPE(pci_ops_virtio_camera);
