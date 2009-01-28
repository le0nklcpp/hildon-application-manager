/*
 * This file is part of the hildon-application-manager.
 *
 * Copyright (C) 2007, 2008 Nokia Corporation.  All Rights reserved.
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

#define _GNU_SOURCE

#include "ham-updates-status-menu-item.h"

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <libintl.h>
#include <sys/inotify.h>
#include <sys/stat.h>

#include <gtk/gtk.h>
#include <gconf/gconf-client.h>

#include <hildon/hildon.h>
#include <libosso.h>
#include <clockd/libtime.h>
#include <libalarm.h>
#include <conic.h>

#include <xexp.h>
#include <user_files.h>

#define DEBUG

#include "util.h"
#include "update-notifier-conf.h"

/* appname for OSSO and alarmd */
#define APPNAME                  "hildon_update_notifier"

/* inotify paths */
#define  INOTIFY_DIR             "/var/lib/hildon-application-manager"

#define STATUSBAR_HAM_ICON_SIZE  16
#define STATUSMENU_HAM_ICON_SIZE 64

#define _(x) dgettext ("hildon-application-manager", (x))

#define HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), HAM_UPDATES_STATUS_MENU_ITEM_TYPE, HamUpdatesStatusMenuItemPrivate))

typedef enum _State State;
enum _State
  {
    UPNO_STATE_INVISIBLE,
    UPNO_STATE_STATIC,
    UPNO_STATE_BLINKING
  };

typedef enum _ConState ConState;
enum _ConState
  {
    UPNO_CON_ONLINE,
    UPNO_CON_OFFLINE
  };

/* inotify watchers id */
enum
  {
    HOME, VAR, MAXWATCH
  };

typedef struct _HamUpdatesStatusMenuItemPrivate HamUpdatesStatusMenuItemPrivate;
struct _HamUpdatesStatusMenuItemPrivate
{
  /* ui */
  GdkPixbuf *icon;
  GtkWidget *button;

  /* environment */
  osso_context_t *osso;
  GConfClient *gconf;

  /* state */
  State state;
  cookie_t alarm_cookie;

  /* inotify */
  gint inotify_fd;
  guint io_watch;
  gint wd[2];

  /* libconic */
  ConIcConnection *conic;
  ConState constate;

  /* apt-worker spawn */
  guint child_id;
};

/* setup prototypes */
static gboolean setup_dbus (HamUpdatesStatusMenuItem *self);
static void setup_gconf (HamUpdatesStatusMenuItem *self);
static gboolean setup_alarm (HamUpdatesStatusMenuItem *upno);
static gboolean setup_alarm_now (gpointer data);
static gboolean setup_inotify (HamUpdatesStatusMenuItem *self);
static void close_inotify (HamUpdatesStatusMenuItem *self);
static void setup_ui (HamUpdatesStatusMenuItem *self);

/* teardown prototypes */
static void delete_all_alarms (void);

/* state handling prototypes */
static void load_state (HamUpdatesStatusMenuItem *self);
static void save_state (HamUpdatesStatusMenuItem *self);
static void update_state (HamUpdatesStatusMenuItem *self);
static void set_state (HamUpdatesStatusMenuItem *self, State state);
static State get_state (HamUpdatesStatusMenuItem *self);

/* connection state prototypes */
static void setup_connection_state (HamUpdatesStatusMenuItem *self);

/* ham querying */
static gboolean ham_is_showing_check_for_updates_view
(HamUpdatesStatusMenuItem *self);

/* Dialog content. Must free the return value */
static gchar* get_dialog_content ();

HD_DEFINE_PLUGIN_MODULE (HamUpdatesStatusMenuItem, ham_updates_status_menu_item,
                         HD_TYPE_STATUS_MENU_ITEM);

static void
ham_updates_status_menu_item_class_finalize
(HamUpdatesStatusMenuItemClass *klass)
{
  /* noop */
}

static void
ham_updates_status_menu_item_finalize (GObject *object)
{
  HamUpdatesStatusMenuItem *self;
  HamUpdatesStatusMenuItemPrivate *priv;

  self = HAM_UPDATES_STATUS_MENU_ITEM (object);
  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  if (priv->icon != NULL)
    g_object_unref (priv->icon);

  if (priv->child_id > 0)
    g_source_remove (priv->child_id);

  delete_all_alarms ();

  close_inotify (self);

  if (priv->conic != NULL)
    g_object_unref (priv->conic);

  if (priv->gconf != NULL)
    g_object_unref (priv->gconf);

  if (priv->osso != NULL)
    osso_deinitialize (priv->osso);

  G_OBJECT_CLASS (ham_updates_status_menu_item_parent_class)->finalize (object);
}

static void
ham_updates_status_menu_item_class_init (HamUpdatesStatusMenuItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = ham_updates_status_menu_item_finalize;

  g_type_class_add_private (object_class,
                            sizeof (HamUpdatesStatusMenuItemPrivate));
}

