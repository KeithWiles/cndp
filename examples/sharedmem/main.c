/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2020-2022 Intel Corporation
 */

#include <stdint.h>            // for uint64_t
#include <cne_common.h>        // for CNE_SET_USED, __cne_unused, cne_countof
#include <cne.h>
#include <cne_mmap.h>        // for mmap_sizes_t, mmap_stats_t
#include <cne_log.h>
#include <cli.h>        // for cli_path_string, cli_add_bin_path, cli_add_tree

#include "main.h"

struct app_info {
    volatile uint64_t stopped;
    int flags;
    mmap_t *mm;
};

struct app_info app_data, *app = &app_data;

#define SERVER_FLAG (1 << 0)

static void
usage(int err)
{
    cne_printf("[cyan]cli[]: [yellow]CLI Test example[]\n");
    cne_printf("  [magenta]Options[]:\n");
    cne_printf("    [yellow]-s,--server[]  - [green]Start as a server process[]\n");
    cne_printf("    [yellow]-c,--client[]  - [green]Start as a client process[]\n");
    cne_printf("    [yellow]-h,--help[]    - [green]This help message[]\n");
    exit(err);
}

static void *
server_func(void *arg __cne_unused)
{
    int ret;

    ret = pthread_detach(pthread_self());
    if (ret)
        CNE_ERR_GOTO(err, "pthread detach failed: %s\n", strerror(ret));

    return NULL;
}

static void *
client_func(void *arg __cne_unused)
{
    int ret;

    ret = pthread_detach(pthread_self());
    if (ret)
        CNE_ERR_GOTO(err, "pthread detach failed: %s\n", strerror(ret));

    return NULL;
}

int
main(int argc, char *argv[])
{
    // clang-format off
    struct option lgopts[] = {
        { "server",     no_argument, NULL, 's' },
        { "client",     no_argument, NULL, 'c' },
        { "help",       no_argument, NULL, 'h' },
        { NULL, 0, 0, 0 }
    };
    // clang-format on
    int option_index, opt;
    pthread_t thd;
    char c;

    option_index = 0;
    while ((opt = getopt_long(argc, argv, "hs:c:", lgopts, &option_index)) != -1) {
        switch (opt) {
        case 'h':
            usage(EXIT_SUCCESS);
            break;
        case 's': /* Server */
            app->flags |= SERVER_FLAG;
            break;
        case 'c': /* Client */
            break;
        default:
            break;
        }
    }

    if (cne_init() < 0)
        CNE_ERR_GOTO(out, "Unable to init CNE\n");

    cne_printf("[yellow]*** [green]Message Channel between processes,");
    cne_printf(" [cyan]Process is [orange]%s\n", (app->flags & SERVER_FLAG) ? "Server" : "Client");
    cne_printf("    [yellow]=== [deeppink]Press ESC key to exit [yellow]===[]\n");

    if (tty_setup(-1, -1) < 0)
        CNE_ERR_GOTO(out, "Unable to setup tty\n");

    err = pthread_create(&thd, NULL, (app->flags & SERVER_FLAG) ? server_func : client_func, NULL);
    if (err)
        CNE_ERR_GOTO(err_exit, "Unable to start thread: %s\n", strerror(err));

    for (;;) {
        if (tty_poll(&c, 1, 1000)) {
            if (c == 'q' || c == 'Q' || c == 27)
                break;
        }
    }
    tty_destroy();
    return 0;
out:
    tty_destroy();
    return -1;
}
