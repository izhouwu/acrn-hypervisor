/*
 * Copyright (C) 2018-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <ICamera.h>
#include <Parameters.h>
#include <signal.h>

#include "camera_manager.h"

using namespace std;
using namespace icamera;

static std::shared_ptr<camera> get_camera_instance(int camera_id);

std::shared_ptr<camera> g_handle[VIRTUAL_CAMERA_NUMQ] = {nullptr};
std::mutex g_handle_mutex;

class virtual_cameras *g_camera_clients[VIRTUAL_CAMERA_NUMQ];
struct virtual_vq_related g_camera_client_related[VIRTUAL_CAMERA_NUMQ];

virtual_cameras::virtual_cameras(int client_id,int socket) : m_index(0),m_state(CLIENT_CONNECT),
	m_closing(0),m_camera_number(0),m_client_id(client_id),m_socket(socket),m_thread(nullptr)
{
	get_camera_list();

	for (int i = 0; i < m_camera_number; i++) {
		m_cameras_info[i].instance = nullptr;//(m_cameras_info[i].id);
		m_cameras_info[i].state = STREAM_OFF;
		m_cameras_info[i].buffers = nullptr;
		pr_info("m_camera_ids[%d] physical id is %d, register_consumer, %p set buffers to NULL\n",
		        i,
		        m_cameras_info[i].id,
		        this);
	}

	try {
		m_thread = new std::thread(msg_thread, this);
		m_thread->detach();
		m_thread_tid = m_thread->get_id();
	} catch (const std::system_error &e) {
		throw std::runtime_error("Create msg_thread failed\n");
	}
};

virtual_cameras::~virtual_cameras()
{
	pr_info("virtual_cameras::~virtual_cameras");

	for (int camera_id = 0; camera_id < m_camera_number; camera_id++) {
		if ((m_cameras_info[camera_id].instance != nullptr) && (m_cameras_info[camera_id].state == STREAM_ON)) {
			m_cameras_info[camera_id].instance->remove_consumer(this);
			m_cameras_info[camera_id].state = STREAM_OFF;
			release_camera_buffer(camera_id);
		}
	}

	delete m_thread;
};

int virtual_cameras::get_camera_list()
{
	vm_camera_info vm_info = {0};
	vm_info.vm_id = 1;

	// TODO, get vm name from client
	vm_info.vm_name = "VM" + std::to_string(m_client_id);
	get_virtual_cameras_config(vm_info);

	m_camera_number = vm_info.camera_infos.size();
	pr_info("%s m_camera_number is %d \n", vm_info.vm_name.c_str(), m_camera_number);

	for (int i = 0; i < vm_info.camera_infos.size(); i++) {
		int index = vm_info.camera_infos[i].logical_id;
		m_cameras_info[index].id = vm_info.camera_infos[i].physical_id;
		pr_info("%s Camera[%d] physical id is %d, shared = %d\n",
		        vm_info.vm_name.c_str(),
		        vm_info.camera_infos[i].logical_id,
		        vm_info.camera_infos[i].physical_id,
		        vm_info.camera_infos[i].shared);
	}

	return 0;
}

int virtual_cameras::get_virtual_camera_id(int physical_camera_id)
{
	for (int i = 0; i < m_camera_number; i++) {
		if (m_cameras_info[i].id == physical_camera_id)
			return i;
	}

	return -1;
}

int virtual_cameras::get_stream_id(struct virtual_camera_request *req)
{
	// We use the stream 0 only now.
	return 0;
}

void *virtual_cameras::msg_thread(virtual_cameras *p)
{
	int num = 0;
	struct virtual_camera_request msg = {0};

	pr_info("%s client_id %d start p->m_socket %d\n", __func__, p->m_client_id, p->m_socket);

	do {
		pr_info("Wait message\n");
		num = read(p->m_socket, (char *)&msg, sizeof(msg));

		pr_info("Received message\n");

		if (num < 0) {
			pr_info("Failed to read from client\n");
			break;
		} else if (num == 0) {
			pr_info("Camera Client %d close socket\n", p->m_client_id);

			/* send msg to camera manager to close this client*/
			p->m_state = CLIENT_DISCONNECT;
			camera_client_state_request req;
			req.client_id = p->m_client_id;
			req.state = CLIENT_DISCONNECT;
			camera_manager::get_instance().push_msg(req);

			break;
		} else {
			if (p->handle_msg(&msg) == -1)
				break;
			pr_info("Received message, call handle_msg, type = %d\n", msg.type);
		}
	} while (p->m_state == CLIENT_CONNECT);

	return nullptr;
}