static void
ham_updates_status_menu_item_init (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  priv->osso = NULL;

  priv->state = UPNO_STATE_INVISIBLE;
  priv->alarm_cookie = 0;

  priv->inotify_fd = -1;
  priv->io_watch = 0;
  priv->wd[HOME] = priv->wd[VAR] = -1;

  priv->conic = NULL;
  priv->constate = UPNO_CON_OFFLINE;

  if (setup_dbus (self))
    {
      LOG ("dbus setup");

      setup_gconf (self);
      setup_ui (self);
      setup_connection_state (self);

      if (setup_inotify (self))
        {
          /* We only setup the alarm after a one minute pause since the alarm
             daemon is not yet running when the plugins are loaded after boot.
             It is arguably a bug in the alarm framework that the daemon needs
             to be running to access and modify the alarm queue.
          */
          g_timeout_add_seconds (60, setup_alarm_now, self);
        }
      else
        {
          close_inotify (self);
        }
    }
}

static gchar*
get_http_proxy (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;
  gchar *proxy;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  if ((proxy = getenv ("http_proxy")) != NULL)
    return g_strdup (proxy);

  proxy = NULL;

  if (priv->conic != NULL)
    {
      const gchar* host;
      gint port;

      host = con_ic_connection_get_proxy_host (priv->conic,
                                               CON_IC_PROXY_PROTOCOL_HTTP);
      port = con_ic_connection_get_proxy_port (priv->conic,
                                               CON_IC_PROXY_PROTOCOL_HTTP);

      if (host != NULL)
        proxy = g_strdup_printf ("http://%s:%d", host, port);
    }
  else if (priv->gconf != NULL)
    {
      proxy = get_gconf_http_proxy ();
    }

  return proxy;
}

static void
check_for_updates_done (GPid pid, gint status, gpointer data)
{
  HamUpdatesStatusMenuItem *self;
  HamUpdatesStatusMenuItemPrivate *priv;

  g_return_if_fail (IS_HAM_UPDATES_STATUS_MENU_ITEM (data));

  self = HAM_UPDATES_STATUS_MENU_ITEM (data);
  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  priv->child_id = 0;

  if (status != -1 && WIFEXITED (status) && WEXITSTATUS (status) == 0)
    {
      LOG ("Check for updates done");
      save_last_update_time (time_get_time ());
      update_state (self);
    }
  else
    {
      /* Ask the Application Manager to perform the update, but don't
	 start it if it isn't running already.
       */
      if (ham_is_running ())
        {
          LOG ("Calling HAM RPC async");
          osso_rpc_async_run (priv->osso,
                              HILDON_APP_MGR_SERVICE,
                              HILDON_APP_MGR_OBJECT_PATH,
                              HILDON_APP_MGR_INTERFACE,
                              HILDON_APP_MGR_OP_CHECK_UPDATES,
                              NULL,
                              NULL,
                              DBUS_TYPE_INVALID);
        }
    }
}

static void
check_for_updates (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;
  gchar *gainroot_cmd;
  gchar *proxy;
  GPid pid;
  GError *error;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  if (priv->constate == UPNO_CON_OFFLINE)
    return;

  error = NULL;

  LOG ("calling apt-worker checking for updates");

  /* Choose the right gainroot command */
  gainroot_cmd = NULL;
  if (running_in_scratchbox ())
    gainroot_cmd = g_strdup ("/usr/bin/fakeroot");
  else
    gainroot_cmd = g_strdup ("/usr/bin/sudo");

  proxy = get_http_proxy (self);
  LOG ("Proxy = %s", proxy);

  /* Build command to be spawned */
  char *argv[] = {
    gainroot_cmd,
    "/usr/libexec/apt-worker",
    "check-for-updates",
    proxy,
    NULL
  };

  if (!g_spawn_async_with_pipes (NULL,
				 argv,
				 NULL,
				 G_SPAWN_DO_NOT_REAP_CHILD,
				 NULL,
				 NULL,
				 &pid,
				 NULL,
				 NULL,
				 NULL,
				 &error))
    {
      fprintf (stderr, "can't run %s: %s\n", argv[0], error->message);
      g_error_free (error);
    }
  else
    {
      priv->child_id = g_child_watch_add (pid, check_for_updates_done, self);
    }

  g_free (gainroot_cmd);
  g_free (proxy);
}

static gint
ham_updates_status_menu_item_rpc_cb (const gchar* interface,
                                     const gchar* method,
                                     GArray* arguments,
                                     gpointer data,
                                     osso_rpc_t* retval)
{
  HamUpdatesStatusMenuItem *self;

  g_return_val_if_fail (IS_HAM_UPDATES_STATUS_MENU_ITEM (data), OSSO_ERROR);
  g_return_val_if_fail (interface != NULL && method != NULL, OSSO_ERROR);

  LOG ("RPC Message: %s:%s", interface, method);

  self = HAM_UPDATES_STATUS_MENU_ITEM (data);

  if (strcmp (interface, UPDATE_NOTIFIER_INTERFACE) != 0)
    return OSSO_ERROR;

  if (strcmp (method, UPDATE_NOTIFIER_OP_CHECK_UPDATES) == 0)
    {
      /* Search for new avalable updates */
      check_for_updates (self);
      /* check_for_notifications (self); */
    }
  else if (strcmp (method, UPDATE_NOTIFIER_OP_CHECK_STATE) == 0)
    {
      /* Update states of the satusbar item */
      update_state (self);
    }
  else
    return OSSO_ERROR;

  return OSSO_OK;
}

