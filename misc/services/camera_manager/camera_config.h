#include <vector>
#pragma once

#include "camera_deserializer.h"
#include "camera_config_interface.h"

using namespace std;

struct deserializer {
	int id;
	vector<int> camera_ids;
};

struct vm_camera_info {
	int vm_id;
	std::string vm_name;
	vector<camera_config_info> camera_infos;
};

struct physical_camera_info {
	int id;
	int width;
	int height;
	int format;
	interface_type type;
	std::string sensor_name;
	std::string devnode;
	/*The native driver libary on the Service VM*/
	std::string driver;
};

struct camera_manager_info {
	int port;
	std::string address;
};

int get_virtual_cameras_config(vm_camera_info& vm_info);
int get_physical_camera_config(physical_camera_info& info);
int get_camera_manager_config(camera_manager_info& info);
