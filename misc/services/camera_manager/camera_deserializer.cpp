/*
 * Copyright (C) 2018-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "camera_deserializer.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GET_SYMBOL(handle, p, symbol)                                                                                  \
	(p) = (typeof(p))dlsym((handle), (symbol));                                                                    \
	if ((p) == NULL) {                                                                                             \
		goto Error;                                                                                            \
	}                                                                                                              \
	pr_info("find %s\n", (symbol));

#define CHECK_RET(value)                                                                                               \
	if ((value) < 0) {                                                                                             \
		pr_info("%s faild, errno is 0x%x\n", __func__, errno);                                                 \
	}

camera_deserializer::camera_deserializer(int deserializer_id,const char* camera_lib) : m_process_thread(nullptr),
	m_deserializer_id(deserializer_id),m_ops{ 0 },m_hal_handle(nullptr),m_stream_list{0},m_camera_tid(0)
{
	pr_info("camera_deserializer::%s Enter\n", __func__);
	m_pending_requests.clear();
	m_camera_ids.clear();
};

camera_deserializer::~camera_deserializer() { pr_info("camera_deserializer::%s Enter\n", __func__); };

int camera_deserializer::open(int camera_id)
{
	int ret = -1;

	pr_info("camera_deserializer::%s Enter\n", __func__);
	if (m_ops.open != NULL) {
		ret = m_ops.open(camera_id);
	}

	CHECK_RET(ret);

	std::unique_lock<std::mutex> lock(m_camera_ids_mutex);
	m_camera_ids.insert(camera_id);

	return ret;
}

int camera_deserializer::close(int camera_id)
{
	pr_info("camera_deserializer::%s Enter\n", __func__);
	if (m_ops.close != NULL) {
		m_ops.close(camera_id);
	}

	std::unique_lock<std::mutex> lock(m_camera_ids_mutex);
	m_camera_ids.erase(camera_id);
	return 0;
}

int camera_deserializer::config_sensor_input(int camera_id, stream_t *input_config)
{
	int ret = 0;

	pr_info("camera_deserializer::%s Enter\n", __func__);
	if (m_ops.config_sensor_input)
		ret = m_ops.config_sensor_input(camera_id, input_config);

	CHECK_RET(ret);
	return ret;
}

int camera_deserializer::init()
{
	int ret = 0;

	pr_info("camera_deserializer::%s Enter\n", __func__);
	if (m_ops.hal_init != NULL)
		ret = m_ops.hal_init();

	if (ret != 0) {
		pr_info("camera_deserializer::%s init camera faild\n", __func__);
		return ret;
	}

	return 0;
}

int camera_deserializer::qbuf(int camera_id, camera_buffer_t **buffer, int num_buffers, void *settings)
{
	int ret = -1;

	pr_info("camera_deserializer::%s Enter\n", __func__);
	if (m_ops.stream_qbuf)
		ret = m_ops.stream_qbuf(camera_id, buffer, num_buffers, settings);

	CHECK_RET(ret);
	return ret;
}

int camera_deserializer::stream_qbuf(int camera_id, camera_buffer_t **buffer, int num_buffers, void *settings)
{
	int ret = 0;
	camera_request request = {0};

	pr_info("camera_deserializer::%s Enter\n", __func__);
	request.m_camera_id = camera_id;
	request.m_buffer_number = num_buffers;

	for (int i = 0; i < num_buffers; i++) {
		request.m_buffers[i] = *buffer[i];
	}

	ret = qbuf(camera_id, buffer, num_buffers);

	m_pending_request.push(request);
	m_request_signal.notify_one();

	CHECK_RET(ret);
	return ret;
}

int camera_deserializer::dqbuf(int camera_id, int stream_id, camera_buffer_t **buffer, void *settings)
{
	int ret = -1;

	pr_info("camera_deserializer::%s Enter\n", __func__);
	if (m_ops.stream_dqbuf)
		ret = m_ops.stream_dqbuf(camera_id, stream_id, buffer, settings);

	CHECK_RET(ret);
	return ret;
}

int camera_deserializer::streams_config(int camera_id, stream_config_t *stream_list)
{
	int ret = 0;

	pr_info("camera_deserializer::%s Enter\n", __func__);
	if (m_ops.config_streams)
		ret = m_ops.config_streams(camera_id, stream_list);

	m_stream_list[camera_id] = *stream_list;
	pr_info("camera_deserializer::%s m_stream_list[%d] streams number is %d\n",
	        __func__,
	        camera_id,
	        m_stream_list[camera_id].num_streams);
	CHECK_RET(ret);
	return ret;
}

int camera_deserializer::stream_start(int camera_id)
{
	int ret = -1;
	if (!m_process_thread) {
		try {
			m_process_thread = new std::thread(process_request, this);
		} catch (const std::system_error &e) {
			pr_info("camera_deserializer::%s create process_request faild, %s\n", __func__, e.what());
		}
		m_camera_tid = m_process_thread->get_id();
	}
	pr_info("camera_deserializer::%s Enter\n", __func__);
	if (m_ops.start_stream) {
		ret = m_ops.start_stream(camera_id);
	}

	CHECK_RET(ret);
	return ret;
}

int camera_deserializer::get_frame_size(int camera_id, int format, int width, int height, int field, int *bpp)
{
	int size = 0;

	pr_info("camera_deserializer::%s Enter\n", __func__);
	if (m_ops.get_frame_size) {
		size = m_ops.get_frame_size(camera_id, format, width, height, field, bpp);
	} else { // default format is YUYV
		size = width * height * 2;
	}

	return size;
}

int camera_deserializer::stream_stop(int camera_id)
{
	int ret = -1;

	pr_info("camera_deserializer::%s Enter\n", __func__);
	if (m_ops.stop_stream)
		ret = m_ops.stop_stream(camera_id);

	CHECK_RET(ret);
	return ret;
}

int camera_deserializer::get_stream_id(int camera_id, stream_t s)
{
	int i = -1;

	pr_info("camera_deserializer::%s Enter\n", __func__);
	pr_info("camera_deserializer::%s streams number is %d\n", __func__, m_stream_list[camera_id].num_streams);
	for (i = 0; i < m_stream_list[camera_id].num_streams; i++) {
		pr_info("camera_deserializer::%s streams[%d] info: 0x%x, %d x %d\n",
		        __func__,
		        i,
		        m_stream_list[camera_id].streams[i].format,
		        m_stream_list[camera_id].streams[i].width,
		        m_stream_list[camera_id].streams[i].height);
		if (s.format == m_stream_list[camera_id].streams[i].format &&
		    s.width == m_stream_list[camera_id].streams[i].width &&
		    s.height == m_stream_list[camera_id].streams[i].height)
			break;
	}
	pr_info("camera_deserializer::%s stream id is %d\n", __func__, i);
	return i;
}

/**
 * This function assumes that the requests sent by all cameras are inter-overlapping
 *
 * TODO: sometimes,a camera continuously sends multiple requests, this will result
 * in additional wait time for other cameras, so we should optimize it.
 */