static gboolean
setup_dbus (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;
  osso_return_t result;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  g_return_val_if_fail (priv->osso == NULL, FALSE);

  priv->osso = osso_initialize (APPNAME, PACKAGE_VERSION, TRUE, NULL);

  if (priv->osso == NULL)
    return FALSE;

  result = osso_rpc_set_cb_f (priv->osso,
                              UPDATE_NOTIFIER_SERVICE,
                              UPDATE_NOTIFIER_OBJECT_PATH,
                              UPDATE_NOTIFIER_INTERFACE,
                              ham_updates_status_menu_item_rpc_cb, self);

  return (result == OSSO_OK);
}

static void
ham_updates_status_menu_item_interval_changed_cb (GConfClient *client,
                                                  guint cnxn_id,
                                                  GConfEntry *entry,
                                                  gpointer data)
{
  g_return_if_fail (IS_HAM_UPDATES_STATUS_MENU_ITEM (data));

  LOG ("Interval value changed");
  delete_all_alarms ();
  setup_alarm (HAM_UPDATES_STATUS_MENU_ITEM (data));
}

static void
setup_gconf (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  g_return_if_fail (priv->gconf == NULL);

  priv->gconf = gconf_client_get_default ();

  gconf_client_add_dir (priv->gconf,
                        UPNO_GCONF_DIR, GCONF_CLIENT_PRELOAD_ONELEVEL, NULL);

  gconf_client_notify_add (priv->gconf, UPNO_GCONF_CHECK_INTERVAL,
                           ham_updates_status_menu_item_interval_changed_cb,
                           self, NULL, NULL);
}

static gboolean
xexp_is_tag_and_not_empty (xexp *x, const char *tag)
{
  return (x != NULL && xexp_is (x, tag) && !xexp_is_empty (x));
}

static gboolean
compare_xexp_text (xexp *x_a, xexp *x_b, const char *tag)
{
  const char *text_a = NULL;
  const char *text_b = NULL;

  if ((x_a == NULL) || (x_b == NULL))
    return ((x_a == NULL) && (x_b == NULL));

  text_a = xexp_aref_text(x_a, tag);
  text_b = xexp_aref_text(x_b, tag);

  if ((text_a == NULL) || (text_b == NULL))
    return ((text_a == NULL) && (text_b == NULL));
  else
    return (strcmp (text_a, text_b) == 0);
}

static void
update_seen_notifications ()
{
  xexp *avail_notifications;
  xexp *seen_notifications;

 avail_notifications = user_file_read_xexp (UFILE_AVAILABLE_NOTIFICATIONS);
 seen_notifications = user_file_read_xexp (UFILE_SEEN_NOTIFICATIONS);

 if (avail_notifications && seen_notifications &&
     xexp_is_tag_and_not_empty (avail_notifications, "info") &&
     (!xexp_is_tag_and_not_empty (seen_notifications, "info") ||
      (xexp_is_tag_and_not_empty (seen_notifications, "info") &&
       !compare_xexp_text (avail_notifications, seen_notifications, "title") &&
       !compare_xexp_text (avail_notifications, seen_notifications, "text") &&
       !compare_xexp_text (avail_notifications, seen_notifications, "uri"))))
   {
     /* as we have new notifications, we no longer need the old seen ones;
      * the writing of UFILE_SEEN_NOTIFICATIONS will trigger an inotify */
     xexp* empty_seen_notifications;

     empty_seen_notifications = xexp_list_new ("info");
     user_file_write_xexp (UFILE_SEEN_NOTIFICATIONS, empty_seen_notifications);
     xexp_free (empty_seen_notifications);
   }

 if (avail_notifications)
   xexp_free (avail_notifications);

 if (seen_notifications)
   xexp_free (seen_notifications);
}

#define BUF_LEN 4096

static gboolean
ham_updates_status_menu_item_inotify_cb (GIOChannel *source,
                                         GIOCondition condition,
                                         gpointer data)
{
  HamUpdatesStatusMenuItemPrivate *priv;
  gchar buf[BUF_LEN];
  gint i;
  gint len;

  /* Return if the object was already destroyed
     or the if inotify is not still ready */
  g_return_val_if_fail (IS_HAM_UPDATES_STATUS_MENU_ITEM (data), FALSE);

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (data);
  g_return_val_if_fail (priv->inotify_fd != -1, FALSE);

  LOG ("inotify callback");

  while (TRUE)
    {
      while ((len = read (priv->inotify_fd, buf, BUF_LEN)) < 0
             && errno == EINTR);
      if (len > 0)
        break;
      else if (len < 0)
        {
          g_warning ("Error reading inotify event: %s", g_strerror (errno));
          goto error_cancel;
        }

      g_assert (len == 0);
      g_warning ("Buffer size %u too small", BUF_LEN);
      goto error_cancel;
    }

  i = 0;
  while (i < len)
    {
      struct inotify_event *event;

      event = (struct inotify_event *) &buf[i];

      if (is_file_modified (event, priv->wd[VAR], AVAILABLE_UPDATES_FILE_NAME)
          || is_file_modified (event, priv->wd[HOME], UFILE_SEEN_UPDATES)
          || is_file_modified (event, priv->wd[HOME], UFILE_SEEN_NOTIFICATIONS))
        {
          update_state (HAM_UPDATES_STATUS_MENU_ITEM (data));
        }
      else if (is_file_modified (event, priv->wd[HOME],
                                 UFILE_AVAILABLE_NOTIFICATIONS))
        {
          update_seen_notifications ();
          update_state (HAM_UPDATES_STATUS_MENU_ITEM (data));
        }

      i += sizeof (struct inotify_event) + event->len;
    }

  return TRUE;

error_cancel:
  priv->io_watch = 0;
  close_inotify (HAM_UPDATES_STATUS_MENU_ITEM (data));
  return FALSE;
}