void virtual_cameras::release_camera_buffer(int camera_id)
{
	camera_buffer_t *buf = m_cameras_info[camera_id].buffers;

	for (int i = 0; (i < g_buffer_count && buf != nullptr); i++,buf++) {
		buf->sequence = -1;
		buf->timestamp = 0;
		buf->index = i;

		pr_info("release_camera_buffer camera %d, client %d m_buffers[%d][%d] %p index %d\n",
		        camera_id,
		        m_socket,
		        camera_id,
		        i,
		        m_cameras_info[camera_id].buffers[i].addr,
		        m_cameras_info[camera_id].buffers[i].index);
		m_cameras_info[camera_id].instance->qbuf(&buf, 1, this);
	}
}

int virtual_cameras::handle_msg(struct virtual_camera_request *req)
{
	int ret = 0;
	int camera_id = req->camera_id;
	int stream_id = -1;
	int num_streams = 2;

	int bpp = 0;
	int buffer_size = 0;
	camera_buffer_t buffer = {0};
	camera_buffer_t *buf = nullptr;

	stream_t stream = {0};

	pr_info("Camera Manager req->type  %d\n", req->type);
	switch (req->type) {
	case VIRTUAL_CAMERA_GET_FORMAT:
		break;
	case VIRTUAL_CAMERA_SET_FORMAT:
		pr_info("format.pixel_format_type is: %d \n", req->format.pixel_format_type);
		pr_info("format.camera_format.height is: %d \n", req->format.camera_format.height);
		pr_info("format.width is: %d \n", req->format.camera_format.width);
		pr_info("format.camera_format.sizeimage is: %d \n", req->format.camera_format.sizeimage);
		pr_info("format.camera_format.stride is: %d \n", req->format.camera_format.stride);

		stream_id = get_stream_id(req);
		if (stream_id < 0) {
			pr_info("virtio_camera VIRTIO_CAMERA_SET_FORMAT failed\n");
			ret = -1;
		}
		break;

	case VIRTUAL_CAMERA_TRY_FORMAT:
		pr_info("format.pixel_format_type is: %d \n", req->format.pixel_format_type);
		pr_info("format.camera_format.height is: %d \n", req->format.camera_format.height);
		pr_info("format.width is: %d \n", req->format.camera_format.width);
		pr_info("format.camera_format.sizeimage is: %d \n", req->format.camera_format.sizeimage);
		pr_info("format.camera_format.stride is: %d \n", req->format.camera_format.stride);

		stream_id = get_stream_id(req);
		if (stream_id < 0) {
			pr_info("virtio_camera VIRTIO_CAMERA_SET_FORMAT failed\n");
			ret = -1;
		}
		break;
	case VIRTUAL_CAMERA_ENUM_FORMAT:
		break;

	case VIRTUAL_CAMERA_ENUM_SIZE:
		break;
	case VIRTUAL_CAMERA_CREATE_BUFFER: // TODO, align HAL and V4L2 interface
		if (m_cameras_info[camera_id].instance == nullptr)
		{
			int i = camera_id;
			m_cameras_info[i].instance = get_camera_instance(m_cameras_info[i].id);
			m_cameras_info[i].state = STREAM_OFF;
			m_cameras_info[i].buffers = nullptr;
			pr_info("m_camera_ids[%d] physical id is %d, register_consumer, %p set buffers to NULL\n",
				i,
				m_cameras_info[i].id,
				this);
		}

		if (m_cameras_info[camera_id].buffers == nullptr)
		{
			pr_info("create buffer for camera %d, virtual_cameras client %d buffer count %d\n",
			        camera_id,
			        m_socket,
				g_buffer_count);
#if 0   //The IPU HAL will 
			m_cameras_info[camera_id].buffers =
			    (camera_buffer_t *)malloc(sizeof(camera_buffer_t) * g_buffer_count);
			buf = m_cameras_info[camera_id].instance->get_buffers(get_stream_id(req));
			m_cameras_info[camera_id].buffers =
				m_cameras_info[camera_id].instance->get_buffers(get_stream_id(req));
			pr_info("create buffer for camera %d, virtual_cameras client %d buffer %p\n",
				camera_id,
				m_socket,
				buf);
			for (int i = 0; i < g_buffer_count; i++) {
				m_cameras_info[camera_id].buffers[i] = buf[i];
				pr_info("create buffer for camera %d, virtual_cameras client %d m_buffers[%d][%d] %p\n",
				        camera_id,
				        m_socket,
				        camera_id,
				        i,
				        m_cameras_info[camera_id].buffers[i].addr);
			}
#else
			m_cameras_info[camera_id].buffers =
				m_cameras_info[camera_id].instance->get_buffers(get_stream_id(req));
			pr_info("create buffer for camera %d, virtual_cameras client %d buffer %p\n",
				camera_id,
				m_socket,
				buf);
#endif
		} else
		{
			pr_info("%p buffers have been created for camera %d, virtual_cameras client %d buffers %p\n",
			        this,
			        camera_id,
			        m_socket,
			        m_cameras_info[camera_id].buffers);
		}
		break;
	case VIRTUAL_CAMERA_DEL_BUFFER:
		pr_info("memset m_buffers client %d camera %d\n",m_socket,camera_id);
		m_cameras_info[camera_id].buffers = nullptr;
		//memset(m_cameras_info[camera_id].buffers,0,sizeof(camera_buffer_t) * g_buffer_count);
		break;

		/* we will send buffer to client and skip the dq requese*/
	case VIRTUAL_CAMERA_DQBUF:
		break;

	case VIRTUAL_CAMERA_QBUF:
		buf = &m_cameras_info[camera_id].buffers[req->buffer.index];
		for (int i = 0; i < g_buffer_count; i++) {
			pr_info("VIRTUAL_CAMERA_QBUF before qbuf:camera %d, client %d m_buffers[%d][%d] %p index %d\n",
			        camera_id,
			        m_socket,
			        camera_id,
			        i,
			        m_cameras_info[camera_id].buffers[i].addr,
			        m_cameras_info[camera_id].buffers[i].index);
		}
		buf->sequence = -1;
		buf->timestamp = 0;
		buf->index = req->buffer.index;

		pr_info("VIRTUAL_CAMERA_QBUF before qbuf:camera %d Client %d qbuf[%d] %p\n",
		        camera_id,
		        m_socket,
		        req->buffer.index,
		        buf->addr);

		// processing data with buf
		m_cameras_info[camera_id].instance->qbuf(&buf, 1, this);

		for (int i = 0; i < g_buffer_count; i++) {
			pr_info("VIRTUAL_CAMERA_QBUF after qbuf:camera %d, client %d m_buffers[%d][%d] %p index %d\n",
			        camera_id,
			        m_socket,
			        camera_id,
			        i,
			        m_cameras_info[camera_id].buffers[i].addr,
			        m_cameras_info[camera_id].buffers[i].index);
		}
		m_index++;

		break;
	case VIRTUAL_CAMERA_STREAM_ON:
		ret = m_cameras_info[camera_id].instance->start();
		pr_info("Camera Manager VIRTUAL_CAMERA_STREAM_ON ret\n");
		m_cameras_info[camera_id].instance->register_consumer(this);
		m_cameras_info[camera_id].state = STREAM_ON;
		break;
	case VIRTUAL_CAMERA_STREAM_OFF:
		m_cameras_info[camera_id].instance->remove_consumer(this);
		m_cameras_info[camera_id].state = STREAM_OFF;
		release_camera_buffer(camera_id);

		pr_info("Camera Manager VIRTUAL_CAMERA_STREAM_OFF \n");
		break;
	case VIRTUAL_CAMERA_OPEN:
		pr_info("Camera Manager VIRTUAL_CAMERA_OPEN\n");
		break;
	case VIRTUAL_CAMERA_CLOSE:
		pr_info("Camera Manager VIRTUAL_CAMERA_CLOSE\n");
		break;
	default:
		ret = -1;
		break;
	};

	return ret;
}