int camera_deserializer::get_next_request(camera_request &request)
{
	pr_info("camera_deserializer::%s Enter\n", __func__);
	if (!m_pending_request.empty()) {
		request = m_pending_request.front();
		m_pending_request.pop();
		return 0;
	} else {
		return -1;
	}
}

/**
 * The process_request can handle the data from multiple cameras, and it can DQ multiple
 * buffers within a single request.
 */
void camera_deserializer::process_request(camera_deserializer *p)
{
	int ret = 0;
	int stream_id = 0;
	camera_request request = {0};
	camera_data notify_data = {0};
	camera_buffer_t *buffer = nullptr;
	pr_info("camera_deserializer::%s Enter\n", __func__);
	do {
		/*Use scopes to avoiding explicit unlock*/
		{
			std::unique_lock<std::mutex> lock(p->m_request_mutex);

			if (p->m_pending_request.empty())
				p->m_request_signal.wait(lock);
			pr_info("camera_deserializer::%s begin process\n", __func__);
			ret = p->get_next_request(request);
		}

		if (ret == 0) {
			for (int i = 0; i < request.m_buffer_number; i++) {
				stream_id = p->get_stream_id(request.m_camera_id, request.m_buffers[i].s);
				pr_info("camera_deserializer::%s call dqbuf stream_id is %d\n", __func__, stream_id);
				buffer = &request.m_buffers[i];
				ret = p->dqbuf(request.m_camera_id, stream_id, &buffer);
				// TODO, add some camera states check
				// if dqbuf timeout, try again
				if (ret != 0)
					continue;
			}
			pr_info("camera_deserializer::%s after call dqbuf %d\n", __func__, ret);
			if (ret == 0) {
				notify_data.id = request.m_camera_id;
				notify_data.request = request;
				pr_info("camera_deserializer::%s call notify\n", __func__);
				p->notify(&notify_data);
			}
		}
	} while (p->m_camera_ids.size() > 0); // TODO, check the active camera list

	pr_info("camera_deserializer::%s Exit!!\n", __func__);
}