static gint
add_watch_for_path (HamUpdatesStatusMenuItem *self, const gchar *path)
{
  HamUpdatesStatusMenuItemPrivate *priv;
  gint watch;

  g_return_val_if_fail (path != NULL, -1);

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  watch = inotify_add_watch (priv->inotify_fd, path,
                             IN_CLOSE_WRITE | IN_MOVED_TO);

  if (watch < 0)
    {
      g_warning ("Failed to create watch for local file %s : %s\n",
                 path, strerror (errno));
      return -1;
    }

  return watch;
}

static gboolean
setup_inotify (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;
  gint fd;
  GIOChannel *io_channel;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  g_return_val_if_fail (priv->inotify_fd == -1, FALSE);

  if ((fd = inotify_init ()) < 0)
    {
      g_warning ("Failed to initialize inotify: %s", g_strerror (errno));
      return FALSE;
    }

  priv->inotify_fd = fd;

  io_channel = g_io_channel_unix_new (fd);
  priv->io_watch = g_io_add_watch (io_channel, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                   ham_updates_status_menu_item_inotify_cb,
                                   self);
  g_io_channel_unref (io_channel);

  {
    gchar *path;

    path = user_file_get_state_dir_path ();
    priv->wd[HOME] = add_watch_for_path (self, path);
    g_free (path);

    priv->wd[VAR] = add_watch_for_path (self, INOTIFY_DIR);

    if (priv->wd[HOME] == -1 || priv->wd[VAR] == -1)
      return FALSE;
  }

  return TRUE;
}

static void
close_inotify (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  if (priv->io_watch > 0)
    g_source_remove (priv->io_watch);
  priv->io_watch = 0;

  if (priv->inotify_fd > 0)
    {
      gint i;

      for (i = 0; i < MAXWATCH; i++)
        {
          if (priv->wd[i] != -1)
            {
              inotify_rm_watch (priv->inotify_fd, priv->wd[i]);
              priv->wd[i] = -1;
            }
        }
      close (priv->inotify_fd);
    }
  priv->inotify_fd = -1;
}

static void
delete_all_alarms (void)
{
  int i;
  cookie_t *cookies;

  if ((cookies = alarmd_event_query (0, 0, 0, 0, APPNAME)))
    {
      for (i = 0; cookies[i]; ++i)
        {
          LOG ("deleting event %d", cookies[i]);
          alarmd_event_del (cookies[i]);
        }
      free(cookies);
    }
}

static cookie_t
get_last_alarm (void)
{
  cookie_t *cookies;
  cookie_t retval;

  retval = 0;
  if ((cookies = alarmd_event_query (0, 0, 0, 0, APPNAME)))
    {
      if (cookies[1] != 0)
        {
          LOG ("Several alarm events found! Killing them all!");
          delete_all_alarms ();
        }
      else
        {
          retval = cookies[0];
        }
      free (cookies);
    }

  return retval;
 }

static time_t
get_interval (HamUpdatesStatusMenuItem* self)
{
  HamUpdatesStatusMenuItemPrivate *priv;
  time_t interval;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  interval = (time_t) gconf_client_get_int (priv->gconf,
                                            UPNO_GCONF_CHECK_INTERVAL,
                                            NULL);

  if (interval <= 0)
    {
      /* Use default value and set it from now on */
      interval = UPNO_DEFAULT_CHECK_INTERVAL;
      gconf_client_set_int (priv->gconf,
                            UPNO_GCONF_CHECK_INTERVAL,
                            (gint) interval,
                            NULL);
    }

  LOG ("The interval is %d", interval);
  return interval;
}

