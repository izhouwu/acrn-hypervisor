/*
 * Copyright (C) 2018-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#pragma once

#include <Parameters.h>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using namespace icamera;

#define ALIGN_UP(x, align) (((x) + ((align)-1)) & ~((align)-1))
#define pr_info printf

const int MAX_STREAM_NUMBER = 5;

typedef std::lock_guard<std::mutex> auto_lock;
typedef std::unique_lock<std::mutex> condition_lock;

struct camera_request {
	int m_camera_id;
	int m_request_id;
	int m_buffer_number;
	camera_buffer_t m_buffers[MAX_STREAM_NUMBER];
};

struct camera_data {
	int id;
	int event;
	camera_request request;
	int reserve;
};

class camera_data_consumer
{
public:
	camera_data_consumer() {};
	virtual ~camera_data_consumer() {};
	virtual int handle_data(camera_data* pdata);
};

class camera_data_provider
{
public:
	camera_data_provider(){};
	virtual ~camera_data_provider(){};
	virtual void register_consumer(camera_data_consumer *consumer);
	virtual void remove_consumer(camera_data_consumer *consumer);
	virtual void notify(camera_data* pdata);
protected:
	std::set<camera_data_consumer *> m_consumers;
	std::mutex m_consumers_mutex;
};
