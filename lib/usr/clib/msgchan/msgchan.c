/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2022-2023 Intel Corporation.
 */

#include <inttypes.h>          // for PRIu32
#include <bsd/string.h>        // for strlcpy
#include <cne_common.h>        // for cne_align32pow2, CNE_CACHE_LINE_SIZE
#include <cne_log.h>           // for CNE_LOG, CNE_LOG_ERR, CNE_LOG_DEBUG
#include <errno.h>             // for EINVAL, errno, ENAMETOOLONG, ENOMEM
#include <stdio.h>             // for fprintf, NULL, size_t, FILE, stdout
#include <string.h>            // for memset, strnlen
#include <stddef.h>            // for offsetof
#include <stdint.h>            // for uint32_t
#include <stdlib.h>            // for calloc, free
#include <sys/types.h>         // for ssize_t
#include <pthread.h>
#include <cne_spinlock.h>
#include <cne_cycles.h>
#include <cne_mutex_helper.h>
#include <cne_ring.h>
#include <cne_tailq.h>

#include "msgchan_priv.h"
#include "msgchan.h"

#define MC_PARENT_PREFIX    "P:"
#define MC_CHILD_PREFIX     "C%d:"
#define MC_RECV_RING_PREFIX "RR:"
#define MC_SEND_RING_PREFIX "SR:"

TAILQ_HEAD(mc_list, cne_tailq_entry);

static struct cne_tailq_elem mc_tailq = {
    .name = "MC_LIST",
};
CNE_REGISTER_TAILQ(mc_tailq)

static pthread_mutex_t mc_list_mutex;

static inline msg_chan_t *
mc_parent(mc_child_t *child)
{
    int offset = offsetof(struct msg_chan, children);

    offset += (child->hdr.index * sizeof(mc_child_t));

    return CNE_PTR_SUB(child, offset);
}

static inline int
mc_list_lock(void)
{
    int ret = pthread_mutex_lock(&mc_list_mutex);

    if (ret)
        CNE_ERR_RET_VAL(0, "failed: %s\n", strerror(ret));

    return 1;
}

static inline void
mc_list_unlock(void)
{
    int ret = pthread_mutex_unlock(&mc_list_mutex);

    if (ret)
        CNE_WARN("failed: %s\n", strerror(ret));
}

static inline mc_child_t *
mc_child_alloc(msg_chan_t *mc)
{
    mc_child_t *child;
    uint32_t expect;

    for (int i = 0; i < MC_MAX_CHILDREN; i++) {
        child  = &mc->children[i];
        expect = 0; /* must reset this value each time thru the loop */

        if (atomic_compare_exchange_strong(&child->hdr.cookie, &expect, MC_CHILD_COOKIE))
            return child;
    }
    return NULL;
}

static inline void
mc_child_free(mc_child_t *child)
{
    if (child && atomic_load(&child->hdr.cookie)) {
        uint32_t expect = MC_CHILD_COOKIE;

        if (!atomic_compare_exchange_strong(&child->hdr.cookie, &expect, 0))
            CNE_RET("unable to free msgchan child %d, cookie %x, expect %x\n", child->hdr.index,
                    child->hdr.cookie, expect);
    }
}

static msgchan_t *
attach_child(msg_chan_t *parent)
{
    mc_child_t *child = NULL;
    char name[MC_NAME_SIZE + 1];
    int n;

    child = mc_child_alloc(parent);
    if (!child)
        CNE_NULL_RET("Failed to allocate new child msg_chan_t structure\n");

    snprintf(name, MC_NAME_SIZE, MC_CHILD_PREFIX, child->hdr.index);
    n = strlcpy(child->hdr.name, name, sizeof(child->hdr.name));
    strlcpy(child->hdr.name + n, parent->hdr.name + 2, sizeof(child->hdr.name) - n);

    child->hdr.rings[MC_RECV_RING] = parent->hdr.rings[MC_SEND_RING]; /* Swap Tx/Rx rings */
    child->hdr.rings[MC_SEND_RING] = parent->hdr.rings[MC_RECV_RING];

    return child;
}