static gboolean
setup_alarm (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;
  alarm_event_t *event;
  alarm_action_t *action;
  time_t interval;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  g_return_val_if_fail (priv->alarm_cookie == 0, FALSE);

  priv->alarm_cookie = get_last_alarm ();
  LOG ("The stored alarm id is %d", priv->alarm_cookie);

  /* We reset the alarm when we don't have a cookie for the old alarm
     (which probably means there is no old alarm), when we can't find
     the old alarm although we have a cookie (which shouldn't happen
     unless someone manually mucks with the alarm queue), or if the
     interval has changed.

     Otherwise we leave the old alarm in place, but we update its
     parameters without touching the timing.
  */
  if (priv->alarm_cookie > 0)
    {
      alarm_event_t *old_event;

      old_event = alarmd_event_get (priv->alarm_cookie);

      if (old_event != NULL)
        {
          if (alarm_event_is_recurring (old_event))
            {
              LOG ("There's already a recurring event. No new event needed");
              return TRUE;
            }
         else
            {
              LOG ("A not recurring event found! Killing them all!");
              delete_all_alarms ();
            }
        }
    }

  LOG ("Creating a new event");

  /* Setup new alarm based on old alarm. */
  event = alarm_event_create ();
  alarm_event_set_alarm_appid (event, APPNAME);

  /* If the trigger time is missed (due to the device being off or
     system time being adjusted beyond the trigger point) the alarm
     should be run anyway. */
  event->flags |= ALARM_EVENT_RUN_DELAYED;

  /* Run only when internet connection is available. */
  /* conic is needed */
  /* FIXME: event->flags |= ALARM_EVENT_CONNECTED; */

  /* If the system time is moved backwards, the alarm should be
     rescheduled. */
  event->flags |= ALARM_EVENT_BACK_RESCHEDULE;

  interval = get_interval (self);
  event->alarm_time = ALARM_RECURRING_SECONDS (time_get_time () + interval);

  /* set the recurrence */
  event->recur_count = -1; /* infinite recorrence */
  event->recur_secs = ALARM_RECURRING_SECONDS (interval);

  /* create the action */
  action = alarm_event_add_actions (event, 1);

  action->flags |= ALARM_ACTION_WHEN_TRIGGERED;
  action->flags |= ALARM_ACTION_TYPE_DBUS;
  action->flags |= ALARM_ACTION_DBUS_USE_ACTIVATION;

  alarm_action_set_dbus_service (action, UPDATE_NOTIFIER_SERVICE);
  alarm_action_set_dbus_path (action, UPDATE_NOTIFIER_OBJECT_PATH);
  alarm_action_set_dbus_interface (action, UPDATE_NOTIFIER_INTERFACE);
  alarm_action_set_dbus_name (action, UPDATE_NOTIFIER_OP_CHECK_UPDATES);

  if (alarm_event_is_sane (event) != -1)
    {
      priv->alarm_cookie = alarmd_event_add (event);
      alarm_event_delete (event);
    }
  else
    LOG ("alarm event is not correct!");

  LOG ("The new alarm id is %d", priv->alarm_cookie);

  return priv->alarm_cookie > 0;
}

static void
run_service_now (HamUpdatesStatusMenuItem *self)
{
  time_t now;
  time_t last_update;
  time_t interval;

  now = time_get_time ();
  last_update = load_last_update_time ();
  interval = get_interval (self);

  LOG ("now = %d, last update = %d, interval = %d", now, last_update, interval);
  if (now - last_update > interval)
    {
      LOG ("we haven't checked for updates since long time ago");
      /* Search for new avalable updates */
      check_for_updates (self);
      /* check_for_notifications (self); */
    }
}

static gboolean
setup_alarm_now (gpointer data)
{
  HamUpdatesStatusMenuItem *self;

  g_return_val_if_fail (IS_HAM_UPDATES_STATUS_MENU_ITEM (data), FALSE);

  self = HAM_UPDATES_STATUS_MENU_ITEM (data);

  run_service_now (self);

  if (setup_alarm (self))
    return FALSE;

  /* Try again in one minute. */
  return TRUE;
}

static void
ham_updates_status_menu_item_button_click_cb (GtkButton *button, gpointer data)
{
  GtkWidget *parent;
  GtkWidget *dialog;
  HamUpdatesStatusMenuItem *self;
  gint response;

  g_return_if_fail (IS_HAM_UPDATES_STATUS_MENU_ITEM (data));

  self = HAM_UPDATES_STATUS_MENU_ITEM (data);

  parent = gtk_widget_get_toplevel (GTK_WIDGET (self));

  dialog = gtk_dialog_new_with_buttons
    (_("ai_sb_update_description"), GTK_WINDOW (parent),
     GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
     _("ai_sb_update_am"), GTK_RESPONSE_YES,
     _("ai_sb_app_push_no"), GTK_RESPONSE_NO,
     NULL);

  /* contents */
  {
    GtkWidget *label;
    gchar *content;

    content = get_dialog_content ();

    if (content == NULL)
      goto error;

    label = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (label), content);
    g_free (content);

    gtk_container_add (GTK_CONTAINER (GTK_DIALOG (dialog)->vbox), label);
  }

  gtk_widget_show_all (dialog);

  response = gtk_dialog_run (GTK_DIALOG (dialog));

  if (response == GTK_RESPONSE_YES)
  {
    HamUpdatesStatusMenuItemPrivate *priv;
    LOG ("Starts Application Manager and opens 'Check for update'");

    priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

    osso_rpc_async_run (priv->osso,
                        HILDON_APP_MGR_SERVICE,
                        HILDON_APP_MGR_OBJECT_PATH,
                        HILDON_APP_MGR_INTERFACE,
                        HILDON_APP_MGR_OP_SHOW_CHECK_FOR_UPDATES,
                        NULL,
                        NULL,
                        DBUS_TYPE_INVALID);
  }

 error:
  gtk_widget_destroy (dialog);
  set_state (self, UPNO_STATE_INVISIBLE);
}

