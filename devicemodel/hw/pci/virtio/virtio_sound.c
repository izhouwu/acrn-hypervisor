/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

/*
 * virtio sound
 * audio mediator device model
 */

#include <err.h>
#include <errno.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sysexits.h>

#include <alsa/asoundlib.h>
#include <alsa/control.h>
#include <sys/queue.h>

#include "dm.h"
#include "pci_core.h"
#include "virtio.h"
#include "virtio_sound.h"
#include "log.h"

#define VIRTIO_SOUND_RINGSZ	256
#define VIRTIO_SOUND_VQ_NUM	4

/*
 * Host capabilities
 */
#define VIRTIO_SND_S_HOSTCAPS		(1UL << VIRTIO_F_VERSION_1)

#define VIRTIO_SOUND_CTL_SEGS	8
#define VIRTIO_SOUND_XFER_SEGS	4

#define VIRTIO_SOUND_CARD	4
#define VIRTIO_SOUND_STREAMS	4
#define VIRTIO_SOUND_CHMAPS	64

#define VIRTIO_SOUND_DEVICE_NAME	64

#define WPRINTF(format, arg...) pr_err(format, ##arg)

enum {
	VIRTIO_SND_BE_INITED = 1,
	VIRTIO_SND_BE_PRE,
	VIRTIO_SND_BE_START,
	VIRTIO_SND_BE_STOP,
	VIRTIO_SND_BE_RELEASE,
	VIRTIO_SND_BE_DEINITED,
};
struct virtio_sound_pcm_param {
	uint32_t features;
	uint64_t formats;
	uint64_t rates;

	uint8_t channels_min;
	uint8_t channels_max;

	uint32_t buffer_bytes;
	uint32_t period_bytes;
	uint8_t	channels;
	uint8_t	format;
	uint8_t rate;

	uint32_t rrate;
};

struct virtio_sound_msg_node {
	STAILQ_ENTRY(virtio_sound_msg_node) link;
	struct iovec *iov;
	struct virtio_vq_info *vq;
	int cnt;
	uint16_t idx;
};

struct virtio_sound_chmap {
	uint8_t channels;
	uint8_t positions[VIRTIO_SND_CHMAP_MAX_SIZE];
};

struct virtio_sound_pcm {
	snd_pcm_t *handle;
	int hda_fn_nid;
	int dir;
	int status;
	int xfer_iov_cnt;
	int id;

	pthread_t tid;
	struct pollfd *poll_fd;
	unsigned int pfd_count;

	char dev_name[VIRTIO_SOUND_DEVICE_NAME];
	struct virtio_sound_pcm_param param;
	STAILQ_HEAD(, virtio_sound_msg_node) head;
	pthread_mutex_t mtx;

	struct virtio_sound_chmap *chmaps[VIRTIO_SOUND_CHMAPS];
	uint32_t chmap_cnt;
};

/*dev struct*/
struct virtio_sound {
	struct virtio_base base;
	struct virtio_vq_info vq[VIRTIO_SOUND_VQ_NUM];
	pthread_mutex_t mtx;
	struct virtio_snd_config snd_cfg;
	uint64_t	features;

	struct virtio_sound_pcm *streams[VIRTIO_SOUND_STREAMS];
	int stream_cnt;
	int chmap_cnt;

	int max_tx_iov_cnt;
	int max_rx_iov_cnt;
	int status;
};

static int virtio_sound_cfgread(void *vdev, int offset, int size, uint32_t *retval);

static struct virtio_ops virtio_snd_ops = {
	"virtio_sound",		/* our name */
	VIRTIO_SOUND_VQ_NUM,	/* we support 4 virtqueue */
	sizeof(struct virtio_snd_config),			/* config reg size */
	NULL,	/* reset */
	NULL,	/* device-wide qnotify */
	virtio_sound_cfgread,				/* read virtio config */
	NULL,				/* write virtio config */
	NULL,				/* apply negotiated features */
	NULL,   /* called on guest set status */
};

/*
 * This array should be added as the same order
 * as enum of VIRTIO_SND_PCM_FMT_XXX.
 */
static const snd_pcm_format_t virtio_sound_v2s_format[] = {
	SND_PCM_FORMAT_IMA_ADPCM,
	SND_PCM_FORMAT_MU_LAW,
	SND_PCM_FORMAT_A_LAW,
	SND_PCM_FORMAT_S8,
	SND_PCM_FORMAT_U8,
	SND_PCM_FORMAT_S16_LE,
	SND_PCM_FORMAT_U16_LE,
	SND_PCM_FORMAT_S18_3LE,
	SND_PCM_FORMAT_U18_3LE,
	SND_PCM_FORMAT_S20_3LE,
	SND_PCM_FORMAT_U20_3LE,
	SND_PCM_FORMAT_S24_3LE,
	SND_PCM_FORMAT_U24_3LE,
	SND_PCM_FORMAT_S20_LE,
	SND_PCM_FORMAT_U20_LE,
	SND_PCM_FORMAT_S24_LE,
	SND_PCM_FORMAT_U24_LE,
	SND_PCM_FORMAT_S32_LE,
	SND_PCM_FORMAT_U32_LE,
	SND_PCM_FORMAT_FLOAT_LE,
	SND_PCM_FORMAT_FLOAT64_LE,
	SND_PCM_FORMAT_DSD_U8,
	SND_PCM_FORMAT_DSD_U16_LE,
	SND_PCM_FORMAT_DSD_U32_LE,
	SND_PCM_FORMAT_IEC958_SUBFRAME_LE
};

/*
 * This array should be added as the same order
 * as enum of VIRTIO_SND_PCM_RATE_XXX.
 */
static const uint32_t virtio_sound_t_rate[] = {
	5512, 8000, 11025, 16000, 22050, 32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000
};