ssize_t
mc_get_total_memsize(unsigned int esize, unsigned int count)
{
    ssize_t sz;

    if (esize == 0)
        esize = RING_DFLT_ELEM_SZ;

    /* Check if element size is a multiple of 4B */
    if (esize % 4 != 0)
        CNE_ERR_RET("element size is not a multiple of 4\n");

    /* count must be a power of 2 */
    if (!cne_is_power_of_2(count) || (count > CNE_RING_SZ_MASK))
        CNE_ERR_RET(
            "Requested number of elements is invalid, must be power of 2, and not exceed %u\n",
            CNE_RING_SZ_MASK);

    sz = sizeof(struct msg_chan) + (cne_ring_get_memsize_elem(esize, count) * 2);
    sz = CNE_ALIGN(sz, CNE_CACHE_LINE_SIZE);

    CNE_DEBUG("MsgChan structure size %ld\n", sizeof(struct msg_chan));
    CNE_DEBUG("MsgChan Rx ring size   %ld\n", cne_ring_get_memsize_elem(esize, count));
    CNE_DEBUG("MsgChan Tx ring size   %ld\n", cne_ring_get_memsize_elem(esize, count));
    CNE_DEBUG("MsgChan total size     %ld\n", sz);
    return sz;
}

msgchan_t *
mc_init(void *addr, size_t size, const char *name, unsigned int esize, unsigned int count,
        uint32_t flags)
{
    msg_chan_t *mc;
    char rname[CNE_RING_NAMESIZE + 1];
    struct cne_tailq_entry *te = NULL;
    struct mc_list *list       = NULL;
    bool allow_child_create;
    ssize_t mc_size;
    int n;

    /* When address is supplied then size can not be zero */
    if (addr && size == 0) {
        errno = EINVAL;
        CNE_NULL_RET("address is given, but size is zero\n");
    }

    /* Name can not be NULL */
    if (name == NULL) {
        errno = EINVAL;
        CNE_NULL_RET("name string not provided\n");
    }

    /* Verify the size of the name will fit in the msgchan structure */
    if (strnlen(name, CNE_RING_NAMESIZE) == CNE_RING_NAMESIZE) {
        errno = ENAMETOOLONG;
        CNE_NULL_RET("name too long\n");
    }

    /* Count can not be zero */
    if (count == 0) {
        errno = EINVAL;
        CNE_NULL_RET("element count is zero\n");
    }

    /* Validate the flags passed in from caller */
    if (flags & ~(MC_NO_CHILD_CREATE | RING_F_EXACT_SZ | RING_F_SC_DEQ | RING_F_SP_ENQ)) {
        errno = EINVAL;
        CNE_NULL_RET("Flags can only have MC_NO_CHILD_CREATE + RING flags set\n");
    }

    /* Determine is a child can be created if needed */
    allow_child_create = ((flags & MC_NO_CHILD_CREATE) == 0);
    flags &= ~MC_NO_CHILD_CREATE; /* Remove flag if present */

    /* Make sure the name is not already used or needs child created */
    mc = mc_lookup(name);
    if (mc) {
        msg_chan_t *child = NULL;

        if (allow_child_create)
            child = attach_child(mc);

        return child;
    }

    /* calculate the msgchan size and validate the values */
    mc_size = mc_get_total_memsize(esize, count);
    if (mc_size < 0) {
        errno = EINVAL;
        return NULL;
    }

    /* Use the callers memory if passed into the routine */
    if (addr) {
        /* callers memory needs to be able to hold the msgchan structures */
        if ((size_t)mc_size > size) {
            errno = ENOMEM;
            CNE_NULL_RET("supplied memory size is < %ld\n", mc_size);
        }
        mc = addr;
        memset(mc, 0, mc_size);
    } else {
        mc = calloc(1, sizeof(msg_chan_t));
        if (!mc)
            CNE_ERR_GOTO(err, "Unable to allocate memory\n");
        mc->flags |= MC_MEM_ALLOCATED;
    }

    /* Set the msgchan as a parent entry */
    mc->hdr.cookie = MC_PARENT_COOKIE;

    /* Create the parent name and put in msgchan structure */
    n = strlcpy(mc->hdr.name, MC_PARENT_PREFIX, sizeof(mc->hdr.name));
    strlcpy(mc->hdr.name + n, name, sizeof(mc->hdr.name) - n);

    /* Create the Recv and Send ring names */
    n = strlcpy(rname, MC_RECV_RING_PREFIX, sizeof(rname)); /* RR - Receive Ring */
    strlcpy(rname + n, name, sizeof(rname) - n);
    if ((mc->hdr.rings[MC_RECV_RING] = cne_ring_create(rname, esize, count, flags)) == NULL)
        CNE_ERR_GOTO(err, "Failed to create Recv ring\n");

    n = strlcpy(rname, MC_SEND_RING_PREFIX, sizeof(rname)); /* SR - Send Ring */
    strlcpy(rname + n, name, sizeof(rname) - n);
    if ((mc->hdr.rings[MC_SEND_RING] = cne_ring_create(rname, esize, count, flags)) == NULL)
        CNE_ERR_GOTO(err, "Failed to create Send ring\n");

    /* Mark the children entries with the child index values */
    for (int i = 0; i < MC_MAX_CHILDREN; i++)
        mc->children[i].hdr.index = i;
    mc->hdr.index = MC_MAX_CHILDREN;

    list = CNE_TAILQ_CAST(mc_tailq.head, mc_list);

    te = calloc(1, sizeof(struct cne_tailq_entry));
    if (te == NULL)
        goto err;

    te->data = mc;

    if (!mc_list_lock())
        CNE_ERR_GOTO(err, "unable to lock msgchan list\n");

    TAILQ_INSERT_TAIL(list, te, next);

    mc_list_unlock();

    return mc;
err:
    if (mc) {
        cne_ring_free(mc->hdr.rings[MC_RECV_RING]);
        cne_ring_free(mc->hdr.rings[MC_SEND_RING]);
        if (mc->flags & MC_MEM_ALLOCATED) {
            memset(mc, 0, sizeof(msg_chan_t));
            free(mc);
        }
    }
    free(te);

    return NULL;
}

