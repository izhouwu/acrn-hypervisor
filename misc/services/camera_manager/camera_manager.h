/*
 * Copyright (C) 2018-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */
#pragma once
#include "camera.h"
#include "camera_data_provider.h"
#include <arpa/inet.h>
#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <pthread.h>
#include <queue>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "camera_config.h"

const int VIRTUAL_CAMERA_NUMQ = 12;
const int MAX_BUFFER_COUNT = 10;
const int MAX_PIPELINE_NUMBER = 4;
const int DEFAULT_PORT = 8000;

using namespace std;
using namespace icamera;

const int default_socket_port = 8000;
const int g_buffer_count = 6;
const std::string default_socket_address = "127.0.0.1";

std::string get_scenario_address_config()
{
	camera_manager_info info = {0};
	get_camera_manager_config(info);
	return (info.address.empty() || info.address.find_first_not_of(' ') == std::string::npos) ?
		default_socket_address : info.address;
}

int get_scenario_port_config()
{
	camera_manager_info info = {0};
	get_camera_manager_config(info);
	return (info.port != 0) ? info.port : default_socket_port;
}

enum camera_client_state { CLIENT_DISCONNECT = 0,CLIENT_CONNECT = 1 };
enum camera_stream_state { STREAM_OFF = 0,STREAM_ON = 1 };

struct virtual_vq_related {
	pthread_mutex_t req_mutex;
	pthread_cond_t req_cond;
	int in_process;
};

struct virtual_v4l2_device {
	void* buffer;
	size_t size;
	char* dev_path;
	int fd;
};

struct virtual_media_device {
	char* dev_path;
	int fd;
};

struct virtual_v4l2sub_device {
	char* dev_path;
	int fd;
};

struct virtual_camera_format_size {
	union {
		uint32_t min_width;
		uint32_t width;
	};
	uint32_t max_width;
	uint32_t step_width;

	union {
		uint32_t min_height;
		uint32_t height;
	};
	uint32_t max_height;
	uint32_t step_height;
	uint32_t stride;
	uint32_t sizeimage;
};

struct virtual_camera_req_format {
	uint32_t pixelformat;
	struct virtual_camera_format_size size;
};

struct vcamera_format {
	uint32_t width;
	uint32_t max_width;
	uint32_t step_width;
	uint32_t height;
	uint32_t max_height;
	uint32_t step_height;
	uint32_t stride;
	uint32_t sizeimage;
};

struct picture_format {
	uint32_t pixel_format_type;
	struct vcamera_format camera_format;
};

struct camera_buffer_ref {
	uint32_t segment;
	// char uuid[16];
	int index;
};

struct dma_buf_info {
	int32_t ref_count;
	int dmabuf_fd;
};

typedef enum {
	VIRTUAL_CAMERA_GET_FORMAT = 1,
	VIRTUAL_CAMERA_SET_FORMAT = 2,
	VIRTUAL_CAMERA_TRY_FORMAT = 3,
	VIRTUAL_CAMERA_ENUM_FORMAT = 4,
	VIRTUAL_CAMERA_ENUM_SIZE = 5,
	VIRTUAL_CAMERA_CREATE_BUFFER = 6,
	VIRTUAL_CAMERA_DEL_BUFFER = 7,
	VIRTUAL_CAMERA_QBUF = 8,
	VIRTUAL_CAMERA_STREAM_ON = 9,
	VIRTUAL_CAMERA_STREAM_OFF = 10,
	VIRTUAL_CAMERA_OPEN = 11,
	VIRTUAL_CAMERA_CLOSE = 12,
	VIRTUAL_CAMERA_DQBUF = 13,

	VIRTUAL_CAMERA_RET_OK = 0x100,

	VIRTUAL_CAMERA_RET_UNSPEC = 0x200,
	VIRTUAL_CAMERA_RET_BUSY = 0x201,
	VIRTUAL_CAMERA_RET_OUT_OF_MEMORY = 0x202,
	VIRTUAL_CAMERA_RET_INVALID = 0x203,
} virtual_camera_request_type;

struct virtual_camera_request {
	int index;
	int camera_id;

	virtual_camera_request_type type;
	struct picture_format format;
	struct camera_buffer_ref buffer;

	char reserve[24];
};

struct camera_client_state_request {
	int index;
	int client_id;
	camera_client_state state;
	char reserve[24];
};

struct camera_info {
	/*The camera physical id*/
	int id;
	camera_stream_state state;
	camera_buffer_t* buffers;
	std::shared_ptr<camera> instance;
};

class virtual_cameras : public camera_data_consumer
{
public:
	virtual_cameras(int client_id,int socket);
	~virtual_cameras();

	int get_stream_id(struct virtual_camera_request* req);
	int get_camera_list();
	int get_virtual_camera_id(int physical_camera_id);

protected:
	virtual int handle_data(camera_data* pdata);

private:
	/* socket handle to camera */
	int m_socket;
	int m_closing;
	int m_camera_number;
	int m_client_id;
	static void* msg_thread(virtual_cameras* p);
	int handle_msg(struct virtual_camera_request* req);
	int fill_camera_request(virtual_camera_request* rsp,void* p);
	void release_camera_buffer(int camera_id);

	camera_client_state m_state;
	std::array<camera_info,VIRTUAL_CAMERA_NUMQ> m_cameras_info;
	std::queue<struct virtual_camera_request> m_camera_client_msg;
	std::thread* m_thread;
	std::thread::id m_thread_tid;
	std::condition_variable m_request_signal;
	std::mutex m_request_mutex;
	int m_index;
};

class camera_manager
{
public:
	static camera_manager& get_instance()
	{
		static camera_manager instance;
		return instance;
	}
	int init();
	void push_msg(camera_client_state_request& req);
	camera_manager(const camera_manager&) = delete;
	camera_manager& operator=(const camera_manager&) = delete;

private:
	static void* process_handle(camera_manager* data);
	int client_control(camera_client_state_request& req);

	std::mutex m_msg_mutex;
	std::thread* m_process_thread;
	std::queue<camera_client_state_request> m_msg;
	std::condition_variable m_msg_signal;

	camera_manager() {}
	~camera_manager() {}
};
