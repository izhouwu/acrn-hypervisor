# Copyright (C) 2022 Intel Corporation.
#
# SPDX-License-Identifier: BSD-3-Clause
#

import re, os
import logging
import cppyy
import inspectorlib.camera as camera

from extractors.helpers import add_child, get_node, get_bdf_from_realpath
from collections import defaultdict
from pathlib import Path
from cppyy.gbl.std import vector

SYS_INPUT_DEVICES_CLASS_PATH = "/sys/class/input/"
SYS_TTY_DEVICES_CLASS_PATH = "/sys/class/tty/"
SYS_DISPLAYS_INFO_PATH = "/sys/class/drm/"

def add_child_with_file_contents(parent_node, tag, filepath, translations = {}):
    try:
        with open(filepath, "r") as f:
            res = f.read().strip()
            if res in translations.keys():
                add_child(parent_node, tag, translations[res])
            else:
                add_child(parent_node, tag, res)
    except Exception as e:
        logging.warning(f"Failed to read the data from {filepath}: {e}")

def get_input_ids():
    input_ids = list()
    root_regex = re.compile("input([0-9]+)")
    for root in filter(lambda x: x.startswith("input"), os.listdir(SYS_INPUT_DEVICES_CLASS_PATH)):
        m = root_regex.match(root)
        if m:
            input_ids.append(int(m.group(1)))
    return sorted(input_ids)

def extract_inputs(device_classes_node):
    inputs_node = add_child(device_classes_node, "inputs", None)
    input_ids = get_input_ids()
    for id in input_ids:
        input_node = add_child(inputs_node, "input", None)
        add_child_with_file_contents(input_node, "name", f"/sys/class/input/input{id}/name")
        add_child_with_file_contents(input_node, "phys", f"/sys/class/input/input{id}/phys")

def get_serial_devs():
    return sorted(filter(lambda x: x.startswith("ttyS"), os.listdir(SYS_TTY_DEVICES_CLASS_PATH)), key=lambda x:int(x[4:]))

def extract_ttys(device_classes_node):
    ttys_node = add_child(device_classes_node, "ttys", None)
    for serial_dev in get_serial_devs():
        serial_node = add_child(ttys_node, "serial")
        add_child(serial_node, "dev_path", f"/dev/{serial_dev}")
        add_child_with_file_contents(serial_node, "type", f"{SYS_TTY_DEVICES_CLASS_PATH}{serial_dev}/type")

def extract_display(board_etree):
    display_regex = re.compile("(card[0-9])-(DP|HDMI|VGA)-.*")
    display_types = {
        "DP": "DisplayPort (DP)",
        "HDMI": "High Definition Multimedia Interface (HDMI)",
        "VGA": "Video Graphics Array (VGA)",
    }
    display_ids = defaultdict(lambda: 0)
    for root in filter(lambda x: x.startswith("card"), os.listdir(SYS_DISPLAYS_INFO_PATH)):
        displays_path = SYS_DISPLAYS_INFO_PATH + root
        status_path = f"{displays_path}/status"
        device_path = f"{displays_path}/device/device"
        m = display_regex.match(root)
        if m:
            try:
                assert Path(status_path).exists(), \
                    f"{status_path} does not exist and connection status of {root} cannot be detected. Failed to add " \
                    f"{root} to board XML."
                assert Path(device_path).exists(), \
                    f"{device_path} does not exist and the graphics card which {root} is connected to cannot be detected. " \
                    f"Failed to add {root} to board XML"
                with open(f"{status_path}", "r") as f:
                    if f.read().strip() == "connected":
                        bus, device, function = \
                            get_bdf_from_realpath(f"{device_path}")
                        adr = hex((device << 16) + function)
                        bus_node = get_node(board_etree, f"//bus[@type='pci' and @address='{hex(bus)}']")
                        if bus_node is None:
                            devices_node = get_node(board_etree, "//devices")
                            bus_node = add_child(devices_node, "bus", type="pci", address=hex(bus))
                        device_node = get_node(bus_node, f"./device[@address='{adr}']")
                        if device_node is None:
                            device_node = add_child(bus_node, "device", None, address=adr)
                        bdf = (bus, device, function)
                        display_id = display_ids[bdf]
                        add_child(device_node, "display", f"{display_id}", type=display_types[m.group(2)])
                        display_ids[bdf] += 1
            except Exception as e:
                logging.warning(f"{e}")

def cameras_init():
    try:
        cppyy.include('ICamera.h')
        cppyy.include("Parameters.h")
        cppyy.load_library('libcamhal.so')
    except Exception as e:
        logging.warning(f"The file cannot be found in the system. {e}")
        return False
    cppyy.gbl.icamera.camera_hal_init()
    return True

def extract_cameras(device_classes_node):
    if not cameras_init():
        add_child(device_classes_node, "camera_list", "\n\t/* Camera data is not available */\n\t")
        return

    camera_number = cppyy.gbl.icamera.get_number_of_cameras()
    cameras_node = add_child(device_classes_node, "camera_list", None)

    stream_list = cppyy.gbl.icamera.stream_array_t()
    for index in range(0, camera_number):
        camera_node = add_child(cameras_node, "camera", None, id=f"{index}")
        stream_list_node = add_child(camera_node, "stream_list", None)

        result = cppyy.gbl.icamera.camera_device_open(index)
        info = cppyy.gbl.icamera.camera_info_t()
        result = cppyy.gbl.icamera.get_camera_info(index, info)

        info.capability.getSupportedStreamConfig(stream_list)
        for i in range(0, stream_list.size()):
            stream_size_node = add_child(stream_list_node, "stream_size", None, id=f"{i}")
            add_child(stream_size_node, "width", f"{stream_list[i].width}")
            add_child(stream_size_node, "height", f"{stream_list[i].height}")
            add_child(stream_size_node, "format", f"{camera.CAMERA_FORMATS_DB[hex(stream_list[i].format)]}")
        cppyy.gbl.icamera.camera_device_close(index)

def extract_topology(device_classes_node, board_etree):
    extract_inputs(device_classes_node)
    extract_ttys(device_classes_node)
    extract_cameras(device_classes_node)
    extract_display(board_etree)

def extract(args, board_etree):
    device_classes_node = get_node(board_etree, "//device-classes")
    extract_topology(device_classes_node, board_etree)