msgchan_t *
mc_create(const char *name, unsigned int esize, unsigned int count, uint32_t flags)
{
    return mc_init(NULL, 0, name, esize, count, flags);
}

void
mc_destroy(msgchan_t *_mc)
{
    struct mc_list *list       = NULL;
    struct cne_tailq_entry *te = NULL;
    mc_common_t *hdr           = _mc;
    msg_chan_t *mc             = _mc;
    mc_child_t *ch             = _mc;

    list = CNE_TAILQ_CAST(mc_tailq.head, mc_list);

    if (hdr) {
        if (mc_list_lock()) {
            switch (hdr->cookie) {
            case MC_PARENT_COOKIE:
                TAILQ_FOREACH (te, list, next) {
                    if (te->data == (void *)hdr)
                        break;
                }

                if (te != NULL) {
                    TAILQ_REMOVE(list, te, next);
                    free(te);

                    cne_ring_free(hdr->rings[MC_RECV_RING]);
                    cne_ring_free(hdr->rings[MC_SEND_RING]);

                    for (int i = 0; i < MC_MAX_CHILDREN; i++) {
                        mc_child_t *child = &mc->children[i];

                        mc_child_free(child);
                    }
                }

                if (mc->flags & MC_MEM_ALLOCATED)
                    free(mc);
                break;
            case MC_CHILD_COOKIE: /* Handle child destroy */
                mc_child_free(ch);
                memset(ch, 0, sizeof(mc_child_t));
                break;
            case MC_FREE_COOKIE:
                break;
            default:
                CNE_ERR("Unknown cookie value %x for %s\n", hdr->cookie, hdr->name);
                break;
            }
            mc_list_unlock();
        }
    }
}

static int
__recv(msg_chan_t *mc, void **objs, int count, uint64_t msec)
{
    cne_ring_t *r;
    int nb_objs = 0;

    atomic_fetch_add(&mc->hdr.recv_calls, 1);

    if (count == 0)
        return 0;

    r = mc->hdr.rings[MC_RECV_RING];

    if (msec) {
        uint64_t begin, stop;

        begin = cne_rdtsc_precise();
        stop  = begin + ((cne_get_timer_hz() / 1000) * msec);

        while (nb_objs == 0 && begin < stop) {
            nb_objs = cne_ring_dequeue_burst(r, objs, count, NULL);
            if (nb_objs == 0) {
                begin = cne_rdtsc_precise();
                cne_pause();
            }
        }
    } else
        nb_objs = cne_ring_dequeue_burst(r, objs, count, NULL);

    if (nb_objs == 0)
        atomic_fetch_add(&mc->hdr.recv_empty, 1);
    else
        atomic_fetch_add(&mc->hdr.recv_cnt, nb_objs);

    return nb_objs;
}

