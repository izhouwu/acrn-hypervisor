/*
 * Copyright (C) 2018-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "camera_client.h"

struct virtual_camera_request g_response;
int g_socket;
int g_fd;
const int g_max_camera_number = 12;
pthread_t g_camera_tid;

virtual_camera_request g_msg;
pthread_mutex_t g_camera_client_req_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_camera_client_req_cond = PTHREAD_COND_INITIALIZER;

/**
 * send msg to camera daemon
 *
 * @param socket the socket id
 * @param req A point to the request msg
 * @param block whether wait the implement result of camera daemon
 **/
static int send_msg(int socket, struct virtual_camera_request *req, bool block = false);
static int camera_client_device_open(int camera_id);
static void camera_client_device_close(int camera_id);
static int camera_client_device_config_streams(int camera_id, stream_config_t *stream_list);
static int camera_client_device_create_buffer(int camera_id);
static int camera_client_device_start(int camera_id);
static int camera_client_device_stop(int camera_id);
static int camera_client_device_allocate_memory(int camera_id, void *data);
static int camera_client_stream_qbuf(int camera_id, camera_buffer_t **buffer, int num_buffers, void *settings);
static int camera_client_stream_qbuf_slim(int camera_id, struct virtual_camera_request *req);
static int camera_client_stream_dqbuf_slim(int camera_id, struct virtual_camera_request *rsp);
static int camera_client_stream_on(int camera_id);
static int camera_client_stream_off(int camera_id);
static int camera_client_stream_dqbuf(int camera_id, int stream_id, camera_buffer_t **buffer, void *settings);
static int connect_devices(char *address, int port);

int send_msg(int sock, struct virtual_camera_request *req, bool block)
{
	int ret = 0;
	printf("send message  %d to server\n", req->type);

	if (send(sock, req, sizeof(struct virtual_camera_request), 0) < 0) {
		printf("Error sending data to server\n");
		return 1;
	}

	if (block) {
		pthread_mutex_lock(&g_camera_client_req_mutex);
		while (!((g_msg.type == req->type) && (g_msg.camera_id == req->camera_id))) {
			pthread_cond_wait(&g_camera_client_req_cond, &g_camera_client_req_mutex);
		};
		ret = (g_msg.type == VIRTUAL_CAMERA_RET_OK) ? 0 : 1;
		pthread_mutex_unlock(&g_camera_client_req_mutex);
	}

	printf("send message  %d to server return \n", ret);
	return ret;
}

class camera_client
{
public:
	camera_client(int camera_id);
	void data_notify(struct virtual_camera_request *req);
	void notify_register(vcamera_data_notify notify);
	int get_buffer(camera_buffer_t* buffer);
	int config_stream(int width,int height);
	int create_buffer();

	stream_config_t m_stream_list;
	stream_t m_stream;
	bool is_callback_available() { return m_data_notify != NULL; }

private:
	int m_fd;
	int m_camera_id;
	int m_buffer_number;
	int m_memory_size;
	void *m_shared_mem;
	vcamera_data_notify m_data_notify;
};

camera_client *g_camera_clients[g_max_camera_number];

camera_client::camera_client(int id)
	: m_fd(0),m_camera_id(id),m_buffer_number(g_buffer_count),m_memory_size(0),m_shared_mem(nullptr),
	m_data_notify(nullptr),m_stream{ 0 }
{

};

int camera_client::config_stream(int width,int height)
{
	m_stream.width = width;
	m_stream.height = height;
	m_stream.size = width * (height + 1) * 2;
};

/*Recommended to use callback mechanism to obtain the best performance*/
void camera_client::notify_register(vcamera_data_notify notify) { m_data_notify = notify; };

void camera_client::data_notify(struct virtual_camera_request *req)
{
	camera_buffer_t buffer = {0};

	buffer.index = req->buffer.index;
	get_buffer(&buffer);
	m_data_notify(m_camera_id, &buffer);
}

