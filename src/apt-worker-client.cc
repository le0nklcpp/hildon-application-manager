/*
 * This file is part of the hildon-application-manager.
 *
 * Copyright (C) 2005, 2006, 2007 Nokia Corporation.  All Rights reserved.
 *
 * Contact: Marius Vollmer <marius.vollmer@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <errno.h>
#include <libintl.h>

#include <gtk/gtk.h>
#include <glib/gspawn.h>

#include "log.h"
#include "util.h"
#include "settings.h"
#include "apt-worker-client.h"
#include "apt-worker-proto.h"

#define _(x) gettext (x)

int apt_worker_out_fd = -1;
int apt_worker_in_fd = -1;
int apt_worker_cancel_fd = -1;
int apt_worker_status_fd = -1;
GPid apt_worker_pid;

gboolean apt_worker_ready = FALSE;

static void cancel_request (int cmd);
static void cancel_all_pending_requests ();

static GString *pmstatus_line;

static void
interpret_pmstatus (char *str)
{
  float percentage;
  char *title;

  if (!strncmp (str, "pmstatus:", 9))
    {
      str += 9;
      str = strchr (str, ':');
      if (str == NULL)
	return;
      str += 1;
      percentage = atof (str);
      str = strchr (str, ':');
      if (str == NULL)
	title = "Working";
      else
	{
	  str += 1;
	  title = str;
	}
	
      set_entertainment_fun (NULL, (int)percentage, 100);
      set_entertainment_cancel (NULL, NULL);
    }
}

static gboolean
read_pmstatus (GIOChannel *channel, GIOCondition cond, gpointer data)
{
  char buf[256], *line_end;
  int n, fd = g_io_channel_unix_get_fd (channel);

  n = read (fd, buf, 256);
  if (n > 0)
    {
      g_string_append_len (pmstatus_line, buf, n);
      while ((line_end = strchr (pmstatus_line->str, '\n')))
	{
	  *line_end = '\0';
	  interpret_pmstatus (pmstatus_line->str);
	  g_string_erase (pmstatus_line, 0, line_end - pmstatus_line->str + 1);
	}
      return TRUE;
    }
  else
    {
      g_io_channel_shutdown (channel, 0, NULL);
      return FALSE;
    }
}

static void
setup_pmstatus_from_fd (int fd)
{
  pmstatus_line = g_string_new ("");
  GIOChannel *channel = g_io_channel_unix_new (fd);
  g_io_add_watch (channel, GIOCondition (G_IO_IN | G_IO_HUP | G_IO_ERR),
		  read_pmstatus, NULL);
  g_io_channel_unref (channel);
}

static bool
must_mkfifo (char *filename, int mode)
{
  if (unlink (filename) < 0 && errno != ENOENT)
    log_perror (filename);
    
  if (mkfifo (filename, mode) < 0)
    {
      log_perror (filename);
      return false;
    }
  return true;
}

static bool
must_unlink (char *filename)
{
  if (unlink (filename) < 0)
    {
      log_perror (filename);
      return false;
    }
  return true;
}

static int
must_open (char *filename, int flags)
{
  int fd = open (filename, flags);
  if (fd < 0)
    {
      log_perror (filename);
      return -1;
    }
  return fd;
}

static int
must_open_nonblock (char *filename, int flags)
{
  int fd = must_open (filename, flags | O_NONBLOCK);
  if (fd >= 0)
    {
      if (fcntl (fd, F_SETFL, flags) < 0)
	perror ("fcntl");
    }
  return fd;
}

static gboolean
handle_apt_worker (GIOChannel *channel, GIOCondition cond, gpointer data)
{
  handle_one_apt_worker_response ();
  return apt_worker_is_running ();
}

static guint apt_source_id;

static void
add_apt_worker_handler ()
{
  GIOChannel *channel = g_io_channel_unix_new (apt_worker_in_fd);
  apt_source_id = g_io_add_watch (channel,
				  GIOCondition (G_IO_IN | G_IO_HUP | G_IO_ERR),
				  handle_apt_worker, NULL);
  g_io_channel_unref (channel);
}

static void
notice_apt_worker_failure ()
{
  //close (apt_worker_in_fd);
  //close (apt_worker_out_fd);
  //close (apt_worker_cancel_fd);

  g_spawn_close_pid (apt_worker_pid);

  apt_worker_in_fd = -1;
  apt_worker_out_fd = -1;
  apt_worker_cancel_fd = -1;

  cancel_all_pending_requests ();

  what_the_fock_p ();
}

static void
apt_worker_watch (GPid pid, int status, gpointer data)
{
  /* Any exit of the apt-worker is a failure.
   */
  add_log ("apt-worker exited.\n");
  notice_apt_worker_failure ();
}

