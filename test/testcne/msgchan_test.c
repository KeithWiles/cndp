/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2022-2023 Intel Corporation.
 */

#include <stdio.h>         // for NULL, snprintf, EOF
#include <string.h>        // for strcmp, strncmp
#include <getopt.h>        // for getopt_long, option
#include <pthread.h>
#include <cne_common.h>        // for CNE_SET_USED, __cne_unused
#include <tst_info.h>          // for TST_ASSERT_EQUAL_AND_CLEANUP, tst_end, TST_A...
#include <msgchan.h>

#include "msgchan_test.h"

#define MSG_CHAN_SIZE       2048
#define MC_SERVER_NAME      "test3"
#define DEFAULT_NUM_THREADS 5

static atomic_uint_fast32_t clients_done;
static volatile int serror, cerror;
static int verbose;
static unsigned long int num_threads;

static inline void
set_object_values(void **objs, int count, int start_val)
{
    for (int i = 0; i < count; i++)
        objs[i] = (void *)(uintptr_t)start_val;
}

static inline int
tst_object_values(void **objs, int count, int start_val)
{
    for (int i = 0; i < count; i++) {
        if (objs[i] != (void *)(uintptr_t)start_val)
            return -1;
    }
    return 0;
}

static int
test1(void)
{
    int sizes[]                       = {64, 128, 256, 512, 1024, 2048, 4096, 8192};
    msgchan_t *mc[cne_countof(sizes)] = {0};
    char name[64]                     = {0};
    int mc_cnt                        = cne_countof(mc);

    memset(mc, 0, sizeof(mc));

    for (int i = 0; i < mc_cnt; i++) {
        snprintf(name, sizeof(name), "test-%d", i);

        mc[i] = mc_create(name, 0, sizes[i], 0);
        if (!mc[i])
            goto err;
    }

    for (int i = 0; i < mc_cnt; i++) {
        if (mc[i])
            mc_destroy(mc[i]);
    }
    return 0;
err:
    for (int i = 0; i < mc_cnt; i++) {
        if (mc[i])
            mc_destroy(mc[i]);
    }
    return -1;
}

static int
test2(void)
{
    msgchan_t *mc1 = NULL, *mc2 = NULL;
    int counts[] = {1, 4, 7, 8, 16, 32, 63, 64, 132, 256};
    void *objs[256], *robjs[256];

    /* Create a parent and child msgchan objects */
    mc1 = mc_create("test2", 0, MSG_CHAN_SIZE, 0);
    if (!mc1)
        CNE_ERR_GOTO(err, "mc_create(test2) failed\n");

    mc2 = mc_create("test2", 0, MSG_CHAN_SIZE, 0);
    if (!mc2)
        CNE_ERR_GOTO(err, "mc_create(test3) failed\n");

    if (verbose)
        mc_list();

    for (int i = 0; i < cne_countof(counts); i++) {
        int count = counts[i];
        int n;

        if (verbose)
            cne_printf("   [cyan]Test [green]%4d [cyan]object count[]\n", count);

        memset(objs, 0, (sizeof(void *) * count));

        set_object_values(objs, count, 0x1234);

        n = mc_send(mc1, objs, count);
        if (n < 0)
            CNE_ERR_GOTO(err, "mc_send() failed: %d\n", n);
        if (n != count)
            CNE_ERR_GOTO(err, "Send %d objs did not match expected %d\n", count, n);

        memset(robjs, 0, sizeof(robjs));
        n = mc_recv(mc2, robjs, count, 0);
        if (n < 0)
            CNE_ERR_GOTO(err, "mc_recv() failed: %d\n", n);
        if (n != count)
            CNE_ERR_GOTO(err, "Recv %d objs did not match expected %d\n", count, n);

        if (tst_object_values(robjs, n, 0x1234))
            CNE_ERR_GOTO(err, "Value returned is invalid\n");
    }

    if (verbose) {
        mc_dump(mc1);
        mc_dump(mc2);
    }

    mc_destroy(mc2);
    mc_destroy(mc1);

    return 0;
err:
    mc_destroy(mc2);
    mc_destroy(mc1);
    return -1;
}