static int
__send(msgchan_t *_mc, void **objs, int count)
{
    msg_chan_t *mc = _mc;
    cne_ring_t *r;
    int nb_objs;

    atomic_fetch_add(&mc->hdr.send_calls, 1);

    r = mc->hdr.rings[MC_SEND_RING];

    nb_objs = cne_ring_enqueue_burst(r, objs, count, NULL);
    if (nb_objs < 0)
        CNE_ERR_RET("[orange]Sending to msgchan failed[]\n");

    atomic_fetch_add(&mc->hdr.send_cnt, nb_objs);
    if (nb_objs != count)
        atomic_fetch_add(&mc->hdr.send_full, 1);

    return nb_objs;
}

int
mc_send(msgchan_t *_mc, void **objs, int count)
{
    msg_chan_t *mc = _mc;

    if (!mc || !objs || mc->hdr.cookie == MC_FREE_COOKIE)
        CNE_ERR_RET("Invalid parameters\n");

    if (count < 0)
        CNE_ERR_RET("Count of objects is negative\n");

    return __send(mc, objs, count);
}

int
mc_recv(msgchan_t *_mc, void **objs, int count, uint64_t msec)
{
    msg_chan_t *mc = _mc;

    if (!mc || !objs || mc->hdr.cookie == MC_FREE_COOKIE)
        CNE_ERR_RET("Invalid parameters\n");

    if (count < 0)
        CNE_ERR_RET("Count of objects is %d\n", count);

    return __recv(mc, objs, count, msec);
}

msgchan_t *
mc_lookup(const char *name)
{
    struct mc_list *list       = NULL;
    struct cne_tailq_entry *te = NULL;
    char pname[MC_NAME_SIZE + 1];

    if (name) {
        int n;

        if (mc_list_lock()) {
            msg_chan_t *mc = NULL;

            /* prepend the parent string P: */
            n = strlcpy(pname, MC_PARENT_PREFIX, sizeof(pname));
            strlcpy(pname + n, name, sizeof(pname) - n);

            list = CNE_TAILQ_CAST(mc_tailq.head, mc_list);

            TAILQ_FOREACH (te, list, next) {
                mc = te->data;

                if (!strcmp(pname, mc->hdr.name)) {
                    mc_list_unlock();
                    return mc;
                }
            }
            mc_list_unlock();
        }
    }
    return NULL;
}

const char *
mc_name(msgchan_t *_mc)
{
    msg_chan_t *mc = _mc;

    return (mc && mc->hdr.cookie != MC_FREE_COOKIE) ? mc->hdr.name : NULL;
}

int
mc_recv_count(msgchan_t *_mc)
{
    msg_chan_t *mc = _mc;

    if (!mc || mc->hdr.cookie == MC_FREE_COOKIE || !mc->hdr.rings[MC_RECV_RING])
        return 0;
    return cne_ring_count(mc->hdr.rings[MC_RECV_RING]);
}

int
mc_send_count(msgchan_t *_mc)
{
    msg_chan_t *mc = _mc;

    if (!mc || mc->hdr.cookie == MC_FREE_COOKIE || !mc->hdr.rings[MC_SEND_RING])
        return 0;
    return cne_ring_count(mc->hdr.rings[MC_SEND_RING]);
}

int
mc_size(msgchan_t *_mc, int *recv_free_cnt, int *send_free_cnt)
{
    msg_chan_t *mc = _mc;

    if (mc && mc->hdr.cookie != MC_FREE_COOKIE) {
        int ring1_sz, ring2_sz;

        ring1_sz = cne_ring_free_count(mc->hdr.rings[MC_RECV_RING]);
        ring2_sz = cne_ring_free_count(mc->hdr.rings[MC_SEND_RING]);

        if (recv_free_cnt)
            *recv_free_cnt = ring1_sz;
        if (send_free_cnt)
            *send_free_cnt = ring2_sz;

        return cne_ring_get_capacity(mc->hdr.rings[MC_RECV_RING]);
    }
    return -1;
}

