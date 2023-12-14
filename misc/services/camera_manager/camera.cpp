/*
 * Copyright (C) 2018-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "camera.h"
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int get_deserializer_id(int camera_id)
{
	// TODO, get deserializer id from scenario
	return camera_id;
}

std::mutex camera::m_camera_deserializers_mutex;
std::shared_ptr<camera_deserializer> camera::m_camera_deserializers[MAX_DESERIALIZER_NUMBER] = {nullptr};

std::shared_ptr<camera_deserializer> camera::get_deserializer(int camera_id, const char *camera_lib)
{
	int deserializer_id = get_deserializer_id(camera_id);
	auto_lock l(camera::m_camera_deserializers_mutex);
	if (!m_camera_deserializers[deserializer_id]) {
		std::shared_ptr<camera_deserializer> sp(
		    create_deserializer(deserializer_id, camera_lib, m_camera_info.type));
		m_camera_deserializers[deserializer_id] = sp;
	}

	return m_camera_deserializers[deserializer_id];
}

camera::camera(int camera_id) : m_buffer_number(MAX_CAMERA_BUFFER),m_camera_id(camera_id),m_streams{ 0 },
	m_stream_list{ 0 },m_camera_info{ 0 },m_buffers{0}
{
	pr_info("camera::%s Enter\n", __func__);

	m_camera_info.id = camera_id;
	get_physical_camera_config(m_camera_info);

#ifdef SHARE_DESERIALIZER
	m_camera_deserializer = get_deserializer(m_camera_id, m_camera_info.driver.c_str());
#else
	std::shared_ptr<camera_deserializer> sp(
	    create_deserializer(camera_id, m_camera_info.driver.c_str(), m_camera_info.type));
	m_camera_deserializer = sp;
#endif
	m_camera_deserializer->register_consumer(this);

	open();
	streams_init();
	buffers_init();
	enqueue_all_buffers();
}

camera::~camera()
{
	close();
	buffers_uninit();
}

int camera::open()
{
	int ret = 0;
	stream_t input_config = {0};

	pr_info("camera::%s Enter\n", __func__);

	ret = m_camera_deserializer->open(m_camera_id);
	pr_info("camera manager m_camera_deserializer->open ret = %d\n", ret);

	memset(&input_config, 0, sizeof(stream_t));
	input_config.format = -1;
	ret = m_camera_deserializer->config_sensor_input(m_camera_id, &input_config);
	pr_info("camera manager m_camera_deserializer->config_sensor_input ret = %d\n", ret);

	return ret;
}

int camera::close()
{
	int ret = 0;

	pr_info("camera::%s Enter\n", __func__);

	ret = m_camera_deserializer->close(m_camera_id);

	pr_info("camera manager m_camera_deserializer->close ret = %d\n", ret);

	return ret;
}

static int get_stride_size(int width, int format)
{
	int stride = 0;

	switch (format) {
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_YYUV:
	case V4L2_PIX_FMT_YVYU:
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_VYUY:
		stride = ALIGN_UP(width * 2, 64);
		break;
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
		stride = ALIGN_UP(width, 64);
		break;
	default:
		stride = ALIGN_UP(width * 2, 64);
		break;
	}

	return stride;
}

/**
 * This function create stream list and stream_t for each camera object.
 * We could config different stream for each camera
 *
 * @return int
 */
int camera::streams_init()
{
	int ret = 0;
	int buffer_count = 6;
	int num_streams = 1;
	int bpp = 0;

	int format = m_camera_info.format;
	int width = m_camera_info.width;
	int height = m_camera_info.height;
	int buffer_size = get_frame_size(format, width, height, V4L2_FIELD_ANY, &bpp);

	pr_info("camera::%s Enter buffer_size = %d\n", __func__, buffer_size);

	/*
	 * The Intel IPU HAL support multiple streams, we use the stream 0 as default.
	 * If we use V4L2 interface, only stream 0 could be used.
	 */

	m_streams[0].format = format;
	m_streams[0].width = width;
	m_streams[0].height = height;
	m_streams[0].memType = V4L2_MEMORY_USERPTR;
	m_streams[0].field = 0;
	m_streams[0].size = buffer_size;
	m_streams[0].stride = get_stride_size(width, format);

	m_stream_list.num_streams = num_streams;
	m_stream_list.streams = m_streams;
	m_stream_list.operation_mode = CAMERA_STREAM_CONFIGURATION_MODE_AUTO;
	ret = m_camera_deserializer->streams_config(m_camera_id, &m_stream_list);
	pr_info("camera manager operation_mode %d m_camera_deserializer->streams_config ret = %d\n",
	        m_stream_list.operation_mode,
	        ret);

	return 0;
}

