/***
  This file was originally part of systemd.

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

#include <glib.h>
#include <sys/time.h>

#include "libsd-time-util.h"

static clockid_t map_clock_id(clockid_t c) {

        /* Some more exotic archs (s390, ppc, …) lack the "ALARM" flavour of the clocks. Thus, clock_gettime() will
         * fail for them. Since they are essentially the same as their non-ALARM pendants (their only difference is
         * when timers are set on them), let's just map them accordingly. This way, we can get the correct time even on
         * those archs. */

        switch (c) {

        case CLOCK_BOOTTIME_ALARM:
                return CLOCK_BOOTTIME;

        case CLOCK_REALTIME_ALARM:
                return CLOCK_REALTIME;

        default:
                return c;
        }
}

static usec_t timespec_load(const struct timespec *ts) {
        g_assert(ts);

        if (ts->tv_sec < 0 || ts->tv_nsec < 0)
                return USEC_INFINITY;

        if ((usec_t) ts->tv_sec > (UINT64_MAX - (ts->tv_nsec / NSEC_PER_USEC)) / USEC_PER_SEC)
                return USEC_INFINITY;

        return
                (usec_t) ts->tv_sec * USEC_PER_SEC +
                (usec_t) ts->tv_nsec / NSEC_PER_USEC;
}

static usec_t now(clockid_t clock_id) {
        struct timespec ts;

        g_assert_cmpint (clock_gettime(map_clock_id(clock_id), &ts), ==, 0);

        return timespec_load(&ts);
}

char *libsd_format_timestamp_relative(char *buf, size_t l, usec_t t) {
        const char *s;
        usec_t n, d;

        if (t <= 0 || t == USEC_INFINITY)
                return NULL;

        n = now(CLOCK_REALTIME);
        if (n > t) {
                d = n - t;
                s = "ago";
        } else {
                d = t - n;
                s = "left";
        }

        if (d >= USEC_PER_YEAR)
                snprintf(buf, l, USEC_FMT " years " USEC_FMT " months %s",
                         d / USEC_PER_YEAR,
                         (d % USEC_PER_YEAR) / USEC_PER_MONTH, s);
        else if (d >= USEC_PER_MONTH)
                snprintf(buf, l, USEC_FMT " months " USEC_FMT " days %s",
                         d / USEC_PER_MONTH,
                         (d % USEC_PER_MONTH) / USEC_PER_DAY, s);
        else if (d >= USEC_PER_WEEK)
                snprintf(buf, l, USEC_FMT " weeks " USEC_FMT " days %s",
                         d / USEC_PER_WEEK,
                         (d % USEC_PER_WEEK) / USEC_PER_DAY, s);
        else if (d >= 2*USEC_PER_DAY)
                snprintf(buf, l, USEC_FMT " days %s", d / USEC_PER_DAY, s);
        else if (d >= 25*USEC_PER_HOUR)
                snprintf(buf, l, "1 day " USEC_FMT "h %s",
                         (d - USEC_PER_DAY) / USEC_PER_HOUR, s);
        else if (d >= 6*USEC_PER_HOUR)
                snprintf(buf, l, USEC_FMT "h %s",
                         d / USEC_PER_HOUR, s);
        else if (d >= USEC_PER_HOUR)
                snprintf(buf, l, USEC_FMT "h " USEC_FMT "min %s",
                         d / USEC_PER_HOUR,
                         (d % USEC_PER_HOUR) / USEC_PER_MINUTE, s);
        else if (d >= 5*USEC_PER_MINUTE)
                snprintf(buf, l, USEC_FMT "min %s",
                         d / USEC_PER_MINUTE, s);
        else if (d >= USEC_PER_MINUTE)
                snprintf(buf, l, USEC_FMT "min " USEC_FMT "s %s",
                         d / USEC_PER_MINUTE,
                         (d % USEC_PER_MINUTE) / USEC_PER_SEC, s);
        else if (d >= USEC_PER_SEC)
                snprintf(buf, l, USEC_FMT "s %s",
                         d / USEC_PER_SEC, s);
        else if (d >= USEC_PER_MSEC)
                snprintf(buf, l, USEC_FMT "ms %s",
                         d / USEC_PER_MSEC, s);
        else if (d > 0)
                snprintf(buf, l, USEC_FMT"us %s",
                         d, s);
        else
                snprintf(buf, l, "now");

        buf[l-1] = 0;
        return buf;
}