void camera_deserializer::deinit()
{
	pr_info("camera_deserializer::%s Enter\n", __func__);
	m_process_thread->join();
	delete m_process_thread;
}

int camera_deserializer_hal::fill_hal_ops(const char *hal_name)
{
	m_hal_handle = dlopen(hal_name, RTLD_LAZY);
	if (m_hal_handle == NULL) {
		pr_info("Failed to open %s %s\n", hal_name, dlerror());
	} else {
		GET_SYMBOL(m_hal_handle, m_ops.get_camera_info, "vcamera_get_camera_info");
		GET_SYMBOL(m_hal_handle, m_ops.hal_init, "vcamera_hal_init");
		GET_SYMBOL(m_hal_handle, m_ops.hal_deinit, "vcamera_hal_deinit");
		GET_SYMBOL(m_hal_handle, m_ops.open, "vcamera_device_open");
		GET_SYMBOL(m_hal_handle, m_ops.close, "vcamera_device_close");
		GET_SYMBOL(m_hal_handle, m_ops.config_sensor_input, "vcamera_device_config_sensor_input");
		GET_SYMBOL(m_hal_handle, m_ops.config_streams, "vcamera_device_config_streams");
		GET_SYMBOL(m_hal_handle, m_ops.start_stream, "vcamera_device_start");
		GET_SYMBOL(m_hal_handle, m_ops.stop_stream, "vcamera_device_stop");
		GET_SYMBOL(m_hal_handle, m_ops.allocate_memory, "vcamera_device_allocate_memory");
		GET_SYMBOL(m_hal_handle, m_ops.get_frame_size, "vcamera_get_frame_size");
		GET_SYMBOL(m_hal_handle, m_ops.stream_qbuf, "vcamera_stream_qbuf");
		GET_SYMBOL(m_hal_handle, m_ops.stream_dqbuf, "vcamera_stream_dqbuf");
		GET_SYMBOL(m_hal_handle, m_ops.set_parameters, "vcamera_set_parameters");
		GET_SYMBOL(m_hal_handle, m_ops.get_parameters, "vcamera_get_parameters");
		GET_SYMBOL(m_hal_handle, m_ops.get_formats_number, "vcamera_get_formats_number");
		GET_SYMBOL(m_hal_handle, m_ops.get_formats, "vcamera_get_formats");
		return 0;
	Error:
		pr_info("Failed to find function in %s %s\n", hal_name, dlerror());
		dlclose(m_hal_handle);
		m_hal_handle = NULL;
	}
	return -1;
}

void camera_deserializer_hal::close_hal_handle()
{
	pr_info("camera_deserializer::%s Enter\n", __func__);
	memset(&m_ops, 0, sizeof(m_ops));
	if (m_hal_handle) {
		dlclose(m_hal_handle);
		m_hal_handle = NULL;
	}
}

camera_deserializer_hal::camera_deserializer_hal(int deserializer_id, const char *camera_lib)
    : camera_deserializer(deserializer_id, camera_lib)
{
	pr_info("camera_deserializer_hal::%s Enter\n", __func__);
	fill_hal_ops(camera_lib);
	init();
};

camera_deserializer_hal::~camera_deserializer_hal()
{
	pr_info("camera_deserializer_hal::%s Enter\n", __func__);
	m_process_thread->join();
	delete m_process_thread;
	close_hal_handle();
};

struct camera_dev {
	int id;
	int fd;
	char name[10];
	uint8_t buffer_count;

	interface_type type;
};