static int
test3(void)
{
    msgchan_t *mc1 = NULL, *mc2 = NULL;
    int counts[] = {1, 4, 7, 8, 16, 32, 63, 64, 132, 256};
    void *objs[256], *robjs[256];
    char *mc_addr1 = NULL, *mc_addr2 = NULL;
    ssize_t mc_size;

    mc_size = mc_get_total_memsize(0, MSG_CHAN_SIZE);
    if (mc_size < 0)
        CNE_ERR_GOTO(err, "mc_get_total_memsize(%d, %d) failed\n", 0, MSG_CHAN_SIZE);

    mc_addr1 = malloc(mc_size);
    if (!mc_addr1)
        CNE_ERR_GOTO(err, "malloc(%zd) failed\n", mc_size);

    /* Create a parent and child msgchan objects */
    mc1 = mc_init(mc_addr1, mc_size, "test2", 0, MSG_CHAN_SIZE, 0);
    if (!mc1)
        CNE_ERR_GOTO(err, "mc_init(test2) failed\n");

    mc_addr2 = malloc(mc_size);
    if (!mc_addr2)
        CNE_ERR_GOTO(err, "malloc(%zd) failed\n", mc_size);

    mc2 = mc_init(mc_addr2, mc_size, "test2", 0, MSG_CHAN_SIZE, 0);
    if (!mc2)
        CNE_ERR_GOTO(err, "mc_create(test3) failed\n");

    if (verbose)
        mc_list();

    for (int i = 0; i < cne_countof(counts); i++) {
        int count = counts[i];
        int n;

        if (verbose)
            cne_printf("   [cyan]Test [green]%4d [cyan]object count[]\n", count);

        memset(objs, 0, (sizeof(void *) * count));

        set_object_values(objs, count, 0x1234);

        n = mc_send(mc1, objs, count);
        if (n < 0)
            CNE_ERR_GOTO(err, "mc_send() failed: %d\n", n);
        if (n != count)
            CNE_ERR_GOTO(err, "Send %d objs did not match expected %d\n", count, n);

        memset(robjs, 0, sizeof(robjs));
        n = mc_recv(mc2, robjs, count, 0);
        if (n < 0)
            CNE_ERR_GOTO(err, "mc_recv() failed: %d\n", n);
        if (n != count)
            CNE_ERR_GOTO(err, "Recv %d objs did not match expected %d\n", count, n);

        if (tst_object_values(robjs, n, 0x1234))
            CNE_ERR_GOTO(err, "Value returned is invalid\n");
    }

    if (verbose) {
        mc_dump(mc1);
        mc_dump(mc2);
    }

    mc_destroy(mc2);
    free(mc_addr2);
    mc_destroy(mc1);
    free(mc_addr1);

    return 0;
err:
    mc_destroy(mc2);
    free(mc_addr2);
    mc_destroy(mc1);
    free(mc_addr1);
    return -1;
}

static void *
server_func(void *arg __cne_unused)
{
    msgchan_t *mc = NULL;
    uint64_t vals[128];
    int ret;

    ret = pthread_detach(pthread_self());
    if (ret)
        CNE_ERR_GOTO(err, "pthread detach failed: %s\n", strerror(ret));

    mc = mc_create(MC_SERVER_NAME, 0, MSG_CHAN_SIZE, 0);
    if (!mc)
        CNE_ERR_GOTO(err, "mc_create('%s') failed\n", MC_SERVER_NAME);

    if (verbose)
        cne_printf("  [orange]>>> [magenta]Server started, waiting for client thread, msgchan: "
                   "[cyan]%s[]\n",
                   mc_name(mc));

    for (;;) {
        int n;

        n = mc_recv(mc, (void **)vals, cne_countof(vals), 0);
        if (n < 0)
            CNE_ERR_GOTO(err, " [orange]Server[] [red]Received error[]\n");
        if (n) {
            int nn;

            do {
                nn = mc_send(mc, (void **)vals, n);
                if (nn < 0)
                    CNE_ERR_GOTO(err, "[orange]mc_send()[] returned error\n");
                n -= nn;
            } while (n > 0);
        }
        if (atomic_load(&clients_done) == (num_threads - 1))
            break;
    }

    if (verbose)
        cne_printf("  [orange]<<< [magenta]Server exiting[]\n");

    if (!verbose)
        mc_dump(mc);
    mc_destroy(mc);
    atomic_fetch_add(&clients_done, 1);
    return NULL;
err:
    serror = -1;
    mc_destroy(mc);
    atomic_fetch_add(&clients_done, 1);
    return NULL;
}

#define SEND_COUNT 2000

