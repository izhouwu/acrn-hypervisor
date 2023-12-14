/*
 * Copyright (C) 2019-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum {
	V4L2_INTERFACE = 0,
	HAL_INTERFACE = 1,
} interface_type;

#include "camera_config_interface.h"

int main(int argc, char **argv)
{
	char *vm_name = "VM1";
	int camera_id = 0;

	get_vm_cameras_number(vm_name);

	camera_config_info info;
	get_vm_camera_config(vm_name, &info, camera_id);

	physical_camera_info_c pinfo;
	pinfo.id = info.physical_id;
	get_physical_camera_config(&pinfo);

	return 0;
}
