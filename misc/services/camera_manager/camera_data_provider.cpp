/*
 * Copyright (C) 2018-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "camera_data_provider.h"

void camera_data_provider::register_consumer(camera_data_consumer *consumer)
{
	auto_lock l(m_consumers_mutex);

	m_consumers.insert(consumer);
	pr_info("camera_data_provider::%s After insert m_consumers size is %ld\n", __func__, m_consumers.size());
}

void camera_data_provider::remove_consumer(camera_data_consumer *consumer)
{
	auto_lock l(m_consumers_mutex);
	m_consumers.erase(consumer);
}

void camera_data_provider::notify(camera_data *pdata)
{
	auto_lock l(m_consumers_mutex);
	pr_info("camera_data_provider::%s Enter, m_consumers size is %ld\n", __func__, m_consumers.size());
	for (auto consumer : m_consumers) {
		pr_info("camera_data_provider::%s call handle_data\n", __func__);
		consumer->handle_data(pdata);
	}
}

int camera_data_consumer::handle_data(camera_data *pdata) { return 0; }