static const uint8_t virtio_sound_s2v_chmap[] = {
	VIRTIO_SND_CHMAP_NONE,
	VIRTIO_SND_CHMAP_NA,
	VIRTIO_SND_CHMAP_MONO,
	VIRTIO_SND_CHMAP_FL,
	VIRTIO_SND_CHMAP_FR,
	VIRTIO_SND_CHMAP_RL,
	VIRTIO_SND_CHMAP_RR,
	VIRTIO_SND_CHMAP_FC,
	VIRTIO_SND_CHMAP_LFE,
	VIRTIO_SND_CHMAP_SL,
	VIRTIO_SND_CHMAP_SR,
	VIRTIO_SND_CHMAP_RC,
	VIRTIO_SND_CHMAP_FLC,
	VIRTIO_SND_CHMAP_FRC,
	VIRTIO_SND_CHMAP_RLC,
	VIRTIO_SND_CHMAP_RRC,
	VIRTIO_SND_CHMAP_FLW,
	VIRTIO_SND_CHMAP_FRW,
	VIRTIO_SND_CHMAP_FLH,
	VIRTIO_SND_CHMAP_FCH,
	VIRTIO_SND_CHMAP_FRH,
	VIRTIO_SND_CHMAP_TC,
	VIRTIO_SND_CHMAP_TFL,
	VIRTIO_SND_CHMAP_TFR,
	VIRTIO_SND_CHMAP_TFC,
	VIRTIO_SND_CHMAP_TRL,
	VIRTIO_SND_CHMAP_TRR,
	VIRTIO_SND_CHMAP_TRC,
	VIRTIO_SND_CHMAP_TFLC,
	VIRTIO_SND_CHMAP_TFRC,
	VIRTIO_SND_CHMAP_TSL,
	VIRTIO_SND_CHMAP_TSR,
	VIRTIO_SND_CHMAP_LLFE,
	VIRTIO_SND_CHMAP_RLFE,
	VIRTIO_SND_CHMAP_BC,
	VIRTIO_SND_CHMAP_BLC,
	VIRTIO_SND_CHMAP_BRC
};

static inline int
virtio_sound_get_frame_size(struct virtio_sound_pcm *stream)
{
	return snd_pcm_format_physical_width(virtio_sound_v2s_format[stream->param.format]) / 8
		* stream->param.channels;
}

static int
virtio_sound_cfgread(void *vdev, int offset, int size, uint32_t *retval)
{
	struct virtio_sound *virt_snd = vdev;
	void* ptr;
	ptr = (uint8_t *)&virt_snd->snd_cfg + offset;
	memcpy(retval, ptr, size);
	return 0;
}

static void
virtio_sound_notify_xfer(struct virtio_sound *virt_snd, struct virtio_vq_info *vq, int iov_cnt)
{
	struct virtio_sound_msg_node *msg_node;
	struct virtio_snd_pcm_xfer *xfer_hdr;
	int n, s;

	while (vq_has_descs(vq)) {
		msg_node = malloc(sizeof(struct virtio_sound_msg_node));
		if (msg_node == NULL) {
			WPRINTF("%s: malloc data node fail!\n", __func__);
			return;
		}
		msg_node->iov = malloc(sizeof(struct iovec) * iov_cnt);
		if (msg_node == NULL) {
			WPRINTF("%s: malloc data node fail!\n", __func__);
			return;
		}
		n = vq_getchain(vq, &msg_node->idx, msg_node->iov, iov_cnt, NULL);
		if (n <= 0) {
			WPRINTF("%s: fail to getchain!\n", __func__);
			free(msg_node);
			return;
		}
		msg_node->cnt = n;
		msg_node->vq = vq;

		xfer_hdr = (struct virtio_snd_pcm_xfer *)msg_node->iov[0].iov_base;
		s = xfer_hdr->stream_id;

		pthread_mutex_lock(&virt_snd->streams[s]->mtx);
		if (STAILQ_EMPTY(&virt_snd->streams[s]->head)) {
			STAILQ_INSERT_HEAD(&virt_snd->streams[s]->head, msg_node, link);
		} else {
			STAILQ_INSERT_TAIL(&virt_snd->streams[s]->head, msg_node, link);
		}
		pthread_mutex_unlock(&virt_snd->streams[s]->mtx);
	}
}

static void
virtio_sound_notify_tx(void *vdev, struct virtio_vq_info *vq)
{
	struct virtio_sound *virt_snd = vdev;
	virtio_sound_notify_xfer(virt_snd, vq, virt_snd->max_tx_iov_cnt);
}

static void
virtio_sound_notify_rx(void *vdev, struct virtio_vq_info *vq)
{
	struct virtio_sound *virt_snd = vdev;
	virtio_sound_notify_xfer(virt_snd, vq, virt_snd->max_rx_iov_cnt);
}

static int
virtio_sound_set_hwparam(struct virtio_sound_pcm *stream)
{
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_uframes_t buffer_size, period_size;
	int dir = stream->dir;
	int err;

	snd_pcm_hw_params_alloca(&hwparams);
	err = snd_pcm_hw_params_any(stream->handle, hwparams);
	if(err < 0) {
		WPRINTF("%s: no configurations available, error number %d!\n", __func__, err);
		return -1;
	}
	err = snd_pcm_hw_params_set_access(stream->handle, hwparams, SND_PCM_ACCESS_MMAP_INTERLEAVED);
	if(err < 0) {
		WPRINTF("%s: set access, error number %d!\n", __func__, err);
		return -1;
	}
	err = snd_pcm_hw_params_set_format(stream->handle, hwparams,
		virtio_sound_v2s_format[stream->param.format]);
	if(err < 0) {
		WPRINTF("%s: set format(%d), error number %d!\n", __func__,
			virtio_sound_v2s_format[stream->param.format], err);
		return -1;
	}
	err = snd_pcm_hw_params_set_channels(stream->handle,
		hwparams, stream->param.channels);
	if(err < 0) {
		WPRINTF("%s: set channels(%d) fail, error number %d!\n", __func__,
			stream->param.channels, err);
		return -1;
	}
	stream->param.rrate = virtio_sound_t_rate[stream->param.rate];
	err = snd_pcm_hw_params_set_rate_near(stream->handle, hwparams, &stream->param.rrate, &dir);
	if(err < 0) {
		WPRINTF("%s: set rate(%u) fail, error number %d!\n", __func__,
			virtio_sound_t_rate[stream->param.rate], err);
		return -1;
	}
	buffer_size = stream->param.buffer_bytes / virtio_sound_get_frame_size(stream);
	err = snd_pcm_hw_params_set_buffer_size(stream->handle, hwparams, buffer_size);
	if(err < 0) {
		WPRINTF("%s: set buffer_size(%ld) fail, error number %d!\n", __func__, buffer_size, err);
		return -1;
	}
	period_size = stream->param.period_bytes / virtio_sound_get_frame_size(stream);
	dir = stream->dir;
	err = snd_pcm_hw_params_set_period_size_near(stream->handle, hwparams,
		&period_size, &dir);
	if(err < 0) {
		WPRINTF("%s: set period_size(%ld) fail, error number %d!\n", __func__, period_size, err);
		return -1;
	}
	err = snd_pcm_hw_params(stream->handle, hwparams);
	if (err < 0) {
		WPRINTF("%s: set hw params fail, error number %d!\n", __func__, err);
		return -1;
	}
	return 0;
}


