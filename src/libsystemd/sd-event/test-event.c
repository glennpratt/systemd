/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

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

#include <sys/wait.h>

#include "sd-event.h"

#include "fd-util.h"
#include "log.h"
#include "macro.h"
#include "signal-util.h"
#include "util.h"

static int prepare_handler(sd_event_source *s, void *userdata) {
        log_info("preparing %c", PTR_TO_INT(userdata));
        return 1;
}

static bool got_a, got_b, got_c, got_unref;
static unsigned got_d;

static int unref_handler(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        sd_event_source_unref(s);
        got_unref = true;
        return 0;
}

static int io_handler(sd_event_source *s, int fd, uint32_t revents, void *userdata) {

        log_info("got IO on %c", PTR_TO_INT(userdata));

        if (userdata == INT_TO_PTR('a')) {
                assert_se(sd_event_source_set_enabled(s, SD_EVENT_OFF) >= 0);
                assert_se(!got_a);
                got_a = true;
        } else if (userdata == INT_TO_PTR('b')) {
                assert_se(!got_b);
                got_b = true;
        } else if (userdata == INT_TO_PTR('d')) {
                got_d++;
                if (got_d < 2)
                        assert_se(sd_event_source_set_enabled(s, SD_EVENT_ONESHOT) >= 0);
                else
                        assert_se(sd_event_source_set_enabled(s, SD_EVENT_OFF) >= 0);
        } else
                assert_not_reached("Yuck!");

        return 1;
}

static int child_handler(sd_event_source *s, const siginfo_t *si, void *userdata) {

        assert_se(s);
        assert_se(si);

        log_info("got child on %c", PTR_TO_INT(userdata));

        assert_se(userdata == INT_TO_PTR('f'));

        assert_se(sd_event_exit(sd_event_source_get_event(s), 0) >= 0);
        sd_event_source_unref(s);

        return 1;
}

static int signal_handler(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata) {
        sd_event_source *p = NULL;
        pid_t pid;

        assert_se(s);
        assert_se(si);

        log_info("got signal on %c", PTR_TO_INT(userdata));

        assert_se(userdata == INT_TO_PTR('e'));

        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGCHLD, -1) >= 0);

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0)
                _exit(0);

        assert_se(sd_event_add_child(sd_event_source_get_event(s), &p, pid, WEXITED, child_handler, INT_TO_PTR('f')) >= 0);
        assert_se(sd_event_source_set_enabled(p, SD_EVENT_ONESHOT) >= 0);

        sd_event_source_unref(s);

        return 1;
}

static int defer_handler(sd_event_source *s, void *userdata) {
        sd_event_source *p = NULL;

        assert_se(s);

        log_info("got defer on %c", PTR_TO_INT(userdata));

        assert_se(userdata == INT_TO_PTR('d'));

        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGUSR1, -1) >= 0);

        assert_se(sd_event_add_signal(sd_event_source_get_event(s), &p, SIGUSR1, signal_handler, INT_TO_PTR('e')) >= 0);
        assert_se(sd_event_source_set_enabled(p, SD_EVENT_ONESHOT) >= 0);
        raise(SIGUSR1);

        sd_event_source_unref(s);

        return 1;
}

static bool do_quit = false;

static int time_handler(sd_event_source *s, uint64_t usec, void *userdata) {
        log_info("got timer on %c", PTR_TO_INT(userdata));

        if (userdata == INT_TO_PTR('c')) {

                if (do_quit) {
                        sd_event_source *p;

                        assert_se(sd_event_add_defer(sd_event_source_get_event(s), &p, defer_handler, INT_TO_PTR('d')) >= 0);
                        assert_se(sd_event_source_set_enabled(p, SD_EVENT_ONESHOT) >= 0);
                } else {
                        assert_se(!got_c);
                        got_c = true;
                }
        } else
                assert_not_reached("Huh?");

        return 2;
}

static bool got_exit = false;

static int exit_handler(sd_event_source *s, void *userdata) {
        log_info("got quit handler on %c", PTR_TO_INT(userdata));

        got_exit = true;

        return 3;
}

static bool got_post = false;

static int post_handler(sd_event_source *s, void *userdata) {
        log_info("got post handler");

        got_post = true;

        return 2;
}