static GdkPixbuf *
icon_load (const gchar *name, gint size)
{
  GtkIconTheme *icon_theme;
  GdkPixbuf *pixbuf;
  GError *error;

  if (name == NULL)
    return NULL;

  pixbuf = NULL;
  error = NULL;

  icon_theme = gtk_icon_theme_get_default ();

  if (size < 1)
  {
    gint idx;
    /* size was smaller than one => use the largest natural size available */
    gint *icon_sizes = gtk_icon_theme_get_icon_sizes (icon_theme, name);
    for (idx = 0; icon_sizes[idx] != 0 && icon_sizes[idx] != -1; idx++)
      size = icon_sizes[idx];
    g_free (icon_sizes);
  }

  pixbuf =  gtk_icon_theme_load_icon (icon_theme, name, size,
                                      GTK_ICON_LOOKUP_NO_SVG, &error);

  if (error != NULL)
  {
    fprintf (stderr, "error loading pixbuf '%s': %s", name, error->message);
    g_error_free (error);
  }

  return pixbuf;
}

static void
build_button (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;
  gchar *title;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  title = _("ai_sb_update_description");

  priv->button = hildon_button_new_with_text
    (HILDON_SIZE_FULLSCREEN_WIDTH | HILDON_SIZE_FINGER_HEIGHT,
     HILDON_BUTTON_ARRANGEMENT_VERTICAL, title, "");

  /* set icon */
  {
    GdkPixbuf *pixbuf;

    pixbuf = icon_load ("general_application_manager",
                        STATUSMENU_HAM_ICON_SIZE);

    if (pixbuf != NULL)
      {
        GtkWidget *image;

        image = gtk_image_new_from_pixbuf (pixbuf);

        if (image != NULL)
          hildon_button_set_image (HILDON_BUTTON (priv->button), image);

        g_object_unref (pixbuf);
      }
  }

  gtk_widget_show (GTK_WIDGET (priv->button));
  g_signal_connect (G_OBJECT (priv->button), "clicked",
                    G_CALLBACK (ham_updates_status_menu_item_button_click_cb),
                    self);

  gtk_container_add (GTK_CONTAINER (self), priv->button);
}

static void
build_status_area_icon (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  priv->icon = icon_load ("qgn_stat_new_updates", 0);

/*   hd_status_plugin_item_set_status_area_icon (HD_STATUS_PLUGIN_ITEM (self), */
/*                                               priv->icon); */
}

static void
setup_ui (HamUpdatesStatusMenuItem *self)
{
  build_button (self);
  build_status_area_icon (self);
  load_state (self);
  update_state (self);
}

static void
load_state (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;
  xexp *state = NULL;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  state = user_file_read_xexp (UFILE_UPDATE_NOTIFIER);

  if (state != NULL)
    {
      set_state (self,
                 xexp_aref_int (state, "icon-state", UPNO_STATE_INVISIBLE));
      /* priv->alarm_cookie =
         (cookie_t) xexp_aref_int (state, "alarm-cookie", 0); */
      xexp_free (state);
    }

  LOG ("state loaded");
}

static void
save_state (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;
  xexp *x_state = NULL;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  x_state = xexp_list_new ("state");

  xexp_aset_int (x_state, "icon-state", (gint) priv->state);
  /* xexp_aset_int (x_state, "alarm-cookie", priv->alarm_cookie); */

  user_file_write_xexp (UFILE_UPDATE_NOTIFIER, x_state);

  xexp_free (x_state);

  LOG ("state saved");
}