int virtual_cameras::fill_camera_request(virtual_camera_request *rsp, void *p)
{
	rsp->type = VIRTUAL_CAMERA_DQBUF;

	for (int i = 0; i < m_camera_number; i++)
	{
		if (m_cameras_info[i].buffers == NULL)
			continue;
		pr_info("fill_camera_request for m_buffers[%d] = %p \n", i, m_cameras_info[i].buffers);
		for (int j = 0; j < g_buffer_count; j++) {
			pr_info("fill_camera_request for client %d m_buffers[%d][%d].addr = %p \n",
			        m_socket,
			        i,
			        j,
			        m_cameras_info[i].buffers[j].addr);
			if (m_cameras_info[i].buffers[j].addr == p) {
				rsp->camera_id = i;
				rsp->buffer.index = j;
				return 0;
			}
		}
	}

	return -1;
}

int virtual_cameras::handle_data(camera_data *pdata)
{
	int ret = 0;
	virtual_camera_request rsp = {0};

	pr_info("Camera Manager virtual_cameras::handle_data address %p\n", pdata->request.m_buffers[0].addr);
	for (int i = 0; i < pdata->request.m_buffer_number; i++) {
		virtual_camera_request rsp;

		ret = fill_camera_request(&rsp, pdata->request.m_buffers[i].addr);

		pr_info("Camera Manager send the buffer_index %d\n", rsp.buffer.index);
		pr_info("Camera Manager send the pdata->request.m_buffers[%d]->index %d\n",
		        i,
		        pdata->request.m_buffers[i].index);

		if (m_state == CLIENT_CONNECT) {
			send(m_socket, &rsp, sizeof(struct virtual_camera_request), 0);
		}
	}
	return 0;
}