static void test_basic(void) {
        sd_event *e = NULL;
        sd_event_source *w = NULL, *x = NULL, *y = NULL, *z = NULL, *q = NULL, *t = NULL;
        static const char ch = 'x';
        int a[2] = { -1, -1 }, b[2] = { -1, -1}, d[2] = { -1, -1}, k[2] = { -1, -1 };
        uint64_t event_now;
        int64_t priority;

        assert_se(pipe(a) >= 0);
        assert_se(pipe(b) >= 0);
        assert_se(pipe(d) >= 0);
        assert_se(pipe(k) >= 0);

        assert_se(sd_event_default(&e) >= 0);
        assert_se(sd_event_now(e, CLOCK_MONOTONIC, &event_now) > 0);

        assert_se(sd_event_set_watchdog(e, true) >= 0);

        /* Test whether we cleanly can destroy an io event source from its own handler */
        got_unref = false;
        assert_se(sd_event_add_io(e, &t, k[0], EPOLLIN, unref_handler, NULL) >= 0);
        assert_se(write(k[1], &ch, 1) == 1);
        assert_se(sd_event_run(e, (uint64_t) -1) >= 1);
        assert_se(got_unref);

        got_a = false, got_b = false, got_c = false, got_d = 0;

        /* Add a oneshot handler, trigger it, re-enable it, and trigger
         * it again. */
        assert_se(sd_event_add_io(e, &w, d[0], EPOLLIN, io_handler, INT_TO_PTR('d')) >= 0);
        assert_se(sd_event_source_set_enabled(w, SD_EVENT_ONESHOT) >= 0);
        assert_se(write(d[1], &ch, 1) >= 0);
        assert_se(sd_event_run(e, (uint64_t) -1) >= 1);
        assert_se(got_d == 1);
        assert_se(write(d[1], &ch, 1) >= 0);
        assert_se(sd_event_run(e, (uint64_t) -1) >= 1);
        assert_se(got_d == 2);

        assert_se(sd_event_add_io(e, &x, a[0], EPOLLIN, io_handler, INT_TO_PTR('a')) >= 0);
        assert_se(sd_event_add_io(e, &y, b[0], EPOLLIN, io_handler, INT_TO_PTR('b')) >= 0);
        assert_se(sd_event_add_time(e, &z, CLOCK_MONOTONIC, 0, 0, time_handler, INT_TO_PTR('c')) >= 0);
        assert_se(sd_event_add_exit(e, &q, exit_handler, INT_TO_PTR('g')) >= 0);

        assert_se(sd_event_source_set_priority(x, 99) >= 0);
        assert_se(sd_event_source_get_priority(x, &priority) >= 0);
        assert_se(priority == 99);
        assert_se(sd_event_source_set_enabled(y, SD_EVENT_ONESHOT) >= 0);
        assert_se(sd_event_source_set_prepare(x, prepare_handler) >= 0);
        assert_se(sd_event_source_set_priority(z, 50) >= 0);
        assert_se(sd_event_source_set_enabled(z, SD_EVENT_ONESHOT) >= 0);
        assert_se(sd_event_source_set_prepare(z, prepare_handler) >= 0);

        /* Test for floating event sources */
        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGRTMIN+1, -1) >= 0);
        assert_se(sd_event_add_signal(e, NULL, SIGRTMIN+1, NULL, NULL) >= 0);

        assert_se(write(a[1], &ch, 1) >= 0);
        assert_se(write(b[1], &ch, 1) >= 0);

        assert_se(!got_a && !got_b && !got_c);

        assert_se(sd_event_run(e, (uint64_t) -1) >= 1);

        assert_se(!got_a && got_b && !got_c);

        assert_se(sd_event_run(e, (uint64_t) -1) >= 1);

        assert_se(!got_a && got_b && got_c);

        assert_se(sd_event_run(e, (uint64_t) -1) >= 1);

        assert_se(got_a && got_b && got_c);

        sd_event_source_unref(x);
        sd_event_source_unref(y);

        do_quit = true;
        assert_se(sd_event_add_post(e, NULL, post_handler, NULL) >= 0);
        assert_se(sd_event_now(e, CLOCK_MONOTONIC, &event_now) == 0);
        assert_se(sd_event_source_set_time(z, event_now + 200 * USEC_PER_MSEC) >= 0);
        assert_se(sd_event_source_set_enabled(z, SD_EVENT_ONESHOT) >= 0);

        assert_se(sd_event_loop(e) >= 0);
        assert_se(got_post);
        assert_se(got_exit);

        sd_event_source_unref(z);
        sd_event_source_unref(q);

        sd_event_source_unref(w);

        sd_event_unref(e);

        safe_close_pair(a);
        safe_close_pair(b);
        safe_close_pair(d);
        safe_close_pair(k);
}

