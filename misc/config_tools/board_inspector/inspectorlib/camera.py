#!/usr/bin/env python3
#
# Copyright (C) 2023 Intel Corporation.
#
# SPDX-License-Identifier: BSD-3-Clause
#

import struct
import logging

def v4l2_fourcc(char1, char2, char3, char4):
    binary_data = struct.pack('4B', ord(char4), ord(char3), ord(char2), ord(char1))

    bytes_data = [binary_data[i:i+8].hex() for i in range(0, len(binary_data), 8)]
    return '0x' + bytes_data[0]

def v4l2_fourcc_be(char1, char2, char3, char4):
    binary_data = struct.pack('4B', ord(char4), ord(char3), ord(char2), ord(char1))
    bytes_data = [binary_data[i:i+8].hex() for i in range(0, len(binary_data), 8)]

    return hex(int(bytes_data[0], 16) | (1 << 31))

CAMERA_FORMATS_DB = {
    # RGB formats (1 or 2 bytes per pixel)
    v4l2_fourcc('R', 'G', 'B', '1'): "V4L2_PIX_FMT_RGB332",  # 8  RGB-3-3-2
    v4l2_fourcc('R', '4', '4', '4'): "V4L2_PIX_FMT_RGB444",  # 16  xxxxrrrr ggggbbbb
    v4l2_fourcc('A', 'R', '1', '2'): "V4L2_PIX_FMT_ARGB444",  # 16  aaaarrrr ggggbbbb
    v4l2_fourcc('X', 'R', '1', '2'): "V4L2_PIX_FMT_XRGB444",  # 16  xxxxrrrr ggggbbbb
    v4l2_fourcc('R', 'A', '1', '2'): "V4L2_PIX_FMT_RGBA444",  # 16  rrrrgggg bbbbaaaa
    v4l2_fourcc('R', 'X', '1', '2'): "V4L2_PIX_FMT_RGBX444",  # 16  rrrrgggg bbbbxxxx
    v4l2_fourcc('A', 'B', '1', '2'): "V4L2_PIX_FMT_ABGR444",  # 16  aaaabbbb ggggrrrr
    v4l2_fourcc('X', 'B', '1', '2'): "V4L2_PIX_FMT_XBGR444",  # 16  xxxxbbbb ggggrrrr
    v4l2_fourcc('G', 'A', '1', '2'): "V4L2_PIX_FMT_BGRA444",  # 16  bbbbgggg rrrraaaa
    v4l2_fourcc('B', 'X', '1', '2'): "V4L2_PIX_FMT_BGRX444",  # 16  bbbbgggg rrrrxxxx
    v4l2_fourcc('R', 'G', 'B', 'O'): "V4L2_PIX_FMT_RGB555",  # 16  RGB-5-5-5
    v4l2_fourcc('A', 'R', '1', '5'): "V4L2_PIX_FMT_ARGB555",  # 16  ARGB-1-5-5-5
    v4l2_fourcc('X', 'R', '1', '5'): "V4L2_PIX_FMT_XRGB555",  # 16  XRGB-1-5-5-5
    v4l2_fourcc('R', 'A', '1', '5'): "V4L2_PIX_FMT_RGBA555",  # 16  RGBA-5-5-5-1
    v4l2_fourcc('R', 'X', '1', '5'): "V4L2_PIX_FMT_RGBX555",  # 16  RGBX-5-5-5-1
    v4l2_fourcc('A', 'B', '1', '5'): "V4L2_PIX_FMT_ABGR555",  # 16  ABGR-1-5-5-5
    v4l2_fourcc('X', 'B', '1', '5'): "V4L2_PIX_FMT_XBGR555",  # 16  XBGR-1-5-5-5
    v4l2_fourcc('B', 'A', '1', '5'): "V4L2_PIX_FMT_BGRA555",  # 16  BGRA-5-5-5-1
    v4l2_fourcc('B', 'X', '1', '5'): "V4L2_PIX_FMT_BGRX555",  # 16  BGRX-5-5-5-1
    v4l2_fourcc('R', 'G', 'B', 'P'): "V4L2_PIX_FMT_RGB565",  # 16  RGB-5-6-5
    v4l2_fourcc('R', 'G', 'B', 'Q'): "V4L2_PIX_FMT_RGB555X",  # 16  RGB-5-5-5 BE
    v4l2_fourcc_be('A', 'R', '1', '5'): "V4L2_PIX_FMT_ARGB555X",  # 16  ARGB-5-5-5 BE
    v4l2_fourcc_be('X', 'R', '1', '5'): "V4L2_PIX_FMT_XRGB555X",  # 16  XRGB-5-5-5 BE
    v4l2_fourcc('R', 'G', 'B', 'R'): "V4L2_PIX_FMT_RGB565X",  # 16  RGB-5-6-5 BE

    # RGB formats (3 or 4 bytes per pixel)
    v4l2_fourcc('B', 'G', 'R', 'H'): "V4L2_PIX_FMT_BGR666",  # 18  BGR-6-6-6
    v4l2_fourcc('B', 'G', 'R', '3'): "V4L2_PIX_FMT_BGR24",  # 24  BGR-8-8-8
    v4l2_fourcc('R', 'G', 'B', '3'): "V4L2_PIX_FMT_RGB24",  # 24  RGB-8-8-8
    v4l2_fourcc('B', 'G', 'R', '4'): "V4L2_PIX_FMT_BGR32",  # 32  BGR-8-8-8-8
    v4l2_fourcc('A', 'R', '2', '4'): "V4L2_PIX_FMT_ABGR32",  # 32  BGRA-8-8-8-8
    v4l2_fourcc('X', 'R', '2', '4'): "V4L2_PIX_FMT_XBGR32",  # 32  BGRX-8-8-8-8
    v4l2_fourcc('R', 'A', '2', '4'): "V4L2_PIX_FMT_BGRA32",  # 32  ABGR-8-8-8-8
    v4l2_fourcc('R', 'X', '2', '4'): "V4L2_PIX_FMT_BGRX32",  # 32  XBGR-8-8-8-8
    v4l2_fourcc('R', 'G', 'B', '4'): "V4L2_PIX_FMT_RGB32",  # 32  RGB-8-8-8-8
    v4l2_fourcc('A', 'B', '2', '4'): "V4L2_PIX_FMT_RGBA32",  # 32  RGBA-8-8-8-8
    v4l2_fourcc('X', 'B', '2', '4'): "V4L2_PIX_FMT_RGBX32",  # 32  RGBX-8-8-8-8
    v4l2_fourcc('B', 'A', '2', '4'): "V4L2_PIX_FMT_ARGB32",  # 32  ARGB-8-8-8-8
    v4l2_fourcc('B', 'X', '2', '4'): "V4L2_PIX_FMT_XRGB32",  # 32  XRGB-8-8-8-8

    # Grey formats
    v4l2_fourcc('G', 'R', 'E', 'Y'): "V4L2_PIX_FMT_GREY",  # 8  Greyscale
    v4l2_fourcc('Y', '0', '4', ' '): "V4L2_PIX_FMT_Y4",  # 4  Greyscale
    v4l2_fourcc('Y', '0', '6', ' '): "V4L2_PIX_FMT_Y6",  # 6  Greyscale
    v4l2_fourcc('Y', '1', '0', ' '): "V4L2_PIX_FMT_Y10",  # 10  Greyscale
    v4l2_fourcc('Y', '1', '2', ' '): "V4L2_PIX_FMT_Y12",  # 12  Greyscale
    v4l2_fourcc('Y', '1', '4', ' '): "V4L2_PIX_FMT_Y14",  # 14  Greyscale
    v4l2_fourcc('Y', '1', '6', ' '): "V4L2_PIX_FMT_Y16",  # 16  Greyscale
    v4l2_fourcc_be('Y', '1', '6', ' '): "V4L2_PIX_FMT_Y16_BE",  # 16  Greyscale BE

    # Grey bit-packed formats
    v4l2_fourcc('Y', '1', '0', 'B'): "V4L2_PIX_FMT_Y10BPACK",  # 10  Greyscale bit-packed
    v4l2_fourcc('Y', '1', '0', 'P'): "V4L2_PIX_FMT_Y10P",  # 10  Greyscale, MIPI RAW10 packed

    # Palette formats
    v4l2_fourcc('P', 'A', 'L', '8'): "V4L2_PIX_FMT_PAL8",  # 8  8-bit palette

    # Chrominance formats
    v4l2_fourcc('U', 'V', '8', ' '): "V4L2_PIX_FMT_UV8",  # 8  UV 4:4

    # Luminance+Chrominance formats
    v4l2_fourcc('Y', 'U', 'Y', 'V'): "V4L2_PIX_FMT_YUYV",  # 16  YUV 4:2:2
    v4l2_fourcc('Y', 'Y', 'U', 'V'): "V4L2_PIX_FMT_YYUV",  # 16  YUV 4:2:2
    v4l2_fourcc('Y', 'V', 'Y', 'U'): "V4L2_PIX_FMT_YVYU",  # 16 YVU 4:2:2
    v4l2_fourcc('U', 'Y', 'V', 'Y'): "V4L2_PIX_FMT_UYVY",  # 16  YUV 4:2:2
    v4l2_fourcc('V', 'Y', 'U', 'Y'): "V4L2_PIX_FMT_VYUY",  # 16  YUV 4:2:2
    v4l2_fourcc('Y', '4', '1', 'P'): "V4L2_PIX_FMT_Y41P",  # 12  YUV 4:1:1
    v4l2_fourcc('Y', '4', '4', '4'): "V4L2_PIX_FMT_YUV444",  # 16  xxxxyyyy uuuuvvvv
    v4l2_fourcc('Y', 'U', 'V', 'O'): "V4L2_PIX_FMT_YUV555",  # 16  YUV-5-5-5
    v4l2_fourcc('Y', 'U', 'V', 'P'): "V4L2_PIX_FMT_YUV565",  # 16  YUV-5-6-5
    v4l2_fourcc('Y', 'U', 'V', '3'): "V4L2_PIX_FMT_YUV24",  # 24  YUV-8-8-8
    v4l2_fourcc('Y', 'U', 'V', '4'): "V4L2_PIX_FMT_YUV32",  # 32  YUV-8-8-8-8
    v4l2_fourcc('A', 'Y', 'U', 'V'): "V4L2_PIX_FMT_AYUV32",  # 32  AYUV-8-8-8-8
    v4l2_fourcc('X', 'Y', 'U', 'V'): "V4L2_PIX_FMT_XYUV32",  # 32  XYUV-8-8-8-8
    v4l2_fourcc('V', 'U', 'Y', 'A'): "V4L2_PIX_FMT_VUYA32",  # 32  VUYA-8-8-8-8
    v4l2_fourcc('V', 'U', 'Y', 'X'): "V4L2_PIX_FMT_VUYX32",  # 32  VUYX-8-8-8-8
    v4l2_fourcc('M', '4', '2', '0'): "V4L2_PIX_FMT_M420",  # 12  YUV 4:2:0 2 lines y, 1 line uv interleaved

    # two planes -- one Y, one Cr + Cb interleaved
    v4l2_fourcc('N', 'V', '1', '2'): "V4L2_PIX_FMT_NV12",  # 12  Y/CbCr 4:2:0
    v4l2_fourcc('N', 'V', '2', '1'): "V4L2_PIX_FMT_NV21",  # 12  Y/CrCb 4:2:0
    v4l2_fourcc('N', 'V', '1', '6'): "V4L2_PIX_FMT_NV16",  # 16  Y/CbCr 4:2:2
    v4l2_fourcc('N', 'V', '6', '1'): "V4L2_PIX_FMT_NV61",  # 16  Y/CrCb 4:2:2
    v4l2_fourcc('N', 'V', '2', '4'): "V4L2_PIX_FMT_NV24",  # 24  Y/CbCr 4:4:4
    v4l2_fourcc('N', 'V', '4', '2'): "V4L2_PIX_FMT_NV42",  # 24  Y/CrCb 4:4:4
    v4l2_fourcc('H', 'M', '1', '2'): "V4L2_PIX_FMT_HM12",  # 8  YUV 4:2:0 16x16 macroblocks

    # two non contiguous planes - one Y, one Cr + Cb interleaved
    v4l2_fourcc('N', 'M', '1', '2'): "V4L2_PIX_FMT_NV12M",  # 12  Y/CbCr 4:2:0
    v4l2_fourcc('N', 'M', '2', '1'): "V4L2_PIX_FMT_NV21M",  # 21  Y/CrCb 4:2:0
    v4l2_fourcc('N', 'M', '1', '6'): "V4L2_PIX_FMT_NV16M",  # 16  Y/CbCr 4:2:2
    v4l2_fourcc('N', 'M', '6', '1'): "V4L2_PIX_FMT_NV61M",  # 16  Y/CrCb 4:2:2
    v4l2_fourcc('T', 'M', '1', '2'): "V4L2_PIX_FMT_NV12MT",  # 12  Y/CbCr 4:2:0 64x32 macroblocks
    v4l2_fourcc('V', 'M', '1', '2'): "V4L2_PIX_FMT_NV12MT_16X16",  # 12  Y/CbCr 4:2:0 16x16 macroblocks

    # three planes - Y Cb, Cr
    v4l2_fourcc('Y', 'U', 'V', '9'): "V4L2_PIX_FMT_YUV410",  # 9  YUV 4:1:0
    v4l2_fourcc('Y', 'V', 'U', '9'): "V4L2_PIX_FMT_YVU410",  # 9  YVU 4:1:0
    v4l2_fourcc('4', '1', '1', 'P'): "V4L2_PIX_FMT_YUV411P",  # 12  YVU411 planar
    v4l2_fourcc('Y', 'U', '1', '2'): "V4L2_PIX_FMT_YUV420",  # 12  YUV 4:2:0
    v4l2_fourcc('Y', 'V', '1', '2'): "V4L2_PIX_FMT_YVU420",  # 12  YVU 4:2:0
    v4l2_fourcc('4', '2', '2', 'P'): "V4L2_PIX_FMT_YUV422P",  # 16  YVU422 planar

    # three non contiguous planes - Y, Cb, Cr
    v4l2_fourcc('Y', 'M', '1', '2'): "V4L2_PIX_FMT_YUV420M",  # 12  YUV420 planar
    v4l2_fourcc('Y', 'M', '2', '1'): "V4L2_PIX_FMT_YVU420M",  # 12  YVU420 planar
    v4l2_fourcc('Y', 'M', '1', '6'): "V4L2_PIX_FMT_YUV422M",  # 16  YUV422 planar
    v4l2_fourcc('Y', 'M', '6', '1'): "V4L2_PIX_FMT_YVU422M",  # 16  YVU422 planar
    v4l2_fourcc('Y', 'M', '2', '4'): "V4L2_PIX_FMT_YUV444M",  # 24  YUV444 planar
    v4l2_fourcc('Y', 'M', '4', '2'): "V4L2_PIX_FMT_YVU444M",  # 24  YVU444 planar

    # Bayer formats - see http://www.siliconimaging.com/RGB%20Bayer.htm
    v4l2_fourcc('B', 'A', '8', '1'): "V4L2_PIX_FMT_SBGGR8",  # 8  BGBG.. GRGR..
    v4l2_fourcc('G', 'B', 'R', 'G'): "V4L2_PIX_FMT_SGBRG8",  # 8  GBGB.. RGRG..
    v4l2_fourcc('G', 'R', 'B', 'G'): "V4L2_PIX_FMT_SGRBG8",  # 8  GRGR.. BGBG..
    v4l2_fourcc('R', 'G', 'G', 'B'): "V4L2_PIX_FMT_SRGGB8",  # 8  RGRG.. GBGB..
    v4l2_fourcc('B', 'G', '1', '0'): "V4L2_PIX_FMT_SBGGR10",  # 10  BGBG.. GRGR..
    v4l2_fourcc('G', 'B', '1', '0'): "V4L2_PIX_FMT_SGBRG10",  # 10  GBGB.. RGRG..
    v4l2_fourcc('B', 'A', '1', '0'): "V4L2_PIX_FMT_SGRBG10",  # 10  GRGR.. BGBG..
    v4l2_fourcc('R', 'G', '1', '0'): "V4L2_PIX_FMT_SRGGB10",  # 10  RGRG.. GBGB..
    # 10bit raw bayer packed, 5 bytes for every 4 pixels
    v4l2_fourcc('p', 'B', 'A', 'A'): "V4L2_PIX_FMT_SBGGR10P",
    v4l2_fourcc('p', 'G', 'A', 'A'): "V4L2_PIX_FMT_SGBRG10P",
    v4l2_fourcc('p', 'g', 'A', 'A'): "V4L2_PIX_FMT_SGRBG10P",
    v4l2_fourcc('p', 'R', 'A', 'A'): "V4L2_PIX_FMT_SRGGB10P",
    # 10bit raw bayer a-law compressed to 8 bits
    v4l2_fourcc('a', 'B', 'A', '8'): "V4L2_PIX_FMT_SBGGR10ALAW8",
    v4l2_fourcc('a', 'G', 'A', '8'): "V4L2_PIX_FMT_SGBRG10ALAW8",
    v4l2_fourcc('a', 'g', 'A', '8'): "V4L2_PIX_FMT_SGRBG10ALAW8",
    v4l2_fourcc('a', 'R', 'A', '8'): "V4L2_PIX_FMT_SRGGB10ALAW8",
    # 10bit raw bayer DPCM compressed to 8 bits
    v4l2_fourcc('b', 'B', 'A', '8'): "V4L2_PIX_FMT_SBGGR10DPCM8",
    v4l2_fourcc('b', 'G', 'A', '8'): "V4L2_PIX_FMT_SGBRG10DPCM8",
    v4l2_fourcc('B', 'D', '1', '0'): "V4L2_PIX_FMT_SGRBG10DPCM8",
    v4l2_fourcc('b', 'R', 'A', '8'): "V4L2_PIX_FMT_SRGGB10DPCM8",
    v4l2_fourcc('B', 'G', '1', '2'): "V4L2_PIX_FMT_SBGGR12",  # 12  BGBG.. GRGR..
    v4l2_fourcc('G', 'B', '1', '2'): "V4L2_PIX_FMT_SGBRG12",  # 12  GBGB.. RGRG..
    v4l2_fourcc('B', 'A', '1', '2'): "V4L2_PIX_FMT_SGRBG12",  # 12  GRGR.. BGBG..
    v4l2_fourcc('R', 'G', '1', '2'): "V4L2_PIX_FMT_SRGGB12",  # 12  RGRG.. GBGB..
    # 12bit raw bayer packed, 6 bytes for every 4 pixels
    v4l2_fourcc('p', 'B', 'C', 'C'): "V4L2_PIX_FMT_SBGGR12P",
    v4l2_fourcc('p', 'G', 'C', 'C'): "V4L2_PIX_FMT_SGBRG12P",
    v4l2_fourcc('p', 'g', 'C', 'C'): "V4L2_PIX_FMT_SGRBG12P",
    v4l2_fourcc('p', 'R', 'C', 'C'): "V4L2_PIX_FMT_SRGGB12P",
    v4l2_fourcc('B', 'G', '1', '4'): "V4L2_PIX_FMT_SBGGR14",  # 14  BGBG.. GRGR..
    v4l2_fourcc('G', 'B', '1', '4'): "V4L2_PIX_FMT_SGBRG14",  # 14  GBGB.. RGRG..
    v4l2_fourcc('G', 'R', '1', '4'): "V4L2_PIX_FMT_SGRBG14",  # 14  GRGR.. BGBG..
    v4l2_fourcc('R', 'G', '1', '4'): "V4L2_PIX_FMT_SRGGB14",  # 14  RGRG.. GBGB..
    # 14bit raw bayer packed, 7 bytes for every 4 pixels
    v4l2_fourcc('p', 'B', 'E', 'E'): "V4L2_PIX_FMT_SBGGR14P",
    v4l2_fourcc('p', 'G', 'E', 'E'): "V4L2_PIX_FMT_SGBRG14P",
    v4l2_fourcc('p', 'g', 'E', 'E'): "V4L2_PIX_FMT_SGRBG14P",
    v4l2_fourcc('p', 'R', 'E', 'E'): "V4L2_PIX_FMT_SRGGB14P",
    v4l2_fourcc('B', 'Y', 'R', '2'): "V4L2_PIX_FMT_SBGGR16",  # 16  BGBG.. GRGR..
    v4l2_fourcc('G', 'B', '1', '6'): "V4L2_PIX_FMT_SGBRG16",  # 16  GBGB.. RGRG..
    v4l2_fourcc('G', 'R', '1', '6'): "V4L2_PIX_FMT_SGRBG16",  # 16  GRGR.. BGBG..
    v4l2_fourcc('R', 'G', '1', '6'): "V4L2_PIX_FMT_SRGGB16",  # 16  RGRG.. GBGB..

    # HSV formats
    v4l2_fourcc('H', 'S', 'V', '3'): "V4L2_PIX_FMT_HSV24",
    v4l2_fourcc('H', 'S', 'V', '4'): "V4L2_PIX_FMT_HSV32",

    # compressed formats
    v4l2_fourcc('M', 'J', 'P', 'G'): "V4L2_PIX_FMT_MJPEG",  # Motion-JPEG
    v4l2_fourcc('J', 'P', 'E', 'G'): "V4L2_PIX_FMT_JPEG",  # JFIF JPEG
    v4l2_fourcc('d', 'v', 's', 'd'): "V4L2_PIX_FMT_DV",  # 1394
    v4l2_fourcc('M', 'P', 'E', 'G'): "V4L2_PIX_FMT_MPEG",  # MPEG-1/2/4 Multiplexed
    v4l2_fourcc('H', '2', '6', '4'): "V4L2_PIX_FMT_H264",  # H264 with start codes
    v4l2_fourcc('A', 'V', 'C', '1'): "V4L2_PIX_FMT_H264_NO_SC",  # H264 without start codes
    v4l2_fourcc('M', '2', '6', '4'): "V4L2_PIX_FMT_H264_MVC",  # H264 MVC
    v4l2_fourcc('H', '2', '6', '3'): "V4L2_PIX_FMT_H263",  # H263
    v4l2_fourcc('M', 'P', 'G', '1'): "V4L2_PIX_FMT_MPEG1",  # MPEG-1 ES
    v4l2_fourcc('M', 'P', 'G', '2'): "V4L2_PIX_FMT_MPEG2",  # MPEG-2 ES
    v4l2_fourcc('M', 'G', '2', 'S'): "V4L2_PIX_FMT_MPEG2_SLICE",  # MPEG-2 parsed slice data
    v4l2_fourcc('M', 'P', 'G', '4'): "V4L2_PIX_FMT_MPEG4",  # MPEG-4 part 2 ES
    v4l2_fourcc('X', 'V', 'I', 'D'): "V4L2_PIX_FMT_XVID",  # Xvid
    v4l2_fourcc('V', 'C', '1', 'G'): "V4L2_PIX_FMT_VC1_ANNEX_G",  # SMPTE 421M Annex G compliant stream
    v4l2_fourcc('V', 'C', '1', 'L'): "V4L2_PIX_FMT_VC1_ANNEX_L",  # SMPTE 421M Annex L compliant stream
    v4l2_fourcc('V', 'P', '8', '0'): "V4L2_PIX_FMT_VP8",  # VP8
    v4l2_fourcc('V', 'P', '8', 'F'): "V4L2_PIX_FMT_VP8_FRAME",  # VP8 parsed frame
    v4l2_fourcc('V', 'P', '9', '0'): "V4L2_PIX_FMT_VP9",  # VP9
    v4l2_fourcc('H', 'E', 'V', 'C'): "V4L2_PIX_FMT_HEVC",  # HEVC aka H.265
    v4l2_fourcc('F', 'W', 'H', 'T'): "V4L2_PIX_FMT_FWHT",  # Fast Walsh Hadamard Transform (vicodec)}
    v4l2_fourcc('S', 'F', 'W', 'H'): "V4L2_PIX_FMT_FWHT_STATELESS",  # Stateless FWHT (vicodec)}
    v4l2_fourcc('S', '2', '6', '4'): "V4L2_PIX_FMT_H264_SLICE",  # H264 parsed slices

    # Vendor-specific formats
    v4l2_fourcc('C', 'P', 'I', 'A'): "V4L2_PIX_FMT_CPIA1",  # cpia1 YUV
    v4l2_fourcc('W', 'N', 'V', 'A'): "V4L2_PIX_FMT_WNVA",  # Winnov hw compress
    v4l2_fourcc('S', '9', '1', '0'): "V4L2_PIX_FMT_SN9C10X",  # SN9C10x compression
    v4l2_fourcc('S', '9', '2', '0'): "V4L2_PIX_FMT_SN9C20X_I420",  # SN9C20x YUV 4:2:0
    v4l2_fourcc('P', 'W', 'C', '1'): "V4L2_PIX_FMT_PWC1",  # pwc older webcam
    v4l2_fourcc('P', 'W', 'C', '2'): "V4L2_PIX_FMT_PWC2",  # pwc newer webcam
    v4l2_fourcc('E', '6', '2', '5'): "V4L2_PIX_FMT_ET61X251",  # ET61X251 compression
    v4l2_fourcc('S', '5', '0', '1'): "V4L2_PIX_FMT_SPCA501",  # YUYV per line
    v4l2_fourcc('S', '5', '0', '5'): "V4L2_PIX_FMT_SPCA505",  # YYUV per line
    v4l2_fourcc('S', '5', '0', '8'): "V4L2_PIX_FMT_SPCA508",  # YUVY per line
    v4l2_fourcc('S', '5', '6', '1'): "V4L2_PIX_FMT_SPCA561",  # compressed GBRG bayer
    v4l2_fourcc('P', '2', '0', '7'): "V4L2_PIX_FMT_PAC207",  # compressed BGGR bayer
    v4l2_fourcc('M', '3', '1', '0'): "V4L2_PIX_FMT_MR97310A",  # compressed BGGR bayer
    v4l2_fourcc('J', 'L', '2', '0'): "V4L2_PIX_FMT_JL2005BCD",  # compressed RGGB bayer
    v4l2_fourcc('S', 'O', 'N', 'X'): "V4L2_PIX_FMT_SN9C2028",  # compressed GBRG bayer
    v4l2_fourcc('9', '0', '5', 'C'): "V4L2_PIX_FMT_SQ905C",  # compressed RGGB bayer
    v4l2_fourcc('P', 'J', 'P', 'G'): "V4L2_PIX_FMT_PJPG",  # Pixart 73xx JPEG
    v4l2_fourcc('O', '5', '1', '1'): "V4L2_PIX_FMT_OV511",  # ov511 JPEG
    v4l2_fourcc('O', '5', '1', '8'): "V4L2_PIX_FMT_OV518",  # ov518 JPEG
    v4l2_fourcc('S', '6', '8', '0'): "V4L2_PIX_FMT_STV0680",  # stv0680 bayer
    v4l2_fourcc('T', 'M', '6', '0'): "V4L2_PIX_FMT_TM6000",  # tm5600/tm60x0
    v4l2_fourcc('C', 'I', 'T', 'V'): "V4L2_PIX_FMT_CIT_YYVYUY",  # one line of Y then 1 line of VYUY
    v4l2_fourcc('K', 'O', 'N', 'I'): "V4L2_PIX_FMT_KONICA420",  # YUV420 planar in blocks of 256 pixels
    v4l2_fourcc('J', 'P', 'G', 'L'): "V4L2_PIX_FMT_JPGL",  # JPEG-Lite
    v4l2_fourcc('S', '4', '0', '1'): "V4L2_PIX_FMT_SE401",  # se401 janggu compressed rgb
    v4l2_fourcc('S', '5', 'C', 'I'): "V4L2_PIX_FMT_S5C_UYVY_JPG",  # S5C73M3 interleaved UYVY/JPEG
    v4l2_fourcc('Y', '8', 'I', ' '): "V4L2_PIX_FMT_Y8I",  # Greyscale 8-bit L/R interleaved
    v4l2_fourcc('Y', '1', '2', 'I'): "V4L2_PIX_FMT_Y12I",  # Greyscale 12-bit L/R interleaved
    v4l2_fourcc('Z', '1', '6', ' '): "V4L2_PIX_FMT_Z16",  # Depth data 16-bit
    v4l2_fourcc('M', 'T', '2', '1'): "V4L2_PIX_FMT_MT21C",  # Mediatek compressed block mode
    v4l2_fourcc('I', 'N', 'Z', 'I'): "V4L2_PIX_FMT_INZI",  # Intel Planar Greyscale 10-bit and Depth 16-bit
    v4l2_fourcc('S', 'T', '1', '2'): "V4L2_PIX_FMT_SUNXI_TILED_NV12",  # Sunxi Tiled NV12 Format
    v4l2_fourcc('C', 'N', 'F', '4'): "V4L2_PIX_FMT_CNF4",  # Intel 4-bit packed depth confidence information
    v4l2_fourcc('H', 'I', '2', '4'): "V4L2_PIX_FMT_HI240",  # BTTV 8-bit dithered RGB

    # 10bit raw bayer packed, 32 bytes for every 25 pixels, last LSB 6 bits unused
    v4l2_fourcc('i', 'p', '3', 'b'): "V4L2_PIX_FMT_IPU3_SBGGR10",  # IPU3 packed 10-bit BGGR bayer
    v4l2_fourcc('i', 'p', '3', 'g'): "V4L2_PIX_FMT_IPU3_SGBRG10",  # IPU3 packed 10-bit GBRG bayer
    v4l2_fourcc('i', 'p', '3', 'G'): "V4L2_PIX_FMT_IPU3_SGRBG10",  # IPU3 packed 10-bit GRBG bayer
    v4l2_fourcc('i', 'p', '3', 'r'): "V4L2_PIX_FMT_IPU3_SRGGB10",  # IPU3 packed 10-bit RGGB bayer

    # SDR formats - used only for Software Defined Radio devices
    v4l2_fourcc('C', 'U', '0', '8'): "V4L2_SDR_FMT_CU8",  # IQ u8
    v4l2_fourcc('C', 'U', '1', '6'): "V4L2_SDR_FMT_CU16LE",  # IQ u16le
    v4l2_fourcc('C', 'S', '0', '8'): "V4L2_SDR_FMT_CS8",  # complex s8
    v4l2_fourcc('C', 'S', '1', '4'): "V4L2_SDR_FMT_CS14LE",  # complex s14le
    v4l2_fourcc('R', 'U', '1', '2'): "V4L2_SDR_FMT_RU12LE",  # real u12le
    v4l2_fourcc('P', 'C', '1', '6'): "V4L2_SDR_FMT_PCU16BE",  # planar complex u16be
    v4l2_fourcc('P', 'C', '1', '8'): "V4L2_SDR_FMT_PCU18BE",  # planar complex u18be
    v4l2_fourcc('P', 'C', '2', '0'): "V4L2_SDR_FMT_PCU20BE",  # planar complex u20be

    # Touch formats - used for Touch devices
    v4l2_fourcc('T', 'D', '1', '6'): "V4L2_TCH_FMT_DELTA_TD16",  # 16-bit signed deltas
    v4l2_fourcc('T', 'D', '0', '8'): "V4L2_TCH_FMT_DELTA_TD08",  # 8-bit signed deltas
    v4l2_fourcc('T', 'U', '1', '6'): "V4L2_TCH_FMT_TU16",  # 16-bit unsigned touch data
    v4l2_fourcc('T', 'U', '0', '8'): "V4L2_TCH_FMT_TU08",  # 8-bit unsigned touch data

    # Meta-data formats
    v4l2_fourcc('V', 'S', 'P', 'H'): "V4L2_META_FMT_VSP1_HGO",  # R-Car VSP1 1-D Histogram
    v4l2_fourcc('V', 'S', 'P', 'T'): "V4L2_META_FMT_VSP1_HGT",  # R-Car VSP1 2-D Histogram
    v4l2_fourcc('U', 'V', 'C', 'H'): "V4L2_META_FMT_UVC",  # UVC Payload Header metadata
    v4l2_fourcc('D', '4', 'X', 'X'): "V4L2_META_FMT_D4XX",  # D4XX Payload Header metadata
    v4l2_fourcc('V', 'I', 'V', 'D'): "V4L2_META_FMT_VIVID",  # Vivid Metadata
}