int camera::start()
{
	int ret = 0;
	auto_lock l(m_consumers_mutex);
	pr_info("camera::%s Enter\n", __func__);
	if (m_consumers.size() == 0) {
		ret = m_camera_deserializer->stream_start(m_camera_id);
	}

	pr_info("camera manager m_camera_deserializer->streams_start ret = %d\n", ret);
	return ret;
}

/**
 * This function foward the qbuf request to deserializer
 * We have record how many consumers register to these buffers
 * We call stream_qbuf of deserialize only there is no any consumer used it.
 *
 * @param buffer, the buffer list
 * @param num_buffers, buffers number
 * @param settings, for metadata setting
 * @return int
 */
int camera::qbuf(camera_buffer_t **buffer, int num_buffers, camera_data_consumer *consumer, void *settings)
{
	int stream_id = 0;
	auto_lock ll(m_buffer_map_mutex);

	pr_info("camera::%s Enter,consumer %p camera_buffer_t[0]->addr %p\n", __func__, consumer, buffer[0]->addr);

	for (int i = 0; i < num_buffers; i++) {
		try {
			buffer[i]->s = m_streams[m_camera_deserializer->get_stream_id(m_camera_id, buffer[i]->s)];
		} catch (const std::out_of_range &e) {
			pr_info("camera::%s stream id error %s\n", __func__, e.what());
		}
	}
	pr_info("camera::%s consume %p buffer[0] stream info(0x%x, %dx%d, type %d, field %d, size %d, stride %d)\n",
	        __func__,
	        consumer,
	        buffer[0]->s.format,
	        buffer[0]->s.width,
	        buffer[0]->s.height,
	        buffer[0]->s.memType,
	        buffer[0]->s.field,
	        buffer[0]->s.size,
	        buffer[0]->s.stride);

	try {
		auto &info = m_buffer_map.at(buffer[0]->addr);
		switch (info.state) {
		case INIT:
			pr_info("camera::qbuf find the address, state is INIT call stream_qbuf\n");
			m_camera_deserializer->stream_qbuf(m_camera_id, buffer, num_buffers, settings);
			info.state = PENDING;
			info.using_clients.clear();
			break;

		case USING:
			for (const auto &element : m_buffer_map) {
				pr_info("call by client %p, m_buffer_map [%p], state is %d, client size %ld\n",
				        consumer,
				        element.first,
				        element.second.state,
				        element.second.using_clients.size());
			}

			info.using_clients.erase(consumer);
			if (info.using_clients.size() == 0) {
				pr_info("camera::qbuf find the address, state is USING, call stream_qbuf\n ");
				m_camera_deserializer->stream_qbuf(m_camera_id, buffer, num_buffers, settings);
				info.state = PENDING;
			}

			break;
		case PENDING:
			break;
		default:
			pr_info("camera::qbuf, invalid camera buffer state\n ");
			break;
		}

		return 0;
	} catch (const std::out_of_range &e) {
		pr_info("camera::qbuf faild, can't find the address\n ");
		return -1;
	}
}

/**
 * A callback function of the camera consumers, will be called by the camera deserializer class
 * If there is no listerner, this function will call stream_qbuf to release these buffers
 *
 * @param pdata
 * @return int
 */
int camera::handle_data(camera_data *pdata)
{
	pr_info("camera::handle_data Enter, call notify\n");
	{
		auto_lock l(m_consumers_mutex);
		if (m_consumers.size() == 0) {
			camera_buffer_t *buffer = pdata->request.m_buffers;
			camera_buffer_t **buffers = &buffer;
			return m_camera_deserializer->stream_qbuf(
			    m_camera_id, buffers, pdata->request.m_buffer_number, NULL);
		}
	}

	notify(pdata);
	return 0;
}

/**
 * This function create a share memory region which named by "camera_daemon_mem_XXX",
 * the "XXX" is the camera ID, these buffers could be access by the camera client
 * So, there is no memory copy between camera daemon and camera client
 *
 */
