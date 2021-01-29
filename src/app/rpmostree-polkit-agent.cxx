/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2011 Lennart Poettering <lennart@poettering.net>
 * Copyright (C) 2012 Matthias Klumpp <matthias@tenstral.net>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with systemd; If not, see <http://www.gnu.org/licenses/>.
 */

/* snipped from PackageKit/systemd */

#include <stdio.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>
#include <inttypes.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <glib.h>

#include "rpmostree-polkit-agent.h"

#define memzero(x,l) (memset((x), 0, (l)))
#define zero(x) (memzero(&(x), sizeof(x)))

#define POLKIT_TTY_AGENT_BINARY_PATH "/usr/bin/pkttyagent"

static pid_t agent_pid = 0;

static int
fork_agent (pid_t *pid, const char *path, ...)
{
  pid_t parent_pid, n_agent_pid;
  int fd;
  gboolean stdout_is_tty, stderr_is_tty;
  unsigned n, i;
  va_list ap;
  char **l;

  g_assert (pid != 0);
  g_assert (path);

  parent_pid = getpid ();

  /* Spawns a temporary TTY agent, making sure it goes away when
   * we go away */

  n_agent_pid = fork ();
  if (n_agent_pid < 0)
    return -errno;

  if (n_agent_pid != 0)
    {
      *pid = n_agent_pid;
      return 0;
    }

  /* In the child:
   *
   * Make sure the agent goes away when the parent dies */
  if (prctl (PR_SET_PDEATHSIG, SIGTERM) < 0)
    err (EXIT_FAILURE, "prctl");

  /* Check whether our parent died before we were able
   * to set the death signal */
  if (getppid () != parent_pid)
    _exit (EXIT_SUCCESS);

  /* TODO: it might be more clean to close all FDs so we don't leak them to the agent */

  stdout_is_tty = isatty (STDOUT_FILENO);
  stderr_is_tty = isatty (STDERR_FILENO);

  if (!stdout_is_tty || !stderr_is_tty)
    {
      /* Detach from stdout/stderr. and reopen
       * /dev/tty for them. This is important to
       * ensure that when systemctl is started via
       * popen() or a similar call that expects to
       * read EOF we actually do generate EOF and
       * not delay this indefinitely by because we
       * keep an unused copy of stdin around. */
      fd = open("/dev/tty", O_WRONLY);
      if (fd < 0)
        err (EXIT_FAILURE, "Failed to open /dev/tty");

      if (!stdout_is_tty)
        dup2(fd, STDOUT_FILENO);

      if (!stderr_is_tty)
        dup2(fd, STDERR_FILENO);

      if (fd > 2)
        close(fd);
    }

  /* Count arguments */
  va_start (ap, path);
  for (n = 0; va_arg(ap, char*); n++)
    ;
  va_end(ap);

  /* Allocate strv */
  l = (char**)alloca (sizeof(char *) * (n + 1));

  /* Fill in arguments */
  va_start (ap, path);
  for (i = 0; i <= n; i++)
    l[i] = va_arg (ap, char*);
  va_end (ap);

  execv (path, l);
  err (EXIT_FAILURE, "Failed to exec %s", path);
}

static int
close_nointr (int fd)
{
  g_assert (fd >= 0);

  for (;;)
    {
      int r;

      r = close (fd);
      if (r >= 0)
        return r;

      if (errno != EINTR)
        return -errno;
    }
}

static void
close_nointr_nofail (int fd)
{
  int saved_errno = errno;

  /* cannot fail, and guarantees errno is unchanged */

  g_assert (close_nointr (fd) == 0);

  errno = saved_errno;
}

static int
fd_wait_for_event (int fd, int event, uint64_t t)
{
  struct pollfd pollfd;
  int r;

  zero(pollfd);
  pollfd.fd = fd;
  pollfd.events = event;

  r = poll(&pollfd, 1, t == (uint64_t) -1 ? -1 : (int) (t / 1000));
  if (r < 0)
    return -errno;

  if (r == 0)
    return 0;

  return pollfd.revents;
}

static int
wait_for_terminate (pid_t pid)
{
  int status;
  g_assert (pid >= 1);

  for (;;)
    {
      if (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
          continue;
        return -errno;
      }
      return 0;
    }
}

int
rpmostree_polkit_agent_open (void)
{
  int r;
  int pipe_fd[2];
  char notify_fd[10 + 1];

  if (agent_pid > 0)
    return 0;

  /* We check STDIN here, not STDOUT, since this is about input,
   * not output */
  if (!isatty (STDIN_FILENO))
    return 0;

  if (pipe (pipe_fd) < 0)
    return -errno;

  snprintf (notify_fd, sizeof (notify_fd), "%i", pipe_fd[1]);
  notify_fd[sizeof (notify_fd) -1] = 0;

  r = fork_agent (&agent_pid,
                  POLKIT_TTY_AGENT_BINARY_PATH,
                  POLKIT_TTY_AGENT_BINARY_PATH,
                  "--notify-fd", notify_fd,
                  "--fallback", NULL);

  /* Close the writing side, because that's the one for the agent */
  close_nointr_nofail (pipe_fd[1]);

  if (r < 0)
    g_warning ("Failed to fork TTY ask password agent: %s", strerror (-r));
  else
    /* Wait until the agent closes the fd */
    fd_wait_for_event (pipe_fd[0], POLLHUP, (uint64_t) -1);

  close_nointr_nofail (pipe_fd[0]);

  return r;
}

void
rpmostree_polkit_agent_close (void)
{

  if (agent_pid <= 0)
    return;

  /* Inform agent that we are done */
  kill (agent_pid, SIGTERM);
  kill (agent_pid, SIGCONT);
  wait_for_terminate (agent_pid);
  agent_pid = 0;
}