bool
start_apt_worker (gchar *prog)
{
  int stdout_fd, stderr_fd;
  GError *error = NULL;
  gchar *sudo;

  // XXX - be more careful with the /tmp files by putting them in a
  //       temporary directory, maybe.

  if (!must_mkfifo ("/tmp/apt-worker.to", 0600)
      || !must_mkfifo ("/tmp/apt-worker.from", 0600)
      || !must_mkfifo ("/tmp/apt-worker.status", 0600)
      || !must_mkfifo ("/tmp/apt-worker.cancel", 0600))
    return false;

  struct stat info;
  if (stat ("/targets/links/scratchbox.config", &info))
    sudo = "/usr/bin/sudo";
  else
    sudo = "/usr/bin/fakeroot";

  gchar *options = "";

  if (break_locks)
    options = "B";

  gchar *args[] = {
    sudo,
    prog, "backend",
    "/tmp/apt-worker.to", "/tmp/apt-worker.from",
    "/tmp/apt-worker.status", "/tmp/apt-worker.cancel",
    options,
    NULL
  };

  if (!g_spawn_async_with_pipes (NULL,
				 args,
				 NULL,
				 GSpawnFlags (G_SPAWN_DO_NOT_REAP_CHILD),
				 NULL,
				 NULL,
				 &apt_worker_pid,
				 NULL,
				 &stdout_fd,
				 &stderr_fd,
				 &error))
    {
      add_log ("can't spawn %s: %s\n", prog, error->message);
      g_error_free (error);
      return false;
    }

  g_child_watch_add (apt_worker_pid, apt_worker_watch, NULL);

  apt_worker_in_fd = must_open_nonblock ("/tmp/apt-worker.from",
					 O_RDONLY);
  apt_worker_status_fd = must_open_nonblock ("/tmp/apt-worker.status", 
					     O_RDONLY);
  if (apt_worker_in_fd < 0 || apt_worker_status_fd < 0)
    return false;

  log_from_fd (stdout_fd);
  log_from_fd (stderr_fd);
  setup_pmstatus_from_fd (apt_worker_status_fd);
  add_apt_worker_handler ();

  return true;
}

static void send_pending_apt_worker_cmds ();

static void
finish_apt_worker_startup ()
{
  apt_worker_out_fd = must_open ("/tmp/apt-worker.to", O_WRONLY);
  apt_worker_cancel_fd = must_open ("/tmp/apt-worker.cancel", O_WRONLY);

  must_unlink ("/tmp/apt-worker.to");
  must_unlink ("/tmp/apt-worker.from");
  must_unlink ("/tmp/apt-worker.status");
  must_unlink ("/tmp/apt-worker.cancel");

  apt_worker_ready = TRUE;

  send_pending_apt_worker_cmds ();
}

void
cancel_apt_worker ()
{
  if (apt_worker_cancel_fd >= 0)
    {
      unsigned char byte = 0;
      if (write (apt_worker_cancel_fd, &byte, 1) != 1)
	log_perror ("cancel");
    }
}

static bool
must_read (void *buf, size_t n)
{
  int r;

  while (n > 0)
    {
      r = read (apt_worker_in_fd, buf, n);
      if (r < 0)
	{
	  log_perror ("read");
	  return false;
	}
      else if (r == 0)
	{
	  add_log ("apt-worker closed connection.\n");
	  return false;
	}
      n -= r;
      buf = ((char *)buf) + r;
    }
  return true;
}

static bool
must_write (void *buf, int n)
{
  int r;

  while (n > 0)
    {
      r = write (apt_worker_out_fd, buf, n);
      if (r < 0)
	{
	  log_perror ("write");
	  return false;
	}
      else if (r == 0)
	{
	  add_log ("apt-worker exited.\n");
	  return false;
	}
      n -= r;
      buf = ((char *)buf) + r;
    }
  return true;
}

bool
apt_worker_is_running ()
{
  return apt_worker_in_fd > 0;
}

bool
send_apt_worker_request (int cmd, int state, int seq, char *data, int len)
{
  apt_request_header req = { cmd, state, seq, len };
  return must_write (&req, sizeof (req)) &&  must_write (data, len);
}