static void test_sd_event_now(void) {
        _cleanup_(sd_event_unrefp) sd_event *e = NULL;
        uint64_t event_now;

        assert_se(sd_event_new(&e) >= 0);
        assert_se(sd_event_now(e, CLOCK_MONOTONIC, &event_now) > 0);
        assert_se(sd_event_now(e, CLOCK_REALTIME, &event_now) > 0);
        assert_se(sd_event_now(e, CLOCK_REALTIME_ALARM, &event_now) > 0);
        if (clock_boottime_supported()) {
                assert_se(sd_event_now(e, CLOCK_BOOTTIME, &event_now) > 0);
                assert_se(sd_event_now(e, CLOCK_BOOTTIME_ALARM, &event_now) > 0);
        }
        assert_se(sd_event_now(e, -1, &event_now) == -EOPNOTSUPP);
        assert_se(sd_event_now(e, 900 /* arbitrary big number */, &event_now) == -EOPNOTSUPP);

        assert_se(sd_event_run(e, 0) == 0);

        assert_se(sd_event_now(e, CLOCK_MONOTONIC, &event_now) == 0);
        assert_se(sd_event_now(e, CLOCK_REALTIME, &event_now) == 0);
        assert_se(sd_event_now(e, CLOCK_REALTIME_ALARM, &event_now) == 0);
        if (clock_boottime_supported()) {
                assert_se(sd_event_now(e, CLOCK_BOOTTIME, &event_now) == 0);
                assert_se(sd_event_now(e, CLOCK_BOOTTIME_ALARM, &event_now) == 0);
        }
        assert_se(sd_event_now(e, -1, &event_now) == -EOPNOTSUPP);
        assert_se(sd_event_now(e, 900 /* arbitrary big number */, &event_now) == -EOPNOTSUPP);
}

static int last_rtqueue_sigval = 0;
static int n_rtqueue = 0;

static int rtqueue_handler(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata) {
        last_rtqueue_sigval = si->ssi_int;
        n_rtqueue++;
        return 0;
}

static void test_rtqueue(void) {
        sd_event_source *u = NULL, *v = NULL, *s = NULL;
        sd_event *e = NULL;

        assert_se(sd_event_default(&e) >= 0);

        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGRTMIN+2, SIGRTMIN+3, SIGUSR2, -1) >= 0);
        assert_se(sd_event_add_signal(e, &u, SIGRTMIN+2, rtqueue_handler, NULL) >= 0);
        assert_se(sd_event_add_signal(e, &v, SIGRTMIN+3, rtqueue_handler, NULL) >= 0);
        assert_se(sd_event_add_signal(e, &s, SIGUSR2, rtqueue_handler, NULL) >= 0);

        assert_se(sd_event_source_set_priority(v, -10) >= 0);

        assert_se(sigqueue(getpid_cached(), SIGRTMIN+2, (union sigval) { .sival_int = 1 }) >= 0);
        assert_se(sigqueue(getpid_cached(), SIGRTMIN+3, (union sigval) { .sival_int = 2 }) >= 0);
        assert_se(sigqueue(getpid_cached(), SIGUSR2, (union sigval) { .sival_int = 3 }) >= 0);
        assert_se(sigqueue(getpid_cached(), SIGRTMIN+3, (union sigval) { .sival_int = 4 }) >= 0);
        assert_se(sigqueue(getpid_cached(), SIGUSR2, (union sigval) { .sival_int = 5 }) >= 0);

        assert_se(n_rtqueue == 0);
        assert_se(last_rtqueue_sigval == 0);

        assert_se(sd_event_run(e, (uint64_t) -1) >= 1);
        assert_se(n_rtqueue == 1);
        assert_se(last_rtqueue_sigval == 2); /* first SIGRTMIN+3 */

        assert_se(sd_event_run(e, (uint64_t) -1) >= 1);
        assert_se(n_rtqueue == 2);
        assert_se(last_rtqueue_sigval == 4); /* second SIGRTMIN+3 */

        assert_se(sd_event_run(e, (uint64_t) -1) >= 1);
        assert_se(n_rtqueue == 3);
        assert_se(last_rtqueue_sigval == 3); /* first SIGUSR2 */

        assert_se(sd_event_run(e, (uint64_t) -1) >= 1);
        assert_se(n_rtqueue == 4);
        assert_se(last_rtqueue_sigval == 1); /* SIGRTMIN+2 */

        assert_se(sd_event_run(e, 0) == 0); /* the other SIGUSR2 is dropped, because the first one was still queued */
        assert_se(n_rtqueue == 4);
        assert_se(last_rtqueue_sigval == 1);

        sd_event_source_unref(u);
        sd_event_source_unref(v);
        sd_event_source_unref(s);

        sd_event_unref(e);
}

int main(int argc, char *argv[]) {

        log_set_max_level(LOG_DEBUG);
        log_parse_environment();

        test_basic();
        test_sd_event_now();
        test_rtqueue();

        return 0;
}