/* TODO: get camera info from scenario*/
static struct camera_dev camera_devs[] = {
    {
        .id = 0,
        .name = "video0",
        .type = HAL_INTERFACE,
    },
    {
        .id = 1,
        .name = "video2",
        .type = HAL_INTERFACE,
    },
    {
        .id = 2,
        .name = "video2",
        .type = HAL_INTERFACE,
    },
    {
        .id = 3,
        .name = "video3",
        .type = HAL_INTERFACE,
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

static int v4l2_open(int camera_id)
{
	char dev_name[20];
	pr_info("v4l2 %s Enter\n", __func__);
	sprintf(dev_name, "/dev/%s", camera_devs[camera_id].name);
	camera_devs[camera_id].fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);
	pr_info("v4l2 open '%s', fd = %d\n", dev_name, camera_devs[camera_id].fd);
	if (-1 == camera_devs[camera_id].fd) {
		pr_info("Cannot open '%s'\n", dev_name);
		return -1;
	}

	return 0;
};

static void v4l2_close(int camera_id)
{
	int fd = camera_devs[camera_id].fd;
	pr_info("v4l2 %s Enter\n", __func__);
	close(fd);
};

static int v4l2_start_stream(int camera_id)
{
	int fd = camera_devs[camera_id].fd;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	pr_info("v4l2 %s Enter\n", __func__);
	return ioctl(fd, VIDIOC_STREAMON, &type);
};

static int v4l2_stop_stream(int camera_id)
{
	int fd = camera_devs[camera_id].fd;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	pr_info("v4l2 %s Enter\n", __func__);
	return ioctl(fd, VIDIOC_STREAMOFF, &type);
};

static int v4l2_create_memory(int camera_id, camera_buffer_t *buffer)
{
	int fd = camera_devs[camera_id].fd;
	struct v4l2_buffer buf = {0};
	pr_info("v4l2 %s Enter\n", __func__);
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = 0;
	return ioctl(fd, VIDIOC_QUERYBUF, &buf);
};

static int v4l2_g_fmt(int camera_id, void *data, int64_t sequence)
{
	int ret = 0;
	int fd = camera_devs[camera_id].fd;
	struct v4l2_format fmt = {0};
	pr_info("v4l2 %s Enter\n", __func__);
	ret = ioctl(fd, VIDIOC_G_FMT, &fmt);
	memcpy(data, &fmt, sizeof(fmt));
	return ret;
};

static int v4l2_s_fmt(int camera_id, void *data)
{
	int ret = 0;
	int fd = camera_devs[camera_id].fd;
	struct v4l2_format *fmt = (struct v4l2_format *)data;
	pr_info("v4l2 %s Enter\n", __func__);
	ret = ioctl(fd, VIDIOC_S_FMT, fmt);
	return ret;
}

static int v4l2_reqbufs(int camera_id)
{
	int ret = 0;
	int fd = camera_devs[camera_id].fd;
	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));
	req.count = camera_devs[camera_id].buffer_count;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_USERPTR;
	ret = ioctl(fd, VIDIOC_REQBUFS, &req);
	pr_info("v4l2 VIDIOC_REQBUFS count %d buffers and ret= %d\n", camera_devs[camera_id].buffer_count, ret);
	return ret;
}

static int v4l2_qbuf(int camera_id, camera_buffer_t **buffer, int num_buffers, void *settings)
{
	int fd = camera_devs[camera_id].fd;
	struct v4l2_buffer buf = {0};
	pr_info("v4l2 %s Enter\n", __func__);

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.index = buffer[0]->index;
	pr_info("v4l2 v4l2_qbuf index = %d\n", buf.index);
	buf.m.userptr = (long unsigned int)buffer[0]->addr;

	buf.length = buffer[0]->s.size;
	int ret = ioctl(fd, VIDIOC_QBUF, &buf);

	pr_info("v4l2 camera_id %d, fd %d v4l2_qbuf memset p-> 0x%lx size = %d ret = %d\n",
	        camera_id,
	        fd,
	        buf.m.userptr,
	        buf.length,
	        ret);
	return ret;
};

static int v4l2_dqbuf(int camera_id, int stream_id, camera_buffer_t **buffer, void *settings)
{
	struct v4l2_buffer buf = {0};
	int fd = camera_devs[camera_id].fd;
	int ret = 0;
	struct pollfd pfd = {0};
	pr_info("v4l2 %s Enter\n", __func__);
	pfd.fd = fd;
	pfd.events = POLLPRI | POLLIN | POLLERR;
	ret = poll(&pfd, 1, 2000);
	pr_info("v4l2 poll fd %d ret = %d\n", fd, ret);
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_USERPTR;
	ret = ioctl(fd, VIDIOC_DQBUF, &buf);
	if (ret == 0) {
		buffer[0]->addr = (void *)buf.m.userptr;
		pr_info("v4l2 get a buffer: %p index %d\n", buffer[0]->addr, buf.index);
	}
	return ret;
};