static int
virtio_sound_set_swparam(struct virtio_sound_pcm *stream)
{
	snd_pcm_sw_params_t *swparams;
	snd_pcm_format_t period_size;
	int err;

	snd_pcm_sw_params_alloca(&swparams);
	err = snd_pcm_sw_params_current(stream->handle, swparams);
	if(err < 0) {
		WPRINTF("%s: no sw params available, error number %d!\n", __func__, err);
		return -1;
	}

	err = snd_pcm_sw_params_set_start_threshold(stream->handle, swparams, 1);
	if(err < 0) {
		WPRINTF("%s: set threshold fail, error number %d!\n", __func__, err);
		return -1;
	}
	period_size = stream->param.period_bytes / virtio_sound_get_frame_size(stream);
	err = snd_pcm_sw_params_set_avail_min(stream->handle, swparams, period_size);
	if(err < 0) {
		WPRINTF("%s: set avail min fail, error number %d!\n", __func__, err);
		return -1;
	}
	err = snd_pcm_sw_params_set_period_event(stream->handle, swparams, 1);
	if(err < 0) {
		WPRINTF("%s: set period event fail, error number %d!\n", __func__, err);
		return -1;
	}
	err = snd_pcm_sw_params(stream->handle, swparams);
	if (err < 0) {
		WPRINTF("%s: set sw params fail, error number %d!\n", __func__, err);
		return -1;
	}
	return 0;
}

static int
virtio_sound_recover(struct virtio_sound_pcm *stream)
{
	snd_pcm_state_t state = snd_pcm_state(stream->handle);
	int err = -1, i;

	if (state == SND_PCM_STATE_XRUN || state == SND_PCM_STATE_SETUP) {
		err = snd_pcm_prepare(stream->handle);
		if(err < 0) {
			WPRINTF("%s: recorver from xrun prepare fail, error number %d!\n", __func__, err);
			return -1;
		}
		err = snd_pcm_start(stream->handle);
		if(err < 0) {
			WPRINTF("%s: recorver from xrun start fail, error number %d!\n", __func__, err);
			return -1;
		}
	} else if (state == SND_PCM_STATE_SUSPENDED) {
		for (i = 0; i < 10; i++) {
			err = snd_pcm_resume(stream->handle);
			if (err == -EAGAIN) {
				WPRINTF("%s: waiting for resume!\n", __func__);
				usleep(5000);
				continue;
			}
			err = snd_pcm_prepare(stream->handle);
			if(err < 0) {
				WPRINTF("%s: recorver form suspend prepare fail, error number %d!\n", __func__, err);
				return -1;
			}
			err = snd_pcm_start(stream->handle);
			if(err < 0) {
				WPRINTF("%s: recorver from suspend start fail, error number %d!\n", __func__, err);
				return -1;
			}
			break;
		}
	}
	return err;
}

static int
virtio_sound_xfer(struct virtio_sound_pcm *stream)
{
	const snd_pcm_channel_area_t *pcm_areas;
	struct virtio_sound_msg_node *msg_node;
	struct virtio_snd_pcm_status *ret_status;
	snd_pcm_sframes_t avail, xfer = 0;
	snd_pcm_uframes_t pcm_offset, frames;
	void * buf;
	int err, i, frame_size, to_copy, len = 0;

	avail = snd_pcm_avail_update(stream->handle);
	if (avail < 0) {
		err = virtio_sound_recover(stream);
		if (err < 0) {
			WPRINTF("%s: recorver form suspend prepare fail, error number %d!\n", __func__, err);
			return -1;
		}
	}
	frame_size = virtio_sound_get_frame_size(stream);
	frames = stream->param.period_bytes / frame_size;
	/*
	 * For frontend send buffer address period by period, backend copy a period
	 * data in one time.
	 */
	if (avail < frames || (msg_node = STAILQ_FIRST(&stream->head)) == NULL) {
		return 0;
	}
	err = snd_pcm_mmap_begin(stream->handle, &pcm_areas, &pcm_offset, &frames);
	if (err < 0) {
		err = virtio_sound_recover(stream);
		if (err < 0) {
			WPRINTF("%s: mmap begin fail, error number %d!\n", __func__, err);
			return -1;
		}
	}
	/*
	 * 'pcm_areas' is an array which contains num_of_channels elements in it.
	 * For interleaved, all elements in the array has the same addr but different offset ("first" in the structure).
	 */
	buf = pcm_areas[0].addr + pcm_offset * frame_size;
	for (i = 1; i < msg_node->cnt - 1; i++) {
		to_copy = msg_node->iov[i].iov_len;
		/*
		 * memcpy can only be used when SNDRV_PCM_INFO_INTERLEAVED.
		 */
		if (stream->dir == SND_PCM_STREAM_PLAYBACK) {
			memcpy(buf, msg_node->iov[i].iov_base, to_copy);
		} else {
			memcpy(msg_node->iov[i].iov_base, buf, to_copy);
			len += msg_node->iov[i].iov_len;
		}
		xfer += to_copy / frame_size;
		buf += to_copy;
	}
	if (xfer != frames) {
		WPRINTF("%s: write fail, xfer %ld, frame %ld!\n", __func__, xfer, frames);
		return -1;
	}
	xfer = snd_pcm_mmap_commit(stream->handle, pcm_offset, frames);
	if(xfer < 0 || xfer != frames) {
		WPRINTF("%s: mmap commit fail, xfer %ld!\n", __func__, xfer);
		return -1;
	}
	ret_status = (struct virtio_snd_pcm_status *)msg_node->iov[msg_node->cnt - 1].iov_base;
	ret_status->status = VIRTIO_SND_S_OK;
	vq_relchain(msg_node->vq, msg_node->idx, len + sizeof(struct virtio_snd_pcm_status));
	vq_endchains(msg_node->vq, 0);
	pthread_mutex_lock(&stream->mtx);
	STAILQ_REMOVE_HEAD(&stream->head, link);
	pthread_mutex_unlock(&stream->mtx);
	free(msg_node->iov);
	free(msg_node);
	return xfer;
}