static std::shared_ptr<camera> get_camera_instance(int camera_id)
{
	std::unique_lock<std::mutex> lock(g_handle_mutex);

	if ((camera_id >= 0) && (camera_id < VIRTUAL_CAMERA_NUMQ)) {
		pr_info("Camera Manager create instance\n");
		if (!g_handle[camera_id]) {
			std::shared_ptr<camera> sp(new camera(camera_id));
			g_handle[camera_id] = sp;
			pr_info("Camera Manager create instance for camera %d\n", camera_id);
		}
		return g_handle[camera_id];
	} else {
		return nullptr;
	}
}

static void remove_camera_instance(int camera_id)
{
	std::unique_lock<std::mutex> lock(g_handle_mutex);

	if ((camera_id >= 0) && (camera_id < VIRTUAL_CAMERA_NUMQ)) {
		pr_info("Camera Manager remove instance\n");
		if (!g_handle[camera_id]) {
			pr_info("Camera Manager remove instance for camera %d\n", camera_id);
			g_handle[camera_id].reset();
			g_handle[camera_id] = nullptr;
		}
	}
}

int camera_manager::client_control(camera_client_state_request &req)
{
	pr_info("camera_manager::%s Enter, client_id %d, state %d\n", __func__, req.client_id, req.state);

	switch (req.state) {
	case CLIENT_CONNECT:
		pr_info("camera_manager::%s CLIENT_CONNECT \n", __func__);
		break;
	case CLIENT_DISCONNECT:
		pr_info("camera_manager::%s CLIENT_DISCONNECT %p \n", __func__, g_camera_clients[req.client_id]);
		delete g_camera_clients[req.client_id];
		g_camera_clients[req.client_id] = nullptr;
		break;
	default:
		break;
	}

	return 0;
}