int camera::buffers_init()
{
	int ret = 0;
	int buffers_size = 0;
	camera_buffer_t *buf = nullptr;
	int total_memory_size = 0;

	pr_info("camera::%s Enter\n", __func__);
	if (m_stream_list.num_streams < 0) {
		return -1;
	}

	for (int i = 0; i < m_stream_list.num_streams; i++) {
		total_memory_size += (ALIGN_UP(m_streams[i].size, getpagesize())) * m_buffer_number;
	}

	pr_info("%s total_memory_size = %d\n", __func__, total_memory_size);

	m_mem_name = "camera_daemon_mem_" + std::to_string(m_camera_id);
	int fd = shm_open(m_mem_name.c_str(), O_RDWR | O_CREAT, 0666);
	if (fd == -1) {
		pr_info("%s create failed\n", m_mem_name.c_str());
		return -1;
	} else if (ftruncate(fd, total_memory_size) == -1) {
		pr_info("%s  ftruncate failed\n", m_mem_name.c_str());
		return -1;
	}

	pr_info("create camera_daemon_mem_%d success\n", m_camera_id);

	for (int i = 0; i < m_stream_list.num_streams; i++) {
		int frame_size = ALIGN_UP(m_streams[i].size, getpagesize());

		m_shared_mem[i] = mmap(NULL, frame_size * m_buffer_number, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		pr_info("m_mem_name is %s, total size is %d, frame_size is %d\n",
		        m_mem_name.c_str(),
		        m_streams[i].size * m_buffer_number,
		        frame_size);
		if (m_shared_mem[i] == MAP_FAILED) {
			pr_info("%s map failed\n", m_mem_name.c_str());
			return -1;
		} else {
			memset(m_shared_mem[i], 0, frame_size * m_buffer_number);
			pr_info("%s set to zero\n", m_mem_name.c_str());
		}

		void *offset = m_shared_mem[i];
		buf = m_buffers[i];
		pr_info("camera manager getpagesize = %d frame_size =  %d\n", getpagesize(), frame_size);
		for (int j = 0; j < m_buffer_number; j++, buf++) {
			buf->s = m_streams[i];
			buf->addr = (void *)((char *)offset + frame_size * j);

			if (buf->addr == nullptr) {
				pr_info("camera::buffers_init failed\n");
				return -1;
			}
		}
	}

	return ret;
}

int camera::buffers_uninit()
{
	int ret = 0;
	int buffers_size = 0;
	camera_buffer_t *buf = nullptr;
	int total_memory_size = 0;

	pr_info("camera::%s Enter\n", __func__);

	for (int i = 0; i < m_stream_list.num_streams; i++) {
		int frame_size = ALIGN_UP(m_streams[i].size, getpagesize());

		ret = munmap(m_shared_mem[i], frame_size * m_buffer_number);
	}

	if (ret != 0) {
		pr_info("camera::%s failed\n", __func__);
	}
	return ret;
}

camera_buffer_t *camera::get_buffers(int stream_id)
{
	pr_info("camera::%s Enter stream_id %d\n", __func__, stream_id);
	if ((stream_id >= 0) && (stream_id < m_stream_list.num_streams)) {
		return m_buffers[stream_id];
	} else {
		return nullptr;
	}
}

int camera::enqueue_all_buffers()
{
	int stream_id = 0;
	camera_buffer_t* buf[m_stream_list.num_streams] = {0};

	pr_info("camera::%s Enter, m_stream_list.num_streams = %d\n",__func__,m_stream_list.num_streams);

	for (int i = 0; i < m_buffer_number; i++) {
		auto_lock ll(m_buffer_map_mutex);
		for (stream_id = 0; stream_id < m_stream_list.num_streams; stream_id++) {
			camera_buffer_info info;
			info.state = INIT;
			info.using_clients.clear();
			m_buffer_map.insert(pair(m_buffers[stream_id][i].addr, info));
			pr_info("camera::%s insert %p\n", __func__, m_buffers[stream_id][i].addr);
		}
	}

	return 0;
}

/**
 * The notify function call the handle_date of virtual_camera to forward buffers
 * We use m_buffer_map to record how many virtual_camera object rigister to it
 */
void camera::notify(camera_data *pdata)
{
	auto_lock l(m_consumers_mutex);
	auto_lock ll(m_buffer_map_mutex);

	pr_info("camera::notify Enter m_consumers size is %ld\n", m_consumers.size());

	for (auto consumer : m_consumers) {
		pr_info("camera_data_provider::notify call handle_data\n");
		consumer->handle_data(pdata);
	}

	for (int i = 0; i < pdata->request.m_buffer_number; i++) {
		auto it = m_buffer_map.find(pdata->request.m_buffers[i].addr);
		if (it != m_buffer_map.end()) {
			camera_buffer_info *p = &it->second;
			p->using_clients = m_consumers;
			pr_info("camera::notify send to p->using_clients size is %ld\n", p->using_clients.size());
			p->state = USING;
		} else {
			for (const auto &element : m_buffer_map) {
				pr_info("camera::notify m_buffer_map [%p], state is %d, client size %ld\n",
				        element.first,
				        element.second.state,
				        element.second.using_clients.size());
			}
			pr_info("camera::notify failed, get wrong address %p\n", pdata->request.m_buffers[i].addr);
		}
	}
}

int camera::get_frame_size(int format, int width, int height, int field, int *bpp)
{
	return m_camera_deserializer->get_frame_size(m_camera_id, format, width, height, field, bpp);
}
