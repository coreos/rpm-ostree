#pragma once

/***
  This file was originally part of systemd.
  https://github.com/systemd/systemd/blob/8b212f3596d03f8e1025cd151d17f9a82433844a/src/basic/time-util.h

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

G_BEGIN_DECLS

typedef uint64_t usec_t;
typedef uint64_t nsec_t;

#define PRI_NSEC PRIu64
#define PRI_USEC PRIu64
#define NSEC_FMT "%" PRI_NSEC
#define USEC_FMT "%" PRI_USEC

#define USEC_INFINITY ((usec_t)UINT64_MAX)
#define NSEC_INFINITY ((nsec_t)UINT64_MAX)

#define MSEC_PER_SEC 1000ULL
#define USEC_PER_SEC ((usec_t)1000000ULL)
#define USEC_PER_MSEC ((usec_t)1000ULL)
#define NSEC_PER_SEC ((nsec_t)1000000000ULL)
#define NSEC_PER_MSEC ((nsec_t)1000000ULL)
#define NSEC_PER_USEC ((nsec_t)1000ULL)

#define USEC_PER_MINUTE ((usec_t)(60ULL * USEC_PER_SEC))
#define NSEC_PER_MINUTE ((nsec_t)(60ULL * NSEC_PER_SEC))
#define USEC_PER_HOUR ((usec_t)(60ULL * USEC_PER_MINUTE))
#define NSEC_PER_HOUR ((nsec_t)(60ULL * NSEC_PER_MINUTE))
#define USEC_PER_DAY ((usec_t)(24ULL * USEC_PER_HOUR))
#define NSEC_PER_DAY ((nsec_t)(24ULL * NSEC_PER_HOUR))
#define USEC_PER_WEEK ((usec_t)(7ULL * USEC_PER_DAY))
#define NSEC_PER_WEEK ((nsec_t)(7ULL * NSEC_PER_DAY))
#define USEC_PER_MONTH ((usec_t)(2629800ULL * USEC_PER_SEC))
#define NSEC_PER_MONTH ((nsec_t)(2629800ULL * NSEC_PER_SEC))
#define USEC_PER_YEAR ((usec_t)(31557600ULL * USEC_PER_SEC))
#define NSEC_PER_YEAR ((nsec_t)(31557600ULL * NSEC_PER_SEC))

/* We assume a maximum timezone length of 6. TZNAME_MAX is not defined on Linux, but glibc
 * internally initializes this to 6. Let's rely on that. */
#define FORMAT_TIMESTAMP_MAX (3U + 1U + 10U + 1U + 8U + 1U + 6U + 1U + 6U + 1U)
#define FORMAT_TIMESTAMP_WIDTH 28U /* when outputting, assume this width */
#define FORMAT_TIMESTAMP_RELATIVE_MAX 256U
#define FORMAT_TIMESPAN_MAX 64U

char *libsd_format_timestamp_relative (char *buf, size_t l, usec_t t);

G_END_DECLS