static void
update_widget_state (HamUpdatesStatusMenuItem *self)
{
  State state;
  gboolean visible;

  state = get_state (self);
  g_object_get (G_OBJECT (self), "visible", &visible, NULL);

  if (state == UPNO_STATE_INVISIBLE && visible)
    {
      LOG ("turning off the widgets");
      hd_status_plugin_item_set_status_area_icon (HD_STATUS_PLUGIN_ITEM (self),
                                                  NULL);
      gtk_widget_hide (GTK_WIDGET (self));
      return;
    }
  else if (!visible) /* this is common to blinking and static */
    {
      HamUpdatesStatusMenuItemPrivate *priv;

      LOG ("turning on the widgets");
      priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

      gtk_widget_show (GTK_WIDGET (self));
      hd_status_plugin_item_set_status_area_icon (HD_STATUS_PLUGIN_ITEM (self),
                                                  priv->icon);
    }
  else
    {
      g_assert_not_reached ();
    }

  switch ((gint) state)
    {
    case UPNO_STATE_STATIC:
      /* @todo */
      break;
    case UPNO_STATE_BLINKING:
      /* @todo */
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
set_state (HamUpdatesStatusMenuItem* self, State state)
{
  HamUpdatesStatusMenuItemPrivate *priv;

  g_return_if_fail (state >= UPNO_STATE_INVISIBLE &&
                    state <= UPNO_STATE_BLINKING);

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  /* let's avoid the obvious */
  if (state == priv->state)
    return;

  /* this rule seems to be applied ever: */
  /* we can only go to blinking if we're invisible */
  g_return_if_fail (!(state == UPNO_STATE_BLINKING
                      && priv->state != UPNO_STATE_INVISIBLE));

  {
    LOG ("Changing icon state from %d to %d", priv->state, state);
    priv->state = state;
    save_state (self);
    update_widget_state (self);
  }
}

static State
get_state (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  return priv->state;
}

/* contains and transports the number of available updates types */
typedef struct {
  gint os;
  gint certified;
  gint other;
  gint new;
} UpdatesCount;

/* calculates the number of available updates types */
static UpdatesCount *
get_updates_count ()
{
  xexp *available_updates;
  xexp *seen_updates;
  UpdatesCount *retval;

  retval = g_new0 (UpdatesCount, 1);

  available_updates = xexp_read_file (AVAILABLE_UPDATES_FILE);

  if (available_updates == NULL)
    goto exit;

  seen_updates = user_file_read_xexp (UFILE_SEEN_UPDATES);

  if (seen_updates == NULL)
    seen_updates = xexp_list_new ("updates");

  /* preconditions ok */
  {
    xexp *x;
    xexp *y;

    y = NULL;

    for (x = xexp_first (available_updates); x != NULL; x = xexp_rest (x))
      {
        if (!xexp_is_text (x))
          continue;

        if ((seen_updates != NULL) && xexp_is_list (seen_updates))
          {
            const gchar *pkg;

            pkg = xexp_text (x);

            for (y = xexp_first (seen_updates); y != NULL; y = xexp_rest (y))
              if (xexp_is_text (y) && strcmp (pkg, xexp_text (y)) == 0)
                break;
          }

        if (y == NULL)
          retval->new++;

        if (xexp_is (x, "os"))
          retval->os++;
        else if (xexp_is (x, "certified"))
          retval->certified++;
        else
          retval->other++;
      }

      xexp_free (available_updates);

      if (seen_updates != NULL)
        xexp_free (seen_updates);
    }

 exit:
  LOG ("new pkgs = %d, new os = %d, new cert = %d, other = %d",
       retval->new, retval->os, retval->certified, retval->other);

  return retval;
}

static gchar*
maybe_add_dots (gchar *str)
{
  gchar *tmp;

  tmp = NULL;
  if (str)
    {
      tmp = g_strconcat (str, "...", NULL);
      g_free (str);
    }

  return tmp;
}

static gchar*
build_status_menu_button_value (UpdatesCount *uc)
{
  gchar *retval;

  g_return_val_if_fail (uc != NULL && uc->new != 0, NULL);

  retval = NULL;

  if (uc->os > 0)
    retval = g_strdup_printf (_("ai_sb_update_os_%d"), uc->os);

  if (uc->certified > 0)
    {
      retval = maybe_add_dots (retval);
      if (!retval)
        retval = g_strdup_printf (_("ai_sb_update_nokia_%d"), uc->certified);
    }

  if (uc->other > 0)
    {
      retval = maybe_add_dots (retval);
      if (!retval)
        retval = g_strdup_printf (_("ai_sb_update_thirdparty_%d"), uc->other);
    }

  LOG ("update string = %s", retval);
  return retval;
}

static gboolean
update_status_menu_button_value (HamUpdatesStatusMenuItem *self)
{
  UpdatesCount *uc;
  gboolean retval;

  retval = FALSE;

  if ((uc = get_updates_count ()) == NULL)
    return FALSE;

  if (uc->new > 0 && !ham_is_showing_check_for_updates_view (self))
    {
      gchar *value;

      if ((value = build_status_menu_button_value (uc)) != NULL)
        {
          HamUpdatesStatusMenuItemPrivate *priv;
          priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);
          hildon_button_set_value (HILDON_BUTTON (priv->button), value);
          retval = TRUE;
          g_free (value);
        }
    }

  g_free (uc);
  return retval;
}

static void
update_state (HamUpdatesStatusMenuItem *self)
{
  LOG ("updating the state");

  if (update_status_menu_button_value (self)
      /* @todo || update_notifications_button_value (self) */)
    set_state (self, UPNO_STATE_BLINKING);
  else
    set_state (self, UPNO_STATE_INVISIBLE);
}

static gchar*
get_package_list_string (gchar *type)
{
  xexp *available_updates;
  xexp *seen_updates;
  gchar *retval;

  retval = NULL;

  available_updates = xexp_read_file (AVAILABLE_UPDATES_FILE);

  if (available_updates == NULL)
    goto exit;

  seen_updates = user_file_read_xexp (UFILE_SEEN_UPDATES);

  if (seen_updates == NULL)
    seen_updates = xexp_list_new ("updates");

  /* preconditions ok */
  {
    xexp *x;
    xexp *y;
    gint c;

    y = NULL;
    c = 0;

    for (x = xexp_first (available_updates); x != NULL; x = xexp_rest (x))
      {
        if (!xexp_is_text (x))
          continue;

        if ((seen_updates != NULL) && xexp_is_list (seen_updates))
          {
            const gchar *pkg;

            pkg = xexp_text (x);

            for (y = xexp_first (seen_updates); y != NULL; y = xexp_rest (y))
              if (xexp_is_text (y) && strcmp (pkg, xexp_text (y)) == 0)
                break;
          }

        if (xexp_is (x, type) && c < 3)
          {
            const gchar *pkg;

            if ((pkg = xexp_text (x)) != NULL)
              {
                c++;
                if (retval != NULL)
                  {
                    gchar *tmp;

                    tmp = retval;
                    retval = g_strconcat (tmp, ", ", pkg, NULL);
                    g_free (tmp);
                  }
                else
                  retval = g_strdup (pkg);
              }
          }
      }

      xexp_free (available_updates);

      if (seen_updates != NULL)
        xexp_free (seen_updates);

      if (c > 2)
        {
          gchar *tmp;

          tmp = retval;
          retval = g_strconcat (tmp, "...", NULL);
          g_free (tmp);
        }
    }

 exit:
  LOG ("pkgs = %s", retval);

  return retval;
}

static gchar*
concat_package_list (gchar *str, gchar *type)
{
  gchar *tmp;
  gchar *pkgs;
  gchar *retval;

  tmp = str;
  pkgs = get_package_list_string (type);
  retval = g_strconcat (tmp, "<small>", pkgs, "</small>\n\n", NULL);
  g_free (tmp);
  g_free (pkgs);

  return retval;
}

static gchar*
build_update_type_string (gchar *str, gchar *title, gchar *type)
{
  gchar *retval;

  if (str != NULL)
    {
      gchar *tmp;

      tmp = str;
      retval = g_strconcat (tmp, "<big>", title, "</big>\n", NULL);
      g_free (tmp);
    }
  else
    retval = g_strconcat ("<big>", title, "</big>\n", NULL);

  retval = concat_package_list (retval, type);

  return retval;
}

static gchar*
get_dialog_content ()
{
  gchar *str;
  UpdatesCount *uc;

  if ((uc = get_updates_count ()) == NULL)
    return NULL;

  if (uc->new == 0)
    return NULL;

  str = NULL;

  if (uc->os > 0)
    {
      gchar *title;

      title = g_strdup_printf (_("ai_sb_update_os_%d"), uc->os);
      str = build_update_type_string (str, title, "os");
      g_free (title);
    }

  if (uc->certified > 0)
    {
      gchar *title;

      title = g_strdup_printf (_("ai_sb_update_nokia_%d"), uc->certified);
      str = build_update_type_string (str, title, "certified");
      g_free (title);
    }

  if (uc->other > 0)
    {
      gchar *title;

      title = g_strdup_printf (_("ai_sb_update_thirdparty_%d"), uc->other);
      str = build_update_type_string (str, title, "other");
      g_free (title);
    }

  return str;
}

static void
ham_updates_status_menu_item_connection_cb (ConIcConnection *connection,
                                            ConIcConnectionEvent *event,
                                            gpointer data)
{
  HamUpdatesStatusMenuItemPrivate *priv;

  g_return_if_fail (IS_HAM_UPDATES_STATUS_MENU_ITEM (data));
  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (data);

  LOG ("got a connect notification");

  if (con_ic_connection_event_get_status (event) == CON_IC_STATUS_CONNECTED)
    {
      const gchar *bearer;

      bearer = con_ic_event_get_bearer_type (CON_IC_EVENT (event));

      /* XXX - only the WLAN_INFRA and the WLAN_ADHOC bearers are
               considered cheap.  There should be a general platform
               feature that tells us whether we need to be careful with
               network access or not.  */
      priv->constate = (strcmp (bearer, "WLAN_ADHOC") == 0
                       || strcmp (bearer, "WLAN_INFRA") == 0);
    }

  LOG ("we're %s", priv->constate == UPNO_CON_OFFLINE ? "offline" : "online");
}

static void
setup_connection_state (HamUpdatesStatusMenuItem *self)
{
  HamUpdatesStatusMenuItemPrivate *priv;

  priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

  if (running_in_scratchbox ())
    {
      /* if we're in scratchbox will assume that we have an inet
         connection */
      priv->constate = UPNO_CON_ONLINE;
      LOG ("we're online");
    }
  else
    {
      priv->conic = con_ic_connection_new ();

      g_signal_connect (G_OBJECT (priv->conic), "connection-event",
                        G_CALLBACK (ham_updates_status_menu_item_connection_cb),
                        self);
      g_object_set (G_OBJECT (priv->conic),
                    "automatic-connection-events", TRUE, NULL);
    }
}

static gboolean
ham_is_showing_check_for_updates_view (HamUpdatesStatusMenuItem *self)
{
  if (ham_is_running ())
    {
      HamUpdatesStatusMenuItemPrivate *priv;
      osso_return_t result;
      osso_rpc_t reply;

      priv = HAM_UPDATES_STATUS_MENU_ITEM_GET_PRIVATE (self);

      LOG ("asking if ham is showing the \"check for updates\" view");
      result = osso_rpc_run (priv->osso,
			     HILDON_APP_MGR_SERVICE,
			     HILDON_APP_MGR_OBJECT_PATH,
			     HILDON_APP_MGR_INTERFACE,
			     HILDON_APP_MGR_OP_SHOWING_CHECK_FOR_UPDATES,
			     &reply,
			     DBUS_TYPE_INVALID);

      if (result == OSSO_OK && reply.type == DBUS_TYPE_BOOLEAN)
        return reply.value.b;
    }

  return FALSE;
}