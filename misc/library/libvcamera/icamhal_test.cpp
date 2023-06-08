/*
 * Copyright (C) 2019-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <linux/videodev2.h>
#include "ICamera.h"

int main(int argc,char** argv)
{
	int ret;
	int camera_id = 0;
	int buffer_count = 6;

	using namespace icamera;

	ret = camera_hal_init();
	printf("camera_test camera_hal_init ret = %d\n", ret);

	ret = camera_device_open(camera_id);
	printf("camera_test camera_device_open ret = %d\n", ret);

	stream_t input_config;
	memset(&input_config, 0, sizeof(stream_t));

	Parameters param;
	camera_get_parameters(camera_id, param);

	camera_ae_mode_t aeMode = AE_MODE_MANUAL;
	int64_t expTime = 20 * 1000;
	param.setAeMode(aeMode);
	param.setExposureTime(expTime);

	camera_set_parameters(camera_id, param);

	input_config.format = -1;

	ret = camera_device_config_sensor_input(camera_id, &input_config);
	printf("camera_test camera_device_config_sensor_input ret = %d\n", ret);

	stream_config_t stream_list;
	stream_t streams[1];

	memset(&streams[0], 0, sizeof(stream_t));

	streams[0].format = V4L2_PIX_FMT_YUYV;

	streams[0].width = 1280;
	streams[0].height = 720;
	streams[0].memType = V4L2_MEMORY_USERPTR;
	streams[0].field = 0;

	streams[0].size = 1280 * 720 * 2;
	streams[0].stride = 2560;
	stream_list.num_streams = 1;
	stream_list.streams = streams;
	stream_list.operation_mode = 2;
	ret = camera_device_config_streams(camera_id, &stream_list);
	printf("camera_test camera_device_config_streams ret = %d\n", ret);

	camera_buffer_t *buffers = (camera_buffer_t *)malloc(sizeof(camera_buffer_t) * buffer_count);
	camera_buffer_t *buf = buffers;
	memset(buffers, 0, sizeof(camera_buffer_t) * buffer_count);

	int bpp = 0;
	int buffer_size = get_frame_size(camera_id, V4L2_PIX_FMT_YUYV, 1280, 720, V4L2_FIELD_ANY, &bpp);

	printf("camera_test buffer_size = %d bpp %d\n", buffer_size, bpp);

	for (int i = 0; i < buffer_count; i++, buf++) {

		buf->s = streams[0];

		posix_memalign(&buf->addr, getpagesize(), buffer_size);
		ret = camera_stream_qbuf(camera_id, &buf);
		printf("camera_test camera_stream_qbuf ret = %d getpagesize() %d\n", ret, getpagesize());
	}

	ret = camera_device_start(camera_id);
	printf("camera_test camera_device_start ret = %d\n", ret);

	int stream_id = 0;
	camera_buffer_t buffer2;
	buf = buffers;
	for (int i = 0; i < 1000; i++) {
		printf("camera_test  call camera_stream_dqbuf 1280 * 721 i = %d\n", i);
		ret = camera_stream_dqbuf(camera_id, stream_id, &buf);
		if (i == 19) {
			FILE *fp = fopen("yuv_80.yuv", "w");
			fwrite(buf->addr, 1, buffer_size, fp);
			fclose(fp);
		}

		printf("camera_test camera_stream_dqbuf ret = %d buf->index %d %p\n", ret, buf->index, buf->addr);
		buf->sequence = -1;
		buf->timestamp = 0;
		ret = camera_stream_qbuf(camera_id, &buf);
		// processing data with buf
		printf("camera_test camera_stream_qbuf22 ret = %d\n", ret);
	}

	camera_device_stop(camera_id);
	camera_device_close(camera_id);
}
