/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2022-2023 Intel Corporation
 */

#ifndef _MSGCHAN_PRIV_H_
#define _MSGCHAN_PRIV_H_

#include <sys/queue.h>
#include <cne_common.h>
#include <cne_ring.h>
#include <cne_ring_api.h>
#include "msgchan.h"

/**
 * @file
 * Private Message Channels information
 *
 * Private data structures and information for msgchan library. The external msgchan pointer
 * is a void pointer and converted to the msg_chan_t structure pointer in the code.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define MC_FREE_COOKIE   0
#define MC_PARENT_COOKIE ('C' << 24 | 'h' << 16 | 'a' << 8 | 'n')
#define MC_CHILD_COOKIE  ('K' << 24 | 'i' << 16 | 'd' << 8 | 'd')
#define MC_MAX_CHILDREN  32 /**< Maximum number of children */

struct mc_common {
    uint32_t cookie;                 /**< Cookie value to test for valid entry */
    uint32_t index;                  /**< Index of child structure */
    char name[MC_NAME_SIZE];         /**< The name of the message channel */
    cne_ring_t *rings[2];            /**< Pointers to the send/recv rings */
    atomic_uint_fast64_t send_calls; /**< Number of send calls */
    atomic_uint_fast64_t send_cnt;   /**< Number of objects sent */
    atomic_uint_fast64_t send_full;  /**< Number of objects send ring is full */
    atomic_uint_fast64_t recv_calls; /**< Number of receive calls */
    atomic_uint_fast64_t recv_cnt;   /**< Number of objects received */
    atomic_uint_fast64_t recv_empty; /**< Number of calls to receive with no values */
} __cne_cache_aligned;

typedef struct mc_common mc_common_t;

typedef struct mc_child {
    mc_common_t hdr;
} mc_child_t;

struct msg_chan {
    mc_common_t hdr;                      /**< Common message channel header */
    uint16_t flags;                       /**< flags used to configure a message channel */
    mc_child_t children[MC_MAX_CHILDREN]; /**< Children array */
} __cne_cache_aligned;

typedef struct msg_chan msg_chan_t;

#ifdef __cplusplus
}
#endif

#endif /* _MSGCHAN_PRIV_H_ */