int camera_client::create_buffer()
{
	std::string memory_name = "camera_daemon_mem_" + std::to_string(m_camera_id);

	m_memory_size = ALIGN_UP(m_stream.size,getpagesize()) * g_buffer_count;
	m_fd = shm_open(memory_name.c_str(),O_RDWR | O_CREAT,0666);
	if (m_fd == -1) {
		printf("create_buffer %s failed\n", memory_name.c_str());
		return -1;
	} else if (ftruncate(m_fd, m_memory_size) == -1) {
		printf("%s ftruncate failed\n", __func__);
		return -1;
	}

	printf("%s open share memory success m_stream.size %d\n", __func__, m_stream.size);
	m_shared_mem = mmap(nullptr, m_memory_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
	printf("%s map memory m_shared_mem %p\n", __func__, m_shared_mem);
	return 0;
}

int camera_client::get_buffer(camera_buffer_t *buffer)
{
	printf("camera_client::%s Enter\n", __func__);

	if ((buffer->index >= 0) && (buffer->index < m_buffer_number))
	{
		buffer->addr = m_shared_mem + ALIGN_UP(m_stream.size,getpagesize()) * buffer->index;
		printf("camera_client::%s buffer->addr %p\n", __func__, buffer->addr);
	}

	return 0;
}

void vcamera_data_callback(int camera_id, vcamera_data_notify callback)
{
	g_camera_clients[camera_id]->notify_register(callback);
}

int camera_client_handle(struct virtual_camera_request *req)
{
	printf("camera_client_handle req->type %d\n", req->type);

	switch (req->type) {
	case VIRTUAL_CAMERA_GET_FORMAT:
	case VIRTUAL_CAMERA_SET_FORMAT:
	case VIRTUAL_CAMERA_TRY_FORMAT:
	case VIRTUAL_CAMERA_ENUM_FORMAT:
	case VIRTUAL_CAMERA_ENUM_SIZE:
	case VIRTUAL_CAMERA_CREATE_BUFFER:
	case VIRTUAL_CAMERA_DEL_BUFFER:
	case VIRTUAL_CAMERA_QBUF:
	case VIRTUAL_CAMERA_STREAM_ON:
	case VIRTUAL_CAMERA_STREAM_OFF:
	case VIRTUAL_CAMERA_OPEN:
	case VIRTUAL_CAMERA_CLOSE:
		pthread_mutex_lock(&g_camera_client_req_mutex);
		g_msg = *req;
		pthread_cond_broadcast(&g_camera_client_req_cond);
		pthread_mutex_unlock(&g_camera_client_req_mutex);
		break;
	case VIRTUAL_CAMERA_DQBUF:
		if (g_camera_clients[req->camera_id]->is_callback_available()) {
			g_camera_clients[req->camera_id]->data_notify(req);
		} else {
			pthread_mutex_lock(&g_camera_client_req_mutex);
			g_msg = *req;
			pthread_cond_broadcast(&g_camera_client_req_cond);
			pthread_mutex_unlock(&g_camera_client_req_mutex);
		}
	default:
		break;
	};

	return 0;
}

void *camera_client_thread(void *)
{
	struct virtual_camera_request msg = {0};
	int num = 0;

	printf("%s \n", __func__);

	do {
		printf("Wait message\n");
		num = read(g_socket, (char *)&msg, sizeof(msg));
		printf("Received message\n");

		if (num < 0) {
			printf("Failed to read from camera manager\n");
		} else if (num == 0) {
			printf("Camera Manager close socket\n");
			break;
		} else {
			printf("Received message, call camera_client_handle, type = %d\n", msg.type);
			camera_client_handle(&msg);
		}
	} while (1);

	return nullptr;
}

int camera_client_device_open(int camera_id)
{
	struct virtual_camera_request req = {0};
	struct virtual_camera_request rsp = {0};

	req.camera_id = camera_id;
	req.type = VIRTUAL_CAMERA_OPEN;

	return send_msg(g_socket, &req);
}

void camera_client_device_close(int camera_id)
{
	struct virtual_camera_request req = {0};
	struct virtual_camera_request rsp = {0};

	req.camera_id = camera_id;
	req.type = VIRTUAL_CAMERA_CLOSE;
	send_msg(g_socket, &req);
}

int camera_client_device_config_streams(int camera_id, stream_config_t *stream_list)
{
	struct virtual_camera_request req = {0};
	struct virtual_camera_request rsp = {0};

	g_camera_clients[camera_id]->config_stream(stream_list->streams[0].width,stream_list->streams[0].height);
	g_camera_clients[camera_id]->create_buffer();

	req.camera_id = camera_id;
	req.format.pixel_format_type = stream_list->streams[0].format;
	req.format.camera_format.height = stream_list->streams[0].height;
	req.format.camera_format.width = stream_list->streams[0].width;
	req.format.camera_format.sizeimage = stream_list->streams[0].size;
	req.format.camera_format.stride = stream_list->streams[0].width;

	req.type = VIRTUAL_CAMERA_SET_FORMAT;

	return send_msg(g_socket, &req);
}

int camera_client_device_start(int camera_id)
{
	struct virtual_camera_request req = {0};
	struct virtual_camera_request rsp = {0};

	req.camera_id = camera_id;
	req.type = VIRTUAL_CAMERA_STREAM_ON;

	return send_msg(g_socket, &req);
}

int camera_client_device_stop(int camera_id)
{
	struct virtual_camera_request req = {0};
	struct virtual_camera_request rsp = {0};

	req.camera_id = camera_id;
	req.type = VIRTUAL_CAMERA_STREAM_OFF;

	return send_msg(g_socket, &req);
}

int camera_client_device_allocate_memory(int camera_id, camera_buffer_t *buffer)
{
	struct virtual_camera_request req = {0};
	struct virtual_camera_request rsp = {0};

	req.camera_id = camera_id;
	req.type = VIRTUAL_CAMERA_CREATE_BUFFER;
	g_camera_clients[camera_id]->get_buffer(buffer);

	return send_msg(g_socket, &req);
}

int camera_client_stream_qbuf(int camera_id, struct virtual_camera_request *req)
{
	req->camera_id = camera_id;
	req->type = VIRTUAL_CAMERA_QBUF;

	printf("send message for type %d\n", req->type);
	printf("send message req->buffer.index %d\n", req->buffer.index);
	return send_msg(g_socket, req);
}

/*It's conflict with callback function*/
int camera_client_stream_dqbuf(int camera_id, struct virtual_camera_request *rsp)
{
	printf("%s Enter\n", __func__);

	pthread_mutex_lock(&g_camera_client_req_mutex);
	while (!((g_msg.type == VIRTUAL_CAMERA_DQBUF) && (g_msg.camera_id == camera_id))) {
		printf("wait for type %d\n", rsp->type);
		pthread_cond_wait(&g_camera_client_req_cond, &g_camera_client_req_mutex);
	};
	*rsp = g_msg;
	printf("camera_client_stream_dqbuf get index %d\n", rsp->buffer.index);
	g_msg.type = VIRTUAL_CAMERA_RET_INVALID;
	pthread_mutex_unlock(&g_camera_client_req_mutex);

	return 0;
}

int camera_client_stream_on(int camera_id)
{
	struct virtual_camera_request req = {0};
	struct virtual_camera_request rsp = {0};

	req.camera_id = camera_id;
	req.type = VIRTUAL_CAMERA_STREAM_ON;

	return send_msg(g_socket, &req);
}

int camera_client_stream_off(int camera_id)
{
	struct virtual_camera_request req = {0};
	struct virtual_camera_request rsp = {0};

	req.camera_id = camera_id;
	req.type = VIRTUAL_CAMERA_STREAM_OFF;

	return send_msg(g_socket, &req);
}

int connect_devices(char *address, int port)
{
	struct sockaddr_in serv_addr = {0};

	if ((g_socket = socket(AF_INET,SOCK_STREAM,0)) < 0)
	{
		printf("Error creating socket\n");
		return 1;
	}

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(port);
	serv_addr.sin_addr.s_addr = inet_addr(address);

	if (connect(g_socket, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		printf("Error connecting to server\n");
		return 1;
	}

	if (recv(g_socket, &g_response, sizeof(g_response), 0) >= 0) {
		printf("recv message, this means the camera ready!\n");
	}
	printf("Connected to server\n");

	return 0;
}

int get_physical_id(int camera_logical_id)
{
	return camera_logical_id; // TODO, this should get from a xml or other script
}

/**
 * Return the number of cameras
 * This should be called before any other calls
 *
 * @return > 0  return cameras number
 * @return == 0 failed to get cameras number
 **/
int vcamera_get_number_of_cameras()
{
	return 2; // TODO, this should get from a xml or other script
}

static int g_inited = 0;
/**
 * Initialize camera HAL
 *
 * @return error code
 **/
int vcamera_hal_init()
{
	int ret = 0;
	if (g_inited == 0) {
		std::string address = get_scenario_address_config();
		int port = get_scenario_port_config();

		ret = connect_devices(const_cast<char *>(address.c_str()), port);

		if (ret == 0) {
			int rc = 0;

			rc = pthread_create(&g_camera_tid, nullptr, camera_client_thread, nullptr);
			if (rc) {
				printf("Failed to create the camera_manager_handle.\n");
				return 0;
			};
		}

		for (int i = 0; i < g_max_camera_number; i++) {
			g_camera_clients[i] = new camera_client(i);
		}
		g_inited = 1;
	}
	return ret;
}

/**
 * De-Initialize camera HAL
 *
 * @return error code
 **/
int vcamera_hal_deinit()
{
	void *jval = nullptr;

	g_inited = 0;

	pthread_join(g_camera_tid, &jval);
	close(g_socket);

	for (int i = 0; i < g_max_camera_number; i++) {
		delete g_camera_clients[i];
	}

	return 0;
}

/**
 * Open one camera device
 *
 * @param camera_id camera index
 *
 * @return error code
 **/
int vcamera_device_open(int camera_id) { return camera_client_device_open(get_physical_id(camera_id)); }

/**
 * Close camera device
 *
 * @param camera_id The ID that opened before
 **/
void vcamera_device_close(int camera_id) { camera_client_device_close(get_physical_id(camera_id)); }

/**
 * Add stream to device
 *
 * @param camera_id The camera ID that was opened
 * @param stream_id
 * @param stream_conf stream configuration
 *
 * @return 0 succeed <0 error
 **/
int vcamera_device_config_streams(int camera_id, stream_config_t *stream_list)
{
	return camera_client_device_config_streams(get_physical_id(camera_id), stream_list);
}

/**
 * Start device
 *
 * Start all streams in device.
 *
 * @param camera_id The Caemra ID that opened before
 *
 * @return error code
 **/
int vcamera_device_start(int camera_id) { return camera_client_device_start(get_physical_id(camera_id)); }

/**
 * Stop device
 *
 * Stop all streams in device.
 *
 * @param camera_id The Caemra ID that opened before
 *
 * @return error code
 **/
int vcamera_device_stop(int camera_id) { return camera_client_device_stop(get_physical_id(camera_id)); }

/**
 * Allocate memory for mmap & dma export io-mode
 *
 * @param camera_id The camera ID that opened before
 * @param buffer stream buff
 *
 * @return error code
 **/
int vcamera_device_allocate_memory(int camera_id, camera_buffer_t *buffer)
{
	return camera_client_device_allocate_memory(get_physical_id(camera_id), buffer);
}

/**
 * Queue a buffer(or more buffers) to a stream
 *
 * @param camera_id The camera ID that opened before
 * @param buffer The array of pointers to the camera_buffer_t
 * @param num_buffers The number of buffers in the array
 *
 * @return error code
 **/
int vcamera_stream_qbuf(int camera_id, camera_buffer_t **buffer, int num_buffers, void *metadata)
{
	struct virtual_camera_request req = {0};

	req.buffer.index = buffer[0]->index;
	printf("vcamera_stream_qbuf get_index %d\n", req.buffer.index);
	return camera_client_stream_qbuf(camera_id, &req);
}

/**
 * Dequeue a buffer from a stream
 *
 * @param camera_id The camera ID that opened before
 * @param stream_id the stream ID that add to device before
 * @param camera_buff stream buff
 *
 * @return error code
 **/
int vcamera_stream_dqbuf(int camera_id, int stream_id, camera_buffer_t **buffer, void *metadata)
{
	int ret = 0;
	struct virtual_camera_request req = {0};

	req.type = VIRTUAL_CAMERA_DQBUF;
	ret = camera_client_stream_dqbuf(camera_id, &req);

	if (ret == 0) {
		buffer[0]->index = req.buffer.index;
		ret = g_camera_clients[camera_id]->get_buffer(buffer[0]);
	}

	return ret;
}

/**
 * get formats number
 *
 * @param camera_id The camera ID that opened before
 *
 * @return formats number
 **/
int vcamera_get_formats_number(int camera_id)
{
	/* All camera client use the same camera should use the same format */
	return 1;
}

/**
 * get formats
 *
 * @param camera_id The camera ID that opened before
 * @param p A point to return the stream_t list
 * @param streams_number the array size of stream_t list
 * @return error code
 **/
int vcamera_get_formats(int camera_id, stream_t *p, int *streams_number)
{
	/*TODO, get the format config by scenario*/
	p->width = TEST_WIDTH;
	p->height = TEST_HEIGHT;
	p->format = V4L2_PIX_FMT_NV12;

	return 0;
}