static void *
client_func(void *arg __cne_unused)
{
    msgchan_t *mc = NULL;
    int counts[]  = {1, 4, 8, 16, 32, 64, 128, 132, 256};
    void *vals[256], *rvals[256];
    int n, ret, nb;

    ret = pthread_detach(pthread_self());
    if (ret)
        CNE_ERR_GOTO(err, "pthread detach failed: %s\n", strerror(ret));

    /* Wait for msgchan server to be created */
    while ((mc = mc_lookup(MC_SERVER_NAME)) == NULL)
        usleep(1000);

    /* Create the Child msgchan */
    mc = mc_create(MC_SERVER_NAME, 0, MSG_CHAN_SIZE, 0);
    if (!mc)
        CNE_ERR_GOTO(err, "mc_create('%s') failed\n", MC_SERVER_NAME);

    if (verbose)
        cne_printf("  [orange]>>> [magenta]Client started, waiting for server thread, msgchan: "
                   "[cyan]%s[]\n",
                   mc_name(mc));

    for (int j = 0; j < cne_countof(counts); j++) {
        int cnt, scnt;

        cnt = counts[j];
        for (int i = 0; i < SEND_COUNT; i++) {
            int k;

            set_object_values(vals, cne_countof(vals), 0xfeedbeef);

            k    = 0;
            scnt = cnt;
            while (scnt) {
                n = mc_send(mc, &vals[k], scnt);
                if (n < 0)
                    CNE_ERR_GOTO(err, "  [magenta]Client Send [green]%'3d[] != [green]%'d[]\n", n,
                                 scnt);
                scnt -= n;
                k += n;
            }

            memset(rvals, 0x55, sizeof(rvals));

            nb = mc_recv(mc, (void **)rvals, cne_countof(rvals), 0);
            if (nb < 0 || tst_object_values(rvals, nb, 0xfeedbeef))
                CNE_ERR_GOTO(err, "  [magenta]Client failed[]\n");
        }
    }

    n = 10; /* Wait for 10ms */

    /* Drain the receive ring */
    for (;;) {
        nb = mc_recv(mc, (void **)rvals, cne_countof(rvals), 0);
        if (nb < 0 || tst_object_values(rvals, nb, 0xfeedbeef))
            CNE_ERR_GOTO(err, "  [magenta]Client failed[]\n");
        if (mc_recv_count(mc) == 0) {
            if (--n == 0)
                break;
            usleep(1000);
        }
    }

    if (verbose)
        cne_printf("  [orange]<<< [magenta]Client exiting[]\n");

    if (!verbose)
        mc_dump(mc);

    mc_destroy(mc);

    atomic_fetch_add(&clients_done, 1);
    return NULL;
err:
    cerror = -1;
    mc_destroy(mc);

    atomic_fetch_add(&clients_done, 1);
    return NULL;
}

static int
test4(void)
{
    pthread_t thd;
    int err;

    serror = cerror = 0;

    atomic_store(&clients_done, 0);

    cne_printf("[yellow]Number of threads [cyan]%ld[]\n", num_threads);

    for (unsigned long int i = 0; i < num_threads; i++) {
        err = pthread_create(&thd, NULL, (i == 0) ? server_func : client_func, NULL);
        if (err)
            CNE_ERR_GOTO(err_exit, "Unable to start client thread: %s\n", strerror(err));
    }

    for (;;) {
        if (atomic_load(&clients_done) == num_threads)
            break;
        usleep(1000);
    }
    atomic_store(&clients_done, num_threads);

    return (serror || cerror) ? -1 : 0;

err_exit:
    atomic_store(&clients_done, num_threads);

    return (serror || cerror) ? -1 : 0;
}

int
msgchan_main(int argc, char **argv)
{
    tst_info_t *tst;
    int opt;
    char **argvopt;
    int option_index;
    static const struct option lgopts[] = {{NULL, 0, 0, 0}};

    argvopt = argv;

    verbose     = 0;
    num_threads = DEFAULT_NUM_THREADS;
    while ((opt = getopt_long(argc, argvopt, "Vt:", lgopts, &option_index)) != EOF) {
        switch (opt) {
        case 'V':
            verbose = 1;
            break;
        case 't':
            num_threads = atoi(optarg);
            if (num_threads < 2)
                num_threads = DEFAULT_NUM_THREADS;
            break;
        default:
            break;
        }
    }
    CNE_SET_USED(verbose);

    tst = tst_start("MsgChan Create/List/Destroy");
    if (test1() < 0)
        goto leave;
    tst_end(tst, TST_PASSED);

    tst = tst_start("MsgChan Server multiple sizes");
    if (test2() < 0)
        goto leave;
    tst_end(tst, TST_PASSED);

    tst = tst_start("MsgChan user memory");
    if (test3() < 0)
        goto leave;
    tst_end(tst, TST_PASSED);

    tst = tst_start("MsgChan Server/Client");
    if (test4() < 0)
        goto leave;
    tst_end(tst, TST_PASSED);

    return 0;
leave:
    tst_end(tst, TST_FAILED);

    return -1;
}
