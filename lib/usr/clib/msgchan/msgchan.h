/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2022-2023 Intel Corporation.
 */

#ifndef _MSGCHAN_H_
#define _MSGCHAN_H_

#include <sys/queue.h>
#include <cne_common.h>
#include <cne_ring.h>
#include <cne_ring_api.h>

/**
 * @file
 * Message Channels
 *
 * Create a message channel using two lockless rings to communicate between two threads.
 *
 * Message channels are similar to pipes in Linux and other platforms, but does not support
 * message passing between processes.
 *
 */

#ifdef __cplusplus
extern "C" {
#endif

#define MC_NAME_SIZE (CNE_NAME_LEN + 4) /**< Max size of the msgchan name */
#define MC_RECV_RING 0                  /**< Receive index into msgchan_t.rings */
#define MC_SEND_RING 1                  /**< Send index into msgchan_t.rings */

#define MC_NO_CHILD_CREATE \
    0x80000000 /**< If set in mc_create() flags then child will not be created */
#define MC_MEM_ALLOCATED 0x40000000 /**< Memory allocated flag (internal) */

typedef void msgchan_t; /**< Opaque msgchan structure pointer */

typedef struct msgchan_info {
    cne_ring_t *recv_ring; /**< Pointers to the recv ring */
    cne_ring_t *send_ring; /**< Pointers to the send ring */
    uint64_t send_calls;   /**< Number of send calls */
    uint64_t send_cnt;     /**< Number of objects sent */
    uint64_t send_full;    /**< Number of objects send ring full */
    uint64_t recv_calls;   /**< Number of receive calls */
    uint64_t recv_cnt;     /**< Number of objects received */
    uint64_t recv_empty;   /**< Number of calls to receive with no values */
} msgchan_info_t;

/**
 * @brief Create a message channel using the specified memory and size.
 *
 * Calling mc_create() with an existing channel name will create a child
 * channel attached to the parent channel. The API can be used to create a message
 * channel in a specific location to help in sharing between processes or the application
 * needs the msgchan created at a particular location. When addr and size are zero, the
 * routine works just like mc_create() API.
 *
 * @param addr
 *   The address of the to place the message channel structure and data, can be NULL.
 * @param size
 *   The size of the message channel memory passed into the routine, can be zero.
 * @param name
 *   The name of the message channel
 * @param esize
 *   The size of the message channel ring entries, if zero use 8 byte default.
 * @param count
 *   The number of entries in the lockless ring for each direction.
 * @param flags
 *   The cne_ring_t flags for SP/SC or MP/MC type flags, look at cne_ring_create()
 *   Defaults to (RING_F_MP_ENQ | RING_F_MC_DEQ) if flags is zero. Use the flags
 *   (RING_F_SP_ENQ | RING_F_SC_DEQ);
 *   Or in the bit MC_NO_CHILD_CREATE to not allow creating a child, NULL will be returned.
 * @return
 *   The pointer to the msgchan structure or NULL on error
 */
CNDP_API msgchan_t *mc_init(void *addr, size_t size, const char *name, unsigned int esize,
                            unsigned int count, uint32_t flags);

/**
 * @brief Create a message channel with the given size of each ring entry.
 *
 * Calling mc_create_elem() with an existing channel name will create a child
 * channel attached to the parent channel.
 *
 * @param name
 *   The name of the message channel
 * @param esize
 *   The size of the message channel ring entries in bytes. Can be zero to use 8 byte default.
 * @param count
 *   The number of entries in the lockless ring for each direction.
 * @param flags
 *   The cne_ring_t flags for SP/SC or MP/MC type flags, look at cne_ring_create()
 *   Defaults to (RING_F_MP_ENQ | RING_F_MC_DEQ) if flags is zero. Use the flags
 *   (RING_F_SP_ENQ | RING_F_SC_DEQ);
 *   Or in the bit MC_NO_CHILD_CREATE to not allow creating a child, NULL will be returned.
 * @return
 *   The pointer to the msgchan structure or NULL on error
 */
CNDP_API msgchan_t *mc_create(const char *name, unsigned int esize, unsigned int count,
                              uint32_t flags);

/**
 * Return the number of bytes required for a message channel structure and rings.
 *
 * @param esize
 *   The number of bytes in a given ring entry, if zero use RING_DFLT_ELEM_SZ (8 bytes).
 * @param count
 *   The number of entries each ring.
 * @return
 *   Number of bytes needed for the given ring information.
 */
ssize_t mc_get_total_memsize(unsigned int esize, unsigned int count);

/**
 * @brief Destroy the message channel and free resources.
 *
 * @param mc
 *   The msgchan structure pointer to destroy
 * @return
 *   N/A
 */
CNDP_API void mc_destroy(msgchan_t *mc);

/**
 * @brief Send object messages to the other end of the channel
 *
 * @param mc
 *   The message channel structure pointer
 * @param objs
 *   An array of void *objects to send
 * @param count
 *   The number of entries in the objs array.
 * @return
 *   -1 on error or number of objects sent.
 */
CNDP_API int mc_send(msgchan_t *mc, void **objs, int count);

/**
 * @brief Receive message routine from other end of the channel
 *
 * @param mc
 *   The message channel structure pointer
 * @param objs
 *   An array of objects pointers to place the received objects pointers
 * @param count
 *   The number of entries in the objs array.
 * @param msec
 *   Number of milliseconds to wait for data, if return without waiting.
 * @return
 *   -1 on error or number of objects
 */
CNDP_API int mc_recv(msgchan_t *mc, void **objs, int count, uint64_t msec);

/**
 * @brief Lookup a message channel by name - parent only lookup
 *
 * @param name
 *   The name of the message channel to find, which is for parent channels
 * @return
 *   NULL if not found, otherwise the message channel pointer
 */
CNDP_API msgchan_t *mc_lookup(const char *name);

/**
 * @brief Return the name string for the msgchan_t pointer
 *
 * @param mc
 *   The message channel structure pointer
 * @return
 *   NULL if invalid pointer or string to message channel name
 */
CNDP_API const char *mc_name(msgchan_t *mc);

/**
 * @brief Return size and free space in the Producer/Consumer rings.
 *
 * @param mc
 *   The message channel structure pointer
 * @param recv_free_cnt
 *   The pointer to place the receive free count, can be NULL.
 * @param send_free_cnt
 *   The pointer to place the send free count, can be NULL.
 * @return
 *   -1 on error or size of the massage channel rings.
 */
CNDP_API int mc_size(msgchan_t *mc, int *recv_free_cnt, int *send_free_cnt);

/**
 * Return the number of entries in the receive ring.
 *
 * @param mc
 *   Pointer to the msgchan structure
 * @return
 *   Number of entries in the receive ring
 */
CNDP_API int mc_recv_count(msgchan_t *mc);

/**
 * Return the number of entries in the send ring.
 *
 * @param mc
 *   Pointer to the msgchan structure
 * @return
 *   Number of entries in the send ring
 */
CNDP_API int mc_send_count(msgchan_t *mc);

/**
 * Return the message channel information structure data
 *
 * @param _mc
 *   The message channel structure pointer
 * @param info
 *   The pointer to the msgchan_info_t structure
 * @return
 *   0 on success or -1 on error
 */
CNDP_API int mc_info(msgchan_t *_mc, msgchan_info_t *info);

/**
 * @brief Dump out the details of the given message channel structure
 *
 * @param mc
 *   The message channel structure pointer
 * @return
 *   -1 if mc is NULL or 0 on success
 */
CNDP_API void mc_dump(msgchan_t *mc);

/**
 * @brief List out all message channels currently created.
 */
CNDP_API void mc_list(void);

#ifdef __cplusplus
}
#endif

#endif /* _MSGCHAN_H_ */
