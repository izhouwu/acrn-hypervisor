/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SHARED_BUF_H
#define SHARED_BUF_H

#include <linux/types.h>
#include "acrn_common.h"

static inline void sbuf_clear_flags(struct shared_buf *sbuf, uint64_t flags)
{
        sbuf->flags &= ~flags;
}

static inline void sbuf_set_flags(struct shared_buf *sbuf, uint64_t flags)
{
        sbuf->flags = flags;
}

static inline void sbuf_add_flags(struct shared_buf *sbuf, uint64_t flags)
{
        sbuf->flags |= flags;
}

int sbuf_get(struct shared_buf *sbuf, uint8_t *data);
int sbuf_write(int fd, struct shared_buf *sbuf);
int sbuf_clear_buffered(struct shared_buf *sbuf);
#endif /* SHARED_BUF_H */
