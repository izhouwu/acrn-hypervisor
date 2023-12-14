/*
 * Copyright (C) 2018-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "camera_manager.h"

/*This module implement the APIs of vICamera which could be call by virtio camera or other App*/
#include "../../library/include/vICamera.h"

#pragma once

#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif

// TODO, the camera format and resolution should get from scenario
const int TEST_WIDTH = 1280;
const int TEST_HEIGHT = 720;

using namespace std;

/**
 * define the data callback interface
 *
 * @param camera_id The camera ID that opened before
 * @param buffer A point to return the camera buffer info
 **/
typedef void (*vcamera_data_notify)(int camera_id, camera_buffer_t *buffer);

#ifdef __cplusplus
}
#endif