void *camera_manager::process_handle(camera_manager *p)
{
	struct virtual_camera_request req = {0};

	pr_info("camera_manager::process_handle start\n");
	do {
		std::unique_lock<std::mutex> lock(p->m_msg_mutex);

		pr_info("camera_manager::process_handle wait states request\n");
		while (p->m_msg.empty())
			p->m_msg_signal.wait(lock);
		pr_info("camera_manager::process_handle get msg\n");

		p->client_control(p->m_msg.front());
		p->m_msg.pop();
	} while (1);

	return nullptr;
}

int camera_manager::init()
{
	try {
		m_process_thread = new std::thread(process_handle, this);
	} catch (const std::system_error &e) {
		perror("Cameram manager create thread failed\n");
		exit(EXIT_FAILURE);
	}

	return 0;
}

void camera_manager::push_msg(camera_client_state_request &req)
{
	std::unique_lock<std::mutex> lock(m_msg_mutex);
	m_msg.push(req);
	m_msg_signal.notify_one();
}

void rsp_return(int socket, virtual_camera_request_type type)
{
	virtual_camera_request rsp = {0};

	rsp.type = type;

	pr_info("%s Camera Manager send rsp for client %d ret %d\n", __func__, socket, rsp.type);
	send(socket, &rsp, sizeof(struct virtual_camera_request), 0);
}

void handle_pipe(int sig) { pr_info("get the single sig %d\n", sig); }

int get_cmd_port_config(char *opts)
{
	char *opt = nullptr;
	int port = DEFAULT_PORT;

	if (opts != NULL) {
		opt = opts;
		if (!strncmp(opt, "port", 4)) {
			(void)strsep(&opt, "=");
			if (opt != NULL) {
				pr_info("%s atoi(opt) = %d\n", __func__, atoi(opt));
				port = atoi(opt);
			}
		}
	}
	return port;
}

int main(int argc, char *argv[])
{
	int server_socket = 0;
	struct sockaddr_in server_addr = {0};
	int client = 0;
	int port_config = 0;
	int camera_client_count = 0;
	std::thread *pthread = nullptr;

	struct sigaction action = {0};
	action.sa_handler = handle_pipe;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(SIGPIPE, &action, NULL);

	camera_manager::get_instance().init();

	if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
		perror("socket failed");
		exit(EXIT_FAILURE);
	}

	port_config = (argc > 1) ? get_cmd_port_config(argv[1]) : get_scenario_port_config();

	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(port_config);

	if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		perror("bind failed");
		close(server_socket);
		exit(EXIT_FAILURE);
	}

	if (listen(server_socket, 3) < 0) {
		perror("listen failed");
		close(server_socket);
		exit(EXIT_FAILURE);
	}

	pr_info("Waiting for connections...\n");
	while (1) {
		if ((client = accept(server_socket, NULL, NULL)) < 0) {
			break;
		} else {
			std::cout << "new connected: " << socket << std::endl;
			try {
				g_camera_clients[camera_client_count] =
				    new virtual_cameras(camera_client_count, client);
			} catch (const std::runtime_error &e) {
				pr_info("Create virtual_cameras failed: %s, the Camera Manager will Exit!\n", e.what());
				rsp_return(client, VIRTUAL_CAMERA_RET_INVALID);
			}
			rsp_return(client, VIRTUAL_CAMERA_RET_OK);
			camera_client_count++;
		}
	}

	for (int i = 0; i < VIRTUAL_CAMERA_NUMQ; i++)
		remove_camera_instance(i);

	close(server_socket);
	pr_info("Camera Manager Exit\n");
}
