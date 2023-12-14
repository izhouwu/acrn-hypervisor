/*
 * Copyright (C) 2018-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */
#pragma once

#include "camera_deserializer.h"
#include "camera_config.h"

class request_thread
{
}; // reserve

/**
 * The camera class is a abstract of SOC Camera and RAW Camera
 *
 * All cameras control camera hardware by a camera deserializer object,
 * and multiple camera objects could use the same deserializer object
 *
 * One camera object could be access by multiple virtual_camera object
 */
class camera : public camera_data_consumer,public camera_data_provider
{
public:
	camera(int camera_id);
	~camera();

	int qbuf(camera_buffer_t** buffer,int num_buffers,camera_data_consumer* listener,void* settings = NULL);
	int start();
	int stream_add() { return -1; }
	int stream_remove() { return -1; };
	camera_buffer_t* get_buffers(int stream_id);
	int get_frame_size(int format,int width,int height,int field,int* bpp);
	virtual int handle_data(camera_data* pdata);
	virtual void notify(camera_data* pdata);
private:
	int open();
	int close();
	int streams_init();
	int buffers_init();
	int buffers_uninit();
	int enqueue_all_buffers();
	void* m_shared_mem[MAX_STREAM_NUMBER];

	static std::mutex m_camera_deserializers_mutex;
	static std::shared_ptr<camera_deserializer> m_camera_deserializers[MAX_DESERIALIZER_NUMBER];
	std::shared_ptr<camera_deserializer> get_deserializer(int camera_id,const char* camera_lib);
	std::shared_ptr<camera_deserializer> m_camera_deserializer;

	/* buffer number for each stream*/
	int m_buffer_number;
	camera_buffer_t m_buffers[MAX_STREAM_NUMBER][MAX_CAMERA_BUFFER];
	/**
	 * A bufer have three states:
	 * INIT: buffer have not used by any client or driver.
	 * PENDING: Use QBUF send to driver and waiting driver to fill data
	 * USING: The buffer was using by clients
	 *
	 */
	enum buffer_state {
		INIT,
		PENDING,
		USING,
	};

	struct camera_buffer_info {
		buffer_state state;
		/**
		 * using_clients: record all user who is using this buffer current now.
		 */
		std::set<camera_data_consumer*> using_clients;
	};

	/**
	 * Each item of m_buffer_map will record the buffer address and the number of listeners
	 * "void*", the buffer address
	 * "int", the listener number which means the user number of these buffers
	 */
	std::map<void*,camera_buffer_info> m_buffer_map;
	std::mutex m_buffer_map_mutex;
	stream_t m_streams[MAX_STREAM_NUMBER];
	stream_config_t m_stream_list;
	int m_camera_id;
	physical_camera_info m_camera_info;
	std::string m_mem_name;
};
