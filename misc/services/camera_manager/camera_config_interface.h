#pragma once

#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
	int logical_id;
	int physical_id;
	int shared;
} camera_config_info;

typedef struct {
	int id;
	int width;
	int height;
	int format;
	interface_type type;
	char sensor_name[128];
	char devnode[128];
	/*If this physical camera was shared with other VMs, the driver have to use camera_client.so */
	char driver[128];
}physical_camera_info_c;

int get_vm_cameras_number(char* vm_name);
int get_vm_camera_config(char* vm_name,camera_config_info* info,int camera_id);
int get_physical_camera_config(physical_camera_info_c* info);
#ifdef __cplusplus
}
#endif