static int
next_seq ()
{
  static int seq;
  return seq++;
}

struct pending_request {
  int seq;
  int state;

  char *data;   // data != NULL means that this is a delayed command,
  int len;      // see send_pending_apt_worker_cmds.

  apt_worker_callback *done_callback;
  void *done_data;
};

static pending_request pending[APTCMD_MAX];

static void
send_apt_worker_cmd (int cmd)
{
  if (!send_apt_worker_request (cmd,
				pending[cmd].state,
				pending[cmd].seq,
				pending[cmd].data,
				pending[cmd].len))
    {
      what_the_fock_p ();
      cancel_request (cmd);
    }
}

static void
send_pending_apt_worker_cmds ()
{
  for (int cmd = 0; cmd < APTCMD_MAX; cmd++)
    {
      if (pending[cmd].data)
	{
	  send_apt_worker_cmd (cmd);
	  g_free (pending[cmd].data);
	  pending[cmd].data = NULL;
	}
    }
}

void
call_apt_worker (int cmd, int state, char *data, int len,
		 apt_worker_callback *done_callback,
		 void *done_data)
{
  assert (cmd >= 0 && cmd < APTCMD_MAX);

  if (!apt_worker_is_running ())
    {
      add_log ("apt-worker is not running\n");
      done_callback (cmd, NULL, done_data);
    }
  else if (pending[cmd].done_callback)
    {
      add_log ("apt-worker command %d already pending\n", cmd);
      done_callback (cmd, NULL, done_data);
    }
  else
    {
      pending[cmd].seq = next_seq ();
      pending[cmd].state = state;
      pending[cmd].done_callback = done_callback;
      pending[cmd].done_data = done_data;

      if (apt_worker_ready)
	{
	  pending[cmd].data = data;
	  pending[cmd].len = len;
	  send_apt_worker_cmd (cmd);
	  pending[cmd].data = NULL;
	}
      else
	{
	  /* We need to make sure that pending[cmd].data is not NULL,
	     since that is the flag that tells
	     send_pending_apt_worker_cmds that this command is indeed
	     pending.
	  */
	  pending[cmd].data = (char *)g_malloc (len == 0? 1 : len);
	  pending[cmd].len = len;
	  memcpy (pending[cmd].data, data, len);
	}
    }
}

static void
cancel_request (int cmd)
{
  apt_worker_callback *done_callback = pending[cmd].done_callback;
  void *done_data = pending[cmd].done_data;

  pending[cmd].done_callback = NULL;
  if (done_callback)
    done_callback (cmd, NULL, done_data);
}

static void
cancel_all_pending_requests ()
{
  for (int i = 0; i < APTCMD_MAX; i++)
    cancel_request (i);
}

void
handle_one_apt_worker_response ()
{
  static bool running = false;

  static apt_response_header res;
  static char *response_data = NULL;
  static int response_len = 0;
  static apt_proto_decoder dec;

  int cmd;

  assert (!running);
    
  if (!must_read (&res, sizeof (res)))
    {
      notice_apt_worker_failure ();
      return;
    }
      
  //printf ("got response %d/%d/%d\n", res.cmd, res.seq, res.len);
  cmd = res.cmd;

  if (response_len < res.len)
    {
      if (response_data)
	delete[] response_data;
      response_data = new char[res.len];
      response_len = res.len;
    }

  if (!must_read (response_data, res.len))
    {
      notice_apt_worker_failure ();
      return;
    }

  if (cmd < 0 || cmd >= APTCMD_MAX)
    {
      fprintf (stderr, "unrecognized command %d\n", res.cmd);
      return;
    }

  if (!apt_worker_ready)
    finish_apt_worker_startup ();

  dec.reset (response_data, res.len);

  if (cmd == APTCMD_STATUS)
    {
      running = true;
      if (pending[cmd].done_callback)
	pending[cmd].done_callback (cmd, &dec, pending[cmd].done_data);
      running = false;
      return;
    }

  if (pending[cmd].seq != res.seq)
    {
      fprintf (stderr, "ignoring out of sequence reply.\n");
      return;
    }
  
  apt_worker_callback *done_callback = pending[cmd].done_callback;
  pending[cmd].done_callback = NULL;

  running = true;
  assert (done_callback);
  done_callback (cmd, &dec, pending[cmd].done_data);
  running = false;
}

static apt_proto_encoder request;