static int v4l2_get_info(int camera_id, void *data)
{
	int fd = camera_devs[camera_id].fd;
	struct v4l2_capability cap = {0};
	pr_info("v4l2 %s Enter\n", __func__);
	return ioctl(fd, VIDIOC_QUERYCAP, &cap);
};

camera_deserializer_v4l2::camera_deserializer_v4l2(int deserializer_id, const char *camera_lib)
    : camera_deserializer(deserializer_id, camera_lib)
{
	pr_info("camera_deserializer_v4l2::%s Enter\n", __func__);

	fill_hal_ops(camera_lib);
	init();
};

int camera_deserializer_v4l2::fill_hal_ops(const char *hal_name)
{
	memset(&m_ops, 0, sizeof(m_ops));
	m_ops.get_camera_info = v4l2_get_info;
	m_ops.open = v4l2_open;
	m_ops.close = v4l2_close;
	m_ops.allocate_memory = v4l2_create_memory;
	m_ops.start_stream = v4l2_start_stream;
	m_ops.stop_stream = v4l2_stop_stream;
	m_ops.stream_qbuf = v4l2_qbuf;
	m_ops.stream_dqbuf = v4l2_dqbuf;
	m_ops.req_bufs = v4l2_reqbufs;
	m_ops.set_parameters = v4l2_s_fmt;
	m_ops.get_parameters = v4l2_g_fmt;
	return 0;
};

int camera_deserializer_v4l2::streams_config(int camera_id, stream_config_t *stream_list)
{
	int ret = 0;
	struct v4l2_format fmt = {0};

	m_stream_list[camera_id] = *stream_list;
	pr_info("camera_deserializer_v4l2::%s m_stream_list[%d] streams number is %d\n",
	        __func__,
	        camera_id,
	        m_stream_list[camera_id].num_streams);

	pr_info("camera_deserializer_v4l2::%s Enter\n", __func__);
	memset(&fmt, 0, sizeof(struct v4l2_format));

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = stream_list->streams[0].width;
	fmt.fmt.pix.height = stream_list->streams[0].height;
	fmt.fmt.pix.pixelformat = stream_list->streams[0].format;
	fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

	pr_info("camera_deserializer_v4l2::%s fmt (%d x %d with 0x%x format)\n",
	        __func__,
	        fmt.fmt.pix.width,
	        fmt.fmt.pix.height,
	        fmt.fmt.pix.pixelformat);

	if (m_ops.set_parameters != NULL) {
		ret = m_ops.set_parameters(camera_id, &fmt);
		pr_info(
		    "camera_deserializer_v4l2::%s m_ops.set_parameters ret %d errno is 0x%x\n", __func__, ret, errno);
	}

	if (m_ops.req_bufs != NULL) {
		camera_devs[camera_id].buffer_count = MAX_CAMERA_BUFFER;
		ret = m_ops.req_bufs(camera_id);
		pr_info("camera_deserializer_v4l2::%s m_ops.req_bufs ret %d\n", __func__, ret);
	}
	return ret;
}

void camera_deserializer_v4l2::close_hal_handle(){

};

/**
 * This function create deserializer object by interface type
 * Now, we have implement HAL interface and will add V4L2 interface for SOC Sensor
 *
 * @param deserializer_id
 * @param camera_lib
 * @param type
 * @return camera_deserializer*
 */
camera_deserializer *create_deserializer(int deserializer_id, const char *camera_lib, int type)
{
	camera_deserializer *p = nullptr;

	try {
		switch (type) {
		case V4L2_INTERFACE:
			p = new camera_deserializer_v4l2(deserializer_id, camera_lib);
			break;
		case HAL_INTERFACE:
			p = new camera_deserializer_hal(deserializer_id, camera_lib);
			break;
		default:
			throw "invalid interface type!";
			break;
		}
	} catch (const std::system_error &e) {
		pr_info("camera::%s create deserializer faild, %s\n", __func__, e.what());
	}

	return p;
}
