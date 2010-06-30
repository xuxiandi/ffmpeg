/*
 * Copyright (c) 2009 Baptiste Coudurier <baptiste.coudurier@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <unistd.h>
#include <fcntl.h>
#include "timer.h"
#include "random_seed.h"
#include "avutil.h"

static int read_random(uint32_t *dst, const char *file)
{
    int fd = open(file, O_RDONLY);
    int err = -1;

    if (fd == -1)
        return -1;
#if HAVE_FCNTL && defined(O_NONBLOCK)
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != -1)
#endif
        err = read(fd, dst, sizeof(*dst));
    close(fd);

    return err;
}

uint32_t av_get_random_seed(void)
{
    uint32_t seed;
    int err;

    err = read_random(&seed, "/dev/urandom");
    if (err != sizeof(seed))
        err = read_random(&seed, "/dev/random");
    if (err == sizeof(seed))
        return seed;

#ifdef AV_READ_TIME
    seed = AV_READ_TIME();
#endif
    // XXX what to do ?
    return seed;
}

#if LIBAVUTIL_VERSION_MAJOR < 51
attribute_deprecated uint32_t ff_random_get_seed(void);
uint32_t ff_random_get_seed(void)
{
    return av_get_random_seed();
}
#endif