static void
virtio_sound_clean_vq(struct virtio_sound_pcm *stream) {
	struct virtio_sound_msg_node *msg_node;
	struct virtio_vq_info *vq;
	struct virtio_snd_pcm_status *ret_status;

	while ((msg_node = STAILQ_FIRST(&stream->head)) != NULL) {
		vq = msg_node->vq;
		ret_status = (struct virtio_snd_pcm_status *)msg_node->iov[msg_node->cnt - 1].iov_base;
		ret_status->status = VIRTIO_SND_S_BAD_MSG;
		vq_relchain(vq, msg_node->idx, sizeof(struct virtio_snd_pcm_status));

		pthread_mutex_lock(&stream->mtx);
		STAILQ_REMOVE_HEAD(&stream->head, link);
		pthread_mutex_unlock(&stream->mtx);
		free(msg_node->iov);
		free(msg_node);
	}

	if (vq)
		vq_endchains(vq, 0);
}

static void*
virtio_sound_pcm_thread(void *param)
{
	unsigned short revents;
	struct virtio_sound_pcm *stream = (struct virtio_sound_pcm*)param;
	int err;

	do {
		poll(stream->poll_fd, stream->pfd_count, -1);
		snd_pcm_poll_descriptors_revents(stream->handle, stream->poll_fd,
			stream->pfd_count, &revents);
		if (revents & POLLOUT || revents & POLLIN) {
			err = virtio_sound_xfer(stream);
			if (err < 0) {
				WPRINTF("%s: stream error!\n", __func__);
				break;
			}
		} else {
			err = virtio_sound_recover(stream);
			if (err < 0) {
				WPRINTF("%s: poll error %d!\n", __func__, (int)snd_pcm_state(stream->handle));
				break;
			}
		}
		if (stream->status == VIRTIO_SND_BE_STOP) {
			usleep(100);
			continue;
		}
	} while (stream->status == VIRTIO_SND_BE_START || stream->status == VIRTIO_SND_BE_STOP);

	if (stream->status == VIRTIO_SND_BE_RELEASE && !STAILQ_EMPTY(&stream->head)) {
		virtio_sound_clean_vq(stream);
	}

	if(stream->handle) {
		if (snd_pcm_close(stream->handle) < 0) {
			WPRINTF("%s: stream %s close error!\n", __func__, stream->dev_name);
		}
		stream->handle = NULL;
	}
	free(stream->poll_fd);
	stream->poll_fd = NULL;
	stream->status = VIRTIO_SND_BE_INITED;
	pthread_exit(NULL);
}

static int
virtio_sound_create_pcm_thread(struct virtio_sound_pcm *stream)
{
	int err;
	pthread_attr_t attr;

	stream->pfd_count = snd_pcm_poll_descriptors_count(stream->handle);
	stream->poll_fd = malloc(sizeof(struct pollfd) * stream->pfd_count);
	if (stream->poll_fd == NULL) {
		WPRINTF("%s: malloc poll fd fail\n", __func__);
		return -1;
	}
	if ((err = snd_pcm_poll_descriptors(stream->handle,
		stream->poll_fd, stream->pfd_count)) <= 0) {
		WPRINTF("%s: get poll descriptor fail, error number %d!\n", __func__, err);
		return -1;
	}
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&stream->tid, &attr, virtio_sound_pcm_thread, (void *)stream);
	return 0;
}

static void virtio_sound_update_iov_cnt(struct virtio_sound *virt_snd, int dir) {
	int i, cnt = 0;

	for (i = 0; i < virt_snd->stream_cnt; i++) {
		if (virt_snd->streams[i]->dir == dir && virt_snd->streams[i]->status != VIRTIO_SND_BE_INITED
			&& cnt < virt_snd->streams[i]->xfer_iov_cnt) {
			cnt = virt_snd->streams[i]->xfer_iov_cnt;
		}
	}
	if (dir == SND_PCM_STREAM_PLAYBACK)
		virt_snd->max_tx_iov_cnt = cnt;
	else
		virt_snd->max_rx_iov_cnt = cnt;
}

