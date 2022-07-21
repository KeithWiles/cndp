# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2022 Intel Corporation

sources = files('main.c')

deps += [mmap, msgchan]

executable('sharedmem',
    sources,
    c_args: cflags,
    install: true,
    dependencies: deps)
