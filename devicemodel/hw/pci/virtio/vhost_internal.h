/*
 * Copyright (C) 2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

/**
 * @file  vhost_internal.h
 *
 * @brief VHOST Internal APIs for ACRN Project
 */

#ifndef __VHOST_INTERNAL_H__
#define __VHOST_INTERNAL_H__

#include "vhost.h"

extern const struct vhost_dev_ops vhost_kernel_ops;
extern const struct vhost_dev_ops vhost_user_ops;

#endif /* __VHOST_INTERNAL_H__ */