int
mc_info(msgchan_t *_mc, msgchan_info_t *info)
{
    msg_chan_t *mc = _mc;

    if (mc && info && mc->hdr.cookie != MC_FREE_COOKIE) {
        info->recv_ring  = mc->hdr.rings[MC_RECV_RING];
        info->send_ring  = mc->hdr.rings[MC_SEND_RING];
        info->send_calls = atomic_load(&mc->hdr.send_calls);
        info->send_cnt   = atomic_load(&mc->hdr.send_cnt);
        info->send_full  = atomic_load(&mc->hdr.send_full);
        info->recv_calls = atomic_load(&mc->hdr.recv_calls);
        info->recv_cnt   = atomic_load(&mc->hdr.recv_cnt);
        info->recv_empty = atomic_load(&mc->hdr.recv_empty);
        return 0;
    }

    return -1;
}

void
mc_dump(msgchan_t *_mc)
{
    msg_chan_t *mc = _mc;

    if (!mc_list_lock())
        CNE_RET("Failed to lock the list\n");

    if (mc && mc->hdr.cookie != MC_FREE_COOKIE) {
        int n = mc_size(_mc, NULL, NULL);

        cne_printf("  [cyan]%-16s [magenta]size [green]%'d[], [magenta]rings: Recv [green]%p[], "
                   "[magenta]Send [green]%p[]\n",
                   mc->hdr.name, n, mc->hdr.rings[MC_RECV_RING], mc->hdr.rings[MC_SEND_RING]);

        cne_printf("          [magenta] %15s %15s %15s %15s[]\n", "Calls", "Count", "Full/Empty",
                   "Adj-Calls");
        cne_printf("     [orange]Send[]: [cyan]%'15ld %'15ld %'15ld %'15ld[]\n",
                   atomic_load(&mc->hdr.send_calls), atomic_load(&mc->hdr.send_cnt),
                   atomic_load(&mc->hdr.send_full),
                   atomic_load(&mc->hdr.send_calls) - atomic_load(&mc->hdr.send_full));
        cne_printf("     [orange]Recv[]: [cyan]%'15ld %'15ld %'15ld %'15ld[]\n",
                   atomic_load(&mc->hdr.recv_calls), atomic_load(&mc->hdr.recv_cnt),
                   atomic_load(&mc->hdr.recv_empty),
                   atomic_load(&mc->hdr.recv_calls) - atomic_load(&mc->hdr.recv_empty));
        if (mc->hdr.cookie == MC_PARENT_COOKIE) {
            cne_printf("     [magenta]Children[]: ");
            for (int i = 0; i < MC_MAX_CHILDREN; i++) {
                mc_child_t *child = &mc->children[i];

                if (child->hdr.cookie == MC_CHILD_COOKIE)
                    cne_printf(" [cyan]%s[]", child->hdr.name);
            }
            cne_printf("[]\n");
            cne_printf("     [magenta]Ring [orange]%s[]: [cyan]%'u [magenta]used entries[]\n",
                       cne_ring_get_name(mc->hdr.rings[MC_SEND_RING]),
                       cne_ring_count(mc->hdr.rings[MC_SEND_RING]));
            cne_printf("     [magenta]Ring [orange]%s[]: [cyan]%'u [magenta]used entries[]\n",
                       cne_ring_get_name(mc->hdr.rings[MC_RECV_RING]),
                       cne_ring_count(mc->hdr.rings[MC_RECV_RING]));
        }
    } else
        CNE_ERR("MsgChan is invalid\n");
    mc_list_unlock();
}

void
mc_list(void)
{
    struct mc_list *list       = NULL;
    struct cne_tailq_entry *te = NULL;

    mc_list_lock();

    list = CNE_TAILQ_CAST(mc_tailq.head, mc_list);

    cne_printf("[yellow]** [cyan]MsgChan [yellow]**[]\n");
    TAILQ_FOREACH (te, list, next)
        mc_dump(te->data);

    mc_list_unlock();
}

CNE_INIT_PRIO(mc_constructor, LAST)
{
    if (cne_mutex_create(&mc_list_mutex, PTHREAD_MUTEX_RECURSIVE))
        CNE_ERR("creating recursive mutex failed\n");
}