static int
virtio_sound_r_pcm_info(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	int i, ret_len;
	struct virtio_snd_query_info *info = (struct virtio_snd_query_info *)iov[0].iov_base;
	struct virtio_snd_pcm_info *pcm_info = iov[2].iov_base;
	struct virtio_sound_pcm *stream;
	struct virtio_snd_hdr *ret = iov[1].iov_base;

	if (n != 3) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if ((info->start_id + info->count) > virt_snd->stream_cnt) {
		WPRINTF("%s: invalid stream, start %d, count = %d!\n", __func__,
			info->start_id, info->count);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	ret_len = info->count * sizeof(struct virtio_snd_pcm_info);
	if (ret_len > iov[2].iov_len) {
		WPRINTF("%s: too small buffer %d, required %d!\n", __func__,
			iov[2].iov_len, ret_len);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	for (i = 0; i < info->count; i++) {
		stream = virt_snd->streams[info->start_id + i];
		if (stream == NULL) {
			WPRINTF("%s: invalid stream, start %d, count = %d!\n", __func__,
				info->start_id, info->count);
			ret->code = VIRTIO_SND_S_BAD_MSG;
			return (int)iov[1].iov_len;
		}
		pcm_info[i].hdr.hda_fn_nid = stream->hda_fn_nid;
		pcm_info[i].features = stream->param.features;
		pcm_info[i].formats = stream->param.formats;
		pcm_info[i].rates = stream->param.rates;
		pcm_info[i].direction = stream->dir;
		pcm_info[i].channels_min = stream->param.channels_min;
		pcm_info[i].channels_max = stream->param.channels_max;
		memset(pcm_info[i].padding, 0, sizeof(pcm_info[i].padding));
	}

	ret->code = VIRTIO_SND_S_OK;
	return ret_len + (int)iov[1].iov_len;
}

static int
virtio_sound_r_set_params(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_pcm_set_params *params = (struct virtio_snd_pcm_set_params *)iov[0].iov_base;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	struct virtio_sound_pcm *stream;
	int err;

	if (n != 2) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if (params->hdr.stream_id >= virt_snd->stream_cnt) {
		WPRINTF("%s: invalid stream %d!\n", __func__, params->hdr.stream_id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	stream = virt_snd->streams[params->hdr.stream_id];
	if (stream->status == VIRTIO_SND_BE_RELEASE) {
		WPRINTF("%s: stream %d is releasing!\n", __func__, stream->id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	if((stream->param.formats && (1 << params->format) == 0) ||
		(stream->param.rates && (1 << params->rate) == 0) ||
		(params->channels < stream->param.channels_min) ||
		(params->channels > stream->param.channels_max)) {
		WPRINTF("%s: invalid parameters sample format %d, frame rate %d, channels %d!\n", __func__,
		params->format, params->rate, params->channels);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	ret->code = VIRTIO_SND_S_OK;
	stream->param.buffer_bytes = params->buffer_bytes;
	stream->param.period_bytes = params->period_bytes;
	stream->param.features = params->features;
	stream->param.channels = params->channels;
	stream->param.format = params->format;
	stream->param.rate = params->rate;

	/*
	 * In extreme case, each data page is disconinuous and the start and end
	 * of data buffer is not 4k align. The total xfer_iov_cnt is
	 * period bytes / 4k + 2 (start and end) + 2 (xfer msg + status).
	 */
	stream->xfer_iov_cnt = stream->param.period_bytes / 4096 + VIRTIO_SOUND_XFER_SEGS;
	if (stream->dir == SND_PCM_STREAM_PLAYBACK) {
		if (stream->xfer_iov_cnt > virt_snd->max_tx_iov_cnt)
			virt_snd->max_tx_iov_cnt = stream->xfer_iov_cnt;
	} else {
		if (stream->xfer_iov_cnt > virt_snd->max_rx_iov_cnt)
			virt_snd->max_rx_iov_cnt = stream->xfer_iov_cnt;
	}

	if(!stream->handle)
		if (snd_pcm_open(&stream->handle, stream->dev_name,
			stream->dir, SND_PCM_NONBLOCK) < 0 || stream->handle == NULL) {
			WPRINTF("%s: stream %s open fail!\n", __func__, stream->dev_name);
			ret->code = VIRTIO_SND_S_BAD_MSG;
			return (int)iov[1].iov_len;
		}
	err = virtio_sound_set_hwparam(stream);
	if (err < 0) {
		WPRINTF("%s: set hw params fail!\n", __func__);
		ret->code = VIRTIO_SND_S_BAD_MSG;
	}
	err = virtio_sound_set_swparam(stream);
	if (err < 0) {
		WPRINTF("%s: set sw params fail!\n", __func__);
		ret->code = VIRTIO_SND_S_BAD_MSG;
	}

	return (int)iov[1].iov_len;
}

static int
virtio_sound_r_pcm_prepare(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_pcm_hdr *pcm = (struct virtio_snd_pcm_hdr *)iov[0].iov_base;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	int s;

	if (n != 2) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if ((s = pcm->stream_id) >= virt_snd->stream_cnt) {
		WPRINTF("%s: invalid stream %d!\n", __func__, s);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	if (virt_snd->streams[s]->status == VIRTIO_SND_BE_RELEASE) {
		WPRINTF("%s: stream %d is releasing!\n", __func__, s);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	ret->code = VIRTIO_SND_S_OK;
	if(!virt_snd->streams[s]->handle)
		if (snd_pcm_open(&virt_snd->streams[s]->handle, virt_snd->streams[s]->dev_name,
			virt_snd->streams[s]->dir, SND_PCM_NONBLOCK) < 0  || virt_snd->streams[s]->handle == NULL) {
			WPRINTF("%s: stream %s open fail!\n", __func__, virt_snd->streams[s]->dev_name);
			ret->code = VIRTIO_SND_S_BAD_MSG;
			return (int)iov[1].iov_len;
		}
	if (snd_pcm_prepare(virt_snd->streams[s]->handle) < 0) {
		WPRINTF("%s: stream %s prepare fail!\n", __func__, virt_snd->streams[s]->dev_name);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	virt_snd->streams[s]->status = VIRTIO_SND_BE_PRE;
	return (int)iov[1].iov_len;
}

static int
virtio_sound_r_pcm_release(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	int s;
	struct virtio_snd_pcm_hdr *pcm = (struct virtio_snd_pcm_hdr *)iov[0].iov_base;
	struct virtio_snd_hdr *ret = iov[1].iov_base;

	if (n != 2) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if ((s = pcm->stream_id) >= VIRTIO_SOUND_STREAMS) {
		WPRINTF("%s: invalid stream %d!\n", __func__, s);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	virt_snd->streams[s]->status = VIRTIO_SND_BE_RELEASE;
	ret->code = VIRTIO_SND_S_OK;
	virtio_sound_update_iov_cnt(virt_snd, virt_snd->streams[s]->dir);

	return (int)iov[1].iov_len;
}

static int
virtio_sound_r_pcm_start(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_pcm_hdr *pcm = (struct virtio_snd_pcm_hdr *)iov[0].iov_base;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	struct virtio_sound_pcm *stream;
	int i;

	if (n != 2) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if (pcm->stream_id >= VIRTIO_SOUND_STREAMS) {
		WPRINTF("%s: invalid stream %d!\n", __func__, pcm->stream_id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	ret->code = VIRTIO_SND_S_OK;
	stream = virt_snd->streams[pcm->stream_id];
	if (stream->status == VIRTIO_SND_BE_RELEASE) {
		WPRINTF("%s: stream %d is releasing!\n", __func__, stream->id);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	if (stream->dir == SND_PCM_STREAM_PLAYBACK) {
		/*
		 * For start threshold is 1, send 2 period before start.
		 * Less start periods benifit the frontend hw_ptr updating.
		 * After 1 period finished, xfer thread will full fill the buffer.
		 * Send 2 periods here to avoid the empty buffer which may cause
		 * pops and clicks.
		 */
		for (i = 0; i < 2; i++) {
			if (virtio_sound_xfer(stream) < 0) {
				WPRINTF("%s: stream fn_id %d xfer error!\n", __func__, stream->hda_fn_nid);
				ret->code = VIRTIO_SND_S_BAD_MSG;
				return (int)iov[1].iov_len;
			}
		}
	}
	stream->status = VIRTIO_SND_BE_START;
	if (virtio_sound_create_pcm_thread(stream) < 0) {
		WPRINTF("%s: create thread fail!\n", __func__);
		ret->code = VIRTIO_SND_S_BAD_MSG;
	}
	if (snd_pcm_start(stream->handle) < 0) {
		WPRINTF("%s: stream %s start error!\n", __func__, stream->dev_name);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	return (int)iov[1].iov_len;
}

static int
virtio_sound_r_pcm_stop(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_pcm_hdr *pcm = (struct virtio_snd_pcm_hdr *)iov[0].iov_base;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	int s;

	if (n != 2) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}

	if ((s = pcm->stream_id) >= VIRTIO_SOUND_STREAMS) {
		WPRINTF("%s: invalid stream %d!\n", __func__, s);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}
	if (snd_pcm_drop(virt_snd->streams[s]->handle) < 0) {
		WPRINTF("%s: stream %s drop error!\n", __func__, virt_snd->streams[s]->dev_name);
	}
	virt_snd->streams[s]->status = VIRTIO_SND_BE_STOP;

	ret->code = VIRTIO_SND_S_OK;
	return (int)iov[1].iov_len;
}

static int
virtio_sound_r_chmap_info(struct virtio_sound *virt_snd, struct iovec *iov, uint8_t n)
{
	struct virtio_snd_query_info *info = (struct virtio_snd_query_info *)iov[0].iov_base;
	struct virtio_snd_chmap_info *chmap_info = iov[2].iov_base;
	struct virtio_sound_pcm *stream = virt_snd->streams[0];
	struct virtio_sound_chmap *chmap;
	struct virtio_snd_hdr *ret = iov[1].iov_base;
	int s, c = 0, i = 0, ret_len;

	if (n != 3) {
		WPRINTF("%s: invalid seg num %d!\n", __func__, n);
		return 0;
	}
	if ((info->start_id + info->count) > virt_snd->chmap_cnt) {
		WPRINTF("%s: invalid chmap, start %d, count = %d!\n", __func__,
			info->start_id, info->count);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	ret_len = info->count * sizeof(struct virtio_snd_chmap_info);
	if (ret_len > iov[2].iov_len) {
		WPRINTF("%s: too small buffer %d, required %d!\n", __func__,
			iov[2].iov_len, ret_len);
		ret->code = VIRTIO_SND_S_BAD_MSG;
		return (int)iov[1].iov_len;
	}

	for(s = 0; s < virt_snd->stream_cnt; s++) {
		if (info->start_id >= i && info->start_id < i + virt_snd->streams[s]->chmap_cnt) {
			c = info->start_id - i;
			stream = virt_snd->streams[s];
			break;
		}
		i += virt_snd->streams[s]->chmap_cnt;
	}

	for (i = 0; i < info->count; i++) {
		chmap = stream->chmaps[c];
		chmap_info[i].hdr.hda_fn_nid = stream->hda_fn_nid;
		chmap_info[i].direction = stream->dir;
		chmap_info[i].channels = chmap->channels;
		memcpy(chmap_info[i].positions, chmap->positions, VIRTIO_SND_CHMAP_MAX_SIZE);

		if (++c >= stream->chmap_cnt) {
			if(++s >= virt_snd->stream_cnt)
				break;
			stream = virt_snd->streams[s];
			c = 0;
		}
	}

	ret->code = VIRTIO_SND_S_OK;
	return ret_len + (int)iov[1].iov_len;
}

static void
virtio_sound_notify_ctl(void *vdev, struct virtio_vq_info *vq)
{
	struct virtio_sound *virt_snd = vdev;
	struct virtio_snd_query_info *info;
	struct iovec iov[VIRTIO_SOUND_CTL_SEGS];
	int n, ret_len = 0;
	uint16_t idx;

	while (vq_has_descs(vq)) {
		n = vq_getchain(vq, &idx, iov, VIRTIO_SOUND_CTL_SEGS, NULL);
		if (n <= 0) {
			WPRINTF("%s: fail to getchain!\n", __func__);
			return;
		}

		info = (struct virtio_snd_query_info *)iov[0].iov_base;
		switch (info->hdr.code) {
			case VIRTIO_SND_R_PCM_INFO:
				ret_len = virtio_sound_r_pcm_info(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_PCM_SET_PARAMS:
				ret_len = virtio_sound_r_set_params(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_PCM_PREPARE:
				ret_len = virtio_sound_r_pcm_prepare(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_PCM_RELEASE:
				ret_len = virtio_sound_r_pcm_release(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_PCM_START:
				ret_len = virtio_sound_r_pcm_start(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_PCM_STOP:
				ret_len = virtio_sound_r_pcm_stop(virt_snd, iov, n);
				break;
			case VIRTIO_SND_R_CHMAP_INFO:
				ret_len = virtio_sound_r_chmap_info(virt_snd, iov, n);
				break;
			default:
				WPRINTF("%s: unsupported request 0x%X!\n", __func__, n);
				break;
		}

		vq_relchain(vq, idx, ret_len);
	}
	vq_endchains(vq, 1);
}

static void
virtio_sound_notify_event(void *vdev, struct virtio_vq_info *vq)
{
}

/*init*/
static void
virtio_sound_cfg_init(struct virtio_sound *virt_snd)
{
	virt_snd->snd_cfg.streams = virt_snd->stream_cnt;
	virt_snd->snd_cfg.jacks = 0;
	virt_snd->snd_cfg.chmaps = virt_snd->chmap_cnt;
	virt_snd->snd_cfg.controls = 0;
}

static bool
virtio_sound_format_support(snd_pcm_t *handle, uint32_t format)
{
	snd_pcm_hw_params_t *hwparams;

	snd_pcm_hw_params_alloca(&hwparams);
	if(snd_pcm_hw_params_any(handle, hwparams) < 0) {
		WPRINTF("%s: no configurations available!\n", __func__);
		return false;
	}
	return (snd_pcm_hw_params_test_format(handle, hwparams, format) == 0);
}

static bool
virtio_sound_rate_support(snd_pcm_t *handle, uint32_t rate, int dir)
{
	snd_pcm_hw_params_t *hwparams;
	uint32_t rrate;

	snd_pcm_hw_params_alloca(&hwparams);
	if(snd_pcm_hw_params_any(handle, hwparams) < 0) {
		WPRINTF("%s: no configurations available!\n", __func__);
		return false;
	}
	rrate = rate;
	return (snd_pcm_hw_params_set_rate_near(handle, hwparams, &rrate, &dir) == 0
		&& rrate == rate);
}

static int
virtio_sound_pcm_param_init(struct virtio_sound_pcm *stream, int dir, char *name, int fn_id)
{
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_chmap_query_t **chmaps;
	uint32_t channels_min, channels_max, i, j;

	stream->dir = dir;
	strncpy(stream->dev_name, name, VIRTIO_SOUND_DEVICE_NAME);
	stream->hda_fn_nid = fn_id;

	if (snd_pcm_open(&stream->handle, stream->dev_name,
		stream->dir, SND_PCM_NONBLOCK) < 0 || stream->handle == NULL) {
		WPRINTF("%s: stream %s open fail!\n", __func__, stream->dev_name);
		return -1;
	}

	for (i = 0; i < ARRAY_SIZE(virtio_sound_v2s_format); i++) {
		if (virtio_sound_format_support(stream->handle, virtio_sound_v2s_format[i]))
			stream->param.formats |= (1 << i);
	}
	for (i = 0; i < ARRAY_SIZE(virtio_sound_t_rate); i++) {
		if(virtio_sound_rate_support(stream->handle, virtio_sound_t_rate[i], dir))
			stream->param.rates |= (1 << i);
	}
	if (stream->param.rates == 0 || stream->param.formats == 0) {
		WPRINTF("%s: get param fail rates 0x%lx formats 0x%lx!\n", __func__,
			stream->param.rates, stream->param.formats);
		return -1;
	}
	stream->param.features = (1 << VIRTIO_SND_PCM_F_EVT_XRUNS);
	snd_pcm_hw_params_alloca(&hwparams);
	if(snd_pcm_hw_params_any(stream->handle, hwparams) < 0) {
		WPRINTF("%s: no configurations available!\n", __func__);
		return -1;
	}
	if (snd_pcm_hw_params_get_channels_min(hwparams, &channels_min) < 0 ||
		snd_pcm_hw_params_get_channels_max(hwparams, &channels_max) < 0) {
			WPRINTF("%s: get channel info fail!\n", __func__);
			return -1;
	}
	stream->param.channels_min = channels_min;
	stream->param.channels_max = channels_max;

	i = 0;
	chmaps = snd_pcm_query_chmaps(stream->handle);
	while (chmaps != NULL && chmaps[i] != NULL) {
		stream->chmaps[i] = malloc(sizeof(struct virtio_sound_chmap));
		if (stream->chmaps[i] == NULL) {
			WPRINTF("%s: malloc chmap buffer fail!\n", __func__);
			return -1;
		}
		stream->chmaps[i]->channels = chmaps[i]->map.channels;
		for (j = 0; j < chmaps[i]->map.channels; j++)
			stream->chmaps[i]->positions[j] = virtio_sound_s2v_chmap[chmaps[i]->map.pos[j]];
		stream->chmap_cnt++;
		i++;
	}
	snd_pcm_free_chmaps(chmaps);
	STAILQ_INIT(&stream->head);

	if (snd_pcm_close(stream->handle) < 0) {
		WPRINTF("%s: stream %s close error!\n", __func__, stream->dev_name);
		return -1;
	}
	stream->handle = NULL;

	return 0;
}

static int
virtio_sound_pcm_init(struct virtio_sound *virt_snd, char *device, char *hda_fn_nid, int dir)
{
	struct virtio_sound_pcm *stream;
	int err;

	if (virt_snd->stream_cnt >= VIRTIO_SOUND_STREAMS) {
		WPRINTF("%s: too many audio streams(%d)!\n", __func__, VIRTIO_SOUND_VQ_NUM);
		return -1;
	}
	stream = malloc(sizeof(struct virtio_sound_pcm));
	if (stream == NULL) {
		WPRINTF("%s: malloc data node fail!\n", __func__);
		return -1;
	}
	memset(stream, 0, sizeof(struct virtio_sound_pcm));

	stream->id = virt_snd->stream_cnt;
	strncpy(stream->dev_name, device, VIRTIO_SOUND_DEVICE_NAME);
	stream->hda_fn_nid = atoi(hda_fn_nid);
	if (virtio_sound_pcm_param_init(stream, dir, stream->dev_name, stream->hda_fn_nid) == 0) {
		virt_snd->streams[virt_snd->stream_cnt] = stream;
		virt_snd->stream_cnt++;
		virt_snd->chmap_cnt += stream->chmap_cnt;
	} else {
		WPRINTF("%s: stream %s close error!\n", __func__, stream->dev_name);
		free(stream);
		return -1;
	}
	err = pthread_mutex_init(&stream->mtx, NULL);
	if (err) {
		WPRINTF("%s: mutex init failed with error %d!\n", __func__, err);
		free(stream);
		return -1;
	}
	return 0;
}

static int
virtio_sound_parse_opts(struct virtio_sound *virt_snd, char *opts)
{
	char *str, *type, *cpy, *c, *param, *device;

	/*
	 * Virtio sound command line should be:
	 * '-s n virtio-sound,...'.
	 * Playback substreams can be added by
	 * 	'pcmp=pcm1_name_str@hda_fn_nid[|pcm2_name_str@hda_fn_nid]'.
	 * Capture substreams can be added by
	 * 	'pcmc=pcm1_name_str@hda_fn_nid[|pcm2_name_str@hda_fn_nid]'.
	 * Kcontrol element can be added by
	 * 	'ctl=kctl1_identifer@card_name[|kctl2_identifer@card_name].
	 * The 'kctl_identifer' should be got by
	 * 	'amixer controls' such as
	 * 	'numid=99,iface=MIXER,name='PCM Playback Volume'.
	 * Substreams and kcontrols should be seperated by '&', as
	 * 	'-s n virtio-sound,pcmp=...&pcmc=...&ctl=...'.
	 */
	c = cpy = strdup(opts);
	while ((str = strsep(&cpy, "&")) != NULL) {
		type = strsep(&str, "=");
		if (strstr("pcmp", type)) {
			while ((param = strsep(&str, "|")) != NULL) {
				device = strsep(&param, "@");
				if (virtio_sound_pcm_init(virt_snd, device, param, VIRTIO_SND_D_OUTPUT) < 0) {
					WPRINTF("%s: fail to init pcm stream %s!\n", __func__, param);
					free(c);
					return -1;
				}
			}
		} else if (strstr("pcmc", type)) {
			while ((param = strsep(&str, "|")) != NULL) {
				device = strsep(&param, "@");
				if (virtio_sound_pcm_init(virt_snd, device, param, VIRTIO_SND_D_INPUT) < 0) {
					WPRINTF("%s: fail to init pcm stream %s!\n", __func__, param);
					free(c);
					return -1;
				}
			}
		} else {
			WPRINTF("%s: unknow type %s!\n", __func__, type);
			free(c);
			return -1;
		}
	}
	free(c);
	return 0;
}

static int
virtio_sound_init(struct vmctx *ctx, struct pci_vdev *dev, char *opts)
{
	struct virtio_sound *virt_snd;
	pthread_mutexattr_t attr;
	int err;

	virt_snd = calloc(1, sizeof(struct virtio_sound));
	if (!virt_snd) {
		WPRINTF(("virtio_sound: calloc returns NULL\n"));
		return -1;
	}
	err = pthread_mutexattr_init(&attr);
	if (err) {
		WPRINTF("%s: mutexattr init failed with erro %d!\n", __func__, err);
		free(virt_snd);
		return -1;
	}
	err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	if (err) {
		WPRINTF("%s: mutexattr_settype failed with error %d.\n",
			       __func__, err);
		free(virt_snd);
		return -1;
	}
	err = pthread_mutex_init(&virt_snd->mtx, &attr);
	if (err) {
		WPRINTF("mutex init failed with error %d!\n", err);
		free(virt_snd);
		return -1;
	}

	virtio_linkup(&virt_snd->base,
		      &virtio_snd_ops,
		      virt_snd,
		      dev,
		      virt_snd->vq,
		      BACKEND_VBSU);

	virt_snd->base.mtx = &virt_snd->mtx;
	virt_snd->base.device_caps = VIRTIO_SND_S_HOSTCAPS;

	virt_snd->vq[0].qsize = VIRTIO_SOUND_RINGSZ;
	virt_snd->vq[1].qsize = VIRTIO_SOUND_RINGSZ;
	virt_snd->vq[2].qsize = VIRTIO_SOUND_RINGSZ;
	virt_snd->vq[3].qsize = VIRTIO_SOUND_RINGSZ;
	virt_snd->vq[0].notify = virtio_sound_notify_ctl;
	virt_snd->vq[1].notify = virtio_sound_notify_event;
	virt_snd->vq[2].notify = virtio_sound_notify_tx;
	virt_snd->vq[3].notify = virtio_sound_notify_rx;

	/* initialize config space */
	pci_set_cfgdata16(dev, PCIR_DEVICE, VIRTIO_TYPE_SOUND + 0x1040);
	pci_set_cfgdata16(dev, PCIR_VENDOR, VIRTIO_VENDOR);
	pci_set_cfgdata8(dev, PCIR_CLASS, PCIC_MULTIMEDIA);
	pci_set_cfgdata8(dev, PCIR_SUBCLASS, PCIS_MULTIMEDIA_AUDIO);
	pci_set_cfgdata16(dev, PCIR_SUBDEV_0, VIRTIO_TYPE_SOUND);
	if (is_winvm == true)
		pci_set_cfgdata16(dev, PCIR_SUBVEND_0, ORACLE_VENDOR_ID);
	else
		pci_set_cfgdata16(dev, PCIR_SUBVEND_0, VIRTIO_VENDOR);

	if (virtio_interrupt_init(&virt_snd->base, virtio_uses_msix())) {
		pthread_mutex_destroy(&virt_snd->mtx);
		free(virt_snd);
		return -1;
	}
	err = virtio_set_modern_bar(&virt_snd->base, false);
	if (err != 0) {
		pthread_mutex_destroy(&virt_snd->mtx);
		free(virt_snd);
		return err;
	}

	virt_snd->stream_cnt = 0;
	virt_snd->chmap_cnt = 0;

	err = virtio_sound_parse_opts(virt_snd, opts);
	if (err != 0) {
		pthread_mutex_destroy(&virt_snd->mtx);
		free(virt_snd);
		return err;
	}

	virtio_sound_cfg_init(virt_snd);
	virt_snd->status = VIRTIO_SND_BE_INITED;
	return 0;
}

static void
virtio_sound_deinit(struct vmctx *ctx, struct pci_vdev *dev, char *opts)
{
	struct virtio_sound *virt_snd = (struct virtio_sound *)dev->arg;
	int s, i;

	virt_snd->status = VIRTIO_SND_BE_DEINITED;
	for (s = 0; s < virt_snd->stream_cnt; s++) {
		if (virt_snd->streams[s]->handle && snd_pcm_close(virt_snd->streams[s]->handle) < 0) {
			WPRINTF("%s: stream %s close error!\n", __func__, virt_snd->streams[s]->dev_name);
		}
		pthread_mutex_destroy(&virt_snd->streams[s]->mtx);
		for (i = 0; i < virt_snd->streams[s]->chmap_cnt; i++) {
			free(virt_snd->streams[s]->chmaps[i]);
		}
		free(virt_snd->streams[s]);
	}
	pthread_mutex_destroy(&virt_snd->mtx);
	free(virt_snd);
}

struct pci_vdev_ops pci_ops_virtio_sound = {
	.class_name	= "virtio-sound",
	.vdev_init	= virtio_sound_init,
	.vdev_deinit	= virtio_sound_deinit,
	.vdev_barwrite	= virtio_pci_write,
	.vdev_barread	= virtio_pci_read
};

DEFINE_PCI_DEVTYPE(pci_ops_virtio_sound);