void
apt_worker_set_status_callback (apt_worker_callback *callback, void *data)
{
  pending[APTCMD_STATUS].done_callback = callback;
  pending[APTCMD_STATUS].done_data = data;
}

void
apt_worker_noop (apt_worker_callback *callback, void *data)
{
  call_apt_worker (APTCMD_NOOP, APTSTATE_DEFAULT, NULL, 0,
		   callback, data);
}

void
apt_worker_get_package_list (int state,
			     bool only_user,
			     bool only_installed,
			     bool only_available,
			     const char *pattern,
			     bool show_magic_sys,
			     apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_int (only_user);
  request.encode_int (only_installed);
  request.encode_int (only_available);
  request.encode_string (pattern);
  request.encode_int (show_magic_sys);
  call_apt_worker (APTCMD_GET_PACKAGE_LIST, state,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_update_cache (int state, apt_worker_callback *callback, void *data)
{
  request.reset ();
  
  char *http_proxy = get_http_proxy ();
  request.encode_string (http_proxy);
  g_free (http_proxy);
  
  char *https_proxy = get_https_proxy ();
  request.encode_string (https_proxy);
  g_free (https_proxy);
  
  call_apt_worker (APTCMD_CHECK_UPDATES, state, 
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_get_catalogues (apt_worker_callback *callback, void *data)
{
  call_apt_worker (APTCMD_GET_CATALOGUES, APTSTATE_DEFAULT, NULL, 0,
		   callback, data);
}

void
apt_worker_set_catalogues (int state, 
			   xexp *catalogues,
			   apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_xexp (catalogues);
  call_apt_worker (APTCMD_SET_CATALOGUES, state,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_get_package_info (int state,
			     const char *package,
			     bool only_installable_info,
			     apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_string (package);
  request.encode_int (only_installable_info);
  call_apt_worker (APTCMD_GET_PACKAGE_INFO, state,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_get_package_details (const char *package,
				const char *version,
				int summary_kind,
				int state,
				apt_worker_callback *callback,
				void *data)
{
  request.reset ();
  request.encode_string (package);
  request.encode_string (version);
  request.encode_int (summary_kind);
  call_apt_worker (APTCMD_GET_PACKAGE_DETAILS, state,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_install_check (int state, const char *package,
			  apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_string (package);
  call_apt_worker (APTCMD_INSTALL_CHECK, state,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_install_package (int state, const char *package,
			    const char *alt_download_root,
			    bool check_free_space, bool updating,
			    apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_string (package);

  request.encode_string (alt_download_root);

  char *http_proxy = get_http_proxy ();
  request.encode_string (http_proxy);
  g_free (http_proxy);
  
  char *https_proxy = get_https_proxy ();
  request.encode_string (https_proxy);
  g_free (https_proxy);
  
  request.encode_int (check_free_space);

  call_apt_worker (APTCMD_INSTALL_PACKAGE, state,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_remove_check (const char *package,
			 apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_string (package);
  call_apt_worker (APTCMD_REMOVE_CHECK, APTSTATE_DEFAULT,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_remove_package (const char *package,
			   apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_string (package);
  call_apt_worker (APTCMD_REMOVE_PACKAGE, APTSTATE_DEFAULT,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_clean (int state, apt_worker_callback *callback, void *data)
{
  call_apt_worker (APTCMD_CLEAN, state, NULL, 0,
		   callback, data);
}

void
apt_worker_install_file (const char *file,
			 apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_string (file);
  call_apt_worker (APTCMD_INSTALL_FILE, APTSTATE_DEFAULT, 
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_get_file_details (bool only_user, const char *file,
			     apt_worker_callback *callback, void *data)
{
  request.reset ();
  request.encode_int (only_user);
  request.encode_string (file);
  call_apt_worker (APTCMD_GET_FILE_DETAILS, APTSTATE_DEFAULT, 
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_save_backup_data (apt_worker_callback *callback,
			     void *data)
{
  request.reset ();
  call_apt_worker (APTCMD_SAVE_BACKUP_DATA, APTSTATE_DEFAULT,
		   request.get_buf (), request.get_len (),
		   callback, data);
}

void
apt_worker_get_system_update_packages (int state,
				       apt_worker_callback *callback,
				       void *data)
{
  request.reset ();
  call_apt_worker (APTCMD_GET_SYSTEM_UPDATE_PACKAGES, state,
		   request.get_buf (), request.get_len (),
		   callback, data);
}
