/*
   SSSD

   Service monitor

   Copyright (C) Simo Sorce			2008

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/param.h>
#include <time.h>
#include <string.h>
#include "config.h"
#ifdef HAVE_SYS_INOTIFY_H
#include <sys/inotify.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

/* Needed for res_init() */
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>

#include "util/util.h"
#include "popt.h"
#include "tevent.h"
#include "confdb/confdb.h"
#include "confdb/confdb_setup.h"
#include "collection.h"
#include "ini_config.h"
#include "db/sysdb.h"
#include "monitor/monitor.h"
#include "dbus/dbus.h"
#include "sbus/sssd_dbus.h"
#include "monitor/monitor_interfaces.h"

/* ping time cannot be less then once every few seconds or the
 * monitor will get crazy hammering children with messages */
#define MONITOR_DEF_PING_TIME 10
#define MONITOR_CONF_ENTRY "config/services/monitor"

struct mt_conn {
    struct sbus_connection *conn;
    struct mt_svc *svc_ptr;
};

struct mt_svc {
    struct mt_svc *prev;
    struct mt_svc *next;

    struct mt_conn *mt_conn;
    struct mt_ctx *mt_ctx;

    char *provider;
    char *command;
    char *name;
    char *identity;
    pid_t pid;

    int ping_time;

    int restarts;
    time_t last_restart;
    time_t last_pong;

    int debug_level;

    struct tevent_timer *ping_ev;
};

struct config_file_callback {
    int wd;
    int retries;
    monitor_reconf_fn fn;
    char *filename;
    time_t modified;
    struct config_file_callback *next;
    struct config_file_callback *prev;
};

struct config_file_ctx {
    TALLOC_CTX *parent_ctx;
    struct tevent_timer *timer;
    bool needs_update;
    struct mt_ctx *mt_ctx;
    struct config_file_callback *callbacks;
};

struct mt_ctx {
    struct tevent_context *ev;
    struct confdb_ctx *cdb;
    TALLOC_CTX *domain_ctx; /* Memory context for domain list */
    struct sss_domain_info *domains;
    TALLOC_CTX *service_ctx; /* Memory context for services */
    char **services;
    struct mt_svc *svc_list;
    struct sbus_connection *sbus_srv;
    struct config_file_ctx *file_ctx;
    int inotify_fd;
    int service_id_timeout;
};

static int start_service(struct mt_svc *mt_svc);

static int dbus_service_init(struct sbus_connection *conn, void *data);
static void identity_check(DBusPendingCall *pending, void *data);

static int service_send_ping(struct mt_svc *svc);
static void ping_check(DBusPendingCall *pending, void *data);

static int service_check_alive(struct mt_svc *svc);

static void set_tasks_checker(struct mt_svc *srv);
static void set_global_checker(struct mt_ctx *ctx);
static int monitor_kill_service (struct mt_svc *svc);

static int get_service_config(struct mt_ctx *ctx, const char *name,
                              struct mt_svc **svc_cfg);
static int get_provider_config(struct mt_ctx *ctx, const char *name,
                              struct mt_svc **svc_cfg);
static int add_new_service(struct mt_ctx *ctx, const char *name);
static int add_new_provider(struct mt_ctx *ctx, const char *name);

static int monitor_signal_reconf(struct config_file_ctx *file_ctx,
                                 const char *filename);
static int update_monitor_config(struct mt_ctx *ctx);
static int monitor_cleanup(void);

/* dbus_get_monitor_version
 * Return the monitor version over D-BUS */
static int dbus_get_monitor_version(DBusMessage *message,
                                    struct sbus_connection *conn)
{
    const char *version = MONITOR_VERSION;
    DBusMessage *reply;
    dbus_bool_t ret;

    reply = dbus_message_new_method_return(message);
    if (!reply) return ENOMEM;
    ret = dbus_message_append_args(reply, DBUS_TYPE_STRING,
                                   &version, DBUS_TYPE_INVALID);
    if (!ret) {
        dbus_message_unref(reply);
        return EIO;
    }

    /* send reply back */
    sbus_conn_send_reply(conn, reply);
    dbus_message_unref(reply);

    return EOK;
}

struct sbus_method monitor_methods[] = {
    { MONITOR_METHOD_VERSION, dbus_get_monitor_version},
    {NULL, NULL}
};

/* monitor_dbus_init
 * Set up the monitor service as a D-BUS Server */
static int monitor_dbus_init(struct mt_ctx *ctx)
{
    struct sbus_method_ctx *sd_ctx;
    char *monitor_address;
    int ret;

    sd_ctx = talloc_zero(ctx, struct sbus_method_ctx);
    if (!sd_ctx) {
        return ENOMEM;
    }

    monitor_address = talloc_asprintf(sd_ctx, "unix:path=%s/%s",
                                      PIPE_PATH, SSSD_SERVICE_PIPE);
    if (!monitor_address) {
        talloc_free(sd_ctx);
        return ENOMEM;
    }

    /* Set up globally-available D-BUS methods */
    sd_ctx->interface = talloc_strdup(sd_ctx, MONITOR_DBUS_INTERFACE);
    if (!sd_ctx->interface) {
        talloc_free(sd_ctx);
        return ENOMEM;
    }
    sd_ctx->path = talloc_strdup(sd_ctx, MONITOR_DBUS_PATH);
    if (!sd_ctx->path) {
        talloc_free(sd_ctx);
        return ENOMEM;
    }
    sd_ctx->methods = monitor_methods;
    sd_ctx->message_handler = sbus_message_handler;

    ret = sbus_new_server(ctx, ctx->ev, monitor_address, sd_ctx,
                          &ctx->sbus_srv, dbus_service_init, ctx);

    talloc_free(monitor_address);

    return ret;
}

static void svc_try_restart(struct mt_svc *svc, time_t now)
{
    int ret;

    DLIST_REMOVE(svc->mt_ctx->svc_list, svc);
    if (svc->last_restart != 0) {
        if ((now - svc->last_restart) > 30) { /* TODO: get val from config */
            /* it was long ago reset restart threshold */
            svc->restarts = 0;
        }
    }

    /* restart the process */
    if (svc->restarts > 3) { /* TODO: get val from config */
        DEBUG(0, ("Process [%s], definitely stopped!\n", svc->name));
        talloc_free(svc);
        return;
    }

    /* Shut down the current ping timer so it will restart
     * cleanly in start_service()
     */
    talloc_free(svc->ping_ev);

    ret = start_service(svc);
    if (ret != EOK) {
        DEBUG(0,("Failed to restart service '%s'\n", svc->name));
        talloc_free(svc);
        return;
    }

    svc->restarts++;
    svc->last_restart = now;
    return;
}

static void tasks_check_handler(struct tevent_context *ev,
                                struct tevent_timer *te,
                                struct timeval t, void *ptr)
{
    struct mt_svc *svc = talloc_get_type(ptr, struct mt_svc);
    time_t now = time(NULL);
    bool process_alive = true;
    int ret;

    ret = service_check_alive(svc);
    switch (ret) {
    case EOK:
        /* all fine */
        break;

    case ECHILD:
        DEBUG(1,("Process (%s) is stopped!\n", svc->name));
        process_alive = false;
        break;

    default:
        /* TODO: should we tear down it ? */
        DEBUG(1,("Checking for service %s(%d) failed!!\n",
                 svc->name, svc->pid));
        break;
    }

    if (process_alive) {
        ret = service_send_ping(svc);
        switch (ret) {
        case EOK:
            /* all fine */
            break;

        case ENXIO:
            DEBUG(1,("Child (%s) not responding! (yet)\n", svc->name));
            break;

        default:
            /* TODO: should we tear it down ? */
            DEBUG(1,("Sending a message to service (%s) failed!!\n", svc->name));
            break;
        }

        if (svc->last_pong != 0) {
            if ((now - svc->last_pong) > (svc->ping_time * 3)) {
                /* too long since we last heard of this process */
                DEBUG(1, ("Killing service [%s], not responding to pings!\n",
                          svc->name));
                monitor_kill_service(svc);
                process_alive = false;
            }
        }

    }

    if (!process_alive) {
        svc_try_restart(svc, now);
        return;
    }

    /* all fine, set up the task checker again */
    set_tasks_checker(svc);
}

static void set_tasks_checker(struct mt_svc *svc)
{
    struct tevent_timer *te = NULL;
    struct timeval tv;

    gettimeofday(&tv, NULL);
    tv.tv_sec += svc->ping_time;
    tv.tv_usec = 0;
    te = tevent_add_timer(svc->mt_ctx->ev, svc, tv, tasks_check_handler, svc);
    if (te == NULL) {
        DEBUG(0, ("failed to add event, monitor offline for [%s]!\n",
                  svc->name));
        /* FIXME: shutdown ? */
    }
    svc->ping_ev = te;
}

static void global_checks_handler(struct tevent_context *ev,
                                  struct tevent_timer *te,
                                  struct timeval t, void *ptr)
{
    struct mt_ctx *ctx = talloc_get_type(ptr, struct mt_ctx);
    struct mt_svc *svc;
    int status;
    pid_t pid;

    errno = 0;
    pid = waitpid(0, &status, WNOHANG);
    if (pid == 0) {
        goto done;
    }

    if (pid == -1) {
        DEBUG(0, ("waitpid returned -1 (errno:%d[%s])\n",
                  errno, strerror(errno)));
        goto done;
    }

    /* let's see if it is a known service, and try to restart it */
    for (svc = ctx->svc_list; svc; svc = svc->next) {
        if (svc->pid == pid) {
            time_t now = time(NULL);
            DEBUG(1, ("Service [%s] did exit\n", svc->name));
            svc_try_restart(svc, now);
            goto done;
        }
    }
    if (svc == NULL) {
        DEBUG(0, ("Unknown child (%d) did exit\n", pid));
    }

done:
    set_global_checker(ctx);
}

static void set_global_checker(struct mt_ctx *ctx)
{
    struct tevent_timer *te = NULL;
    struct timeval tv;

    gettimeofday(&tv, NULL);
    tv.tv_sec += 1; /* once a second */
    tv.tv_usec = 0;
    te = tevent_add_timer(ctx->ev, ctx, tv, global_checks_handler, ctx);
    if (te == NULL) {
        DEBUG(0, ("failed to add global checker event! PANIC TIME!\n"));
        /* FIXME: is this right ? shoulkd we try to clean up first ?*/
        exit(-1);
    }
}

static int monitor_kill_service (struct mt_svc *svc)
{
    int ret;
    ret = kill(svc->pid, SIGTERM);
    if (ret != EOK) {
        DEBUG(0,("Sending signal to child (%s:%d) failed! "
                 "Ignore and pretend child is dead.\n",
                 svc->name, svc->pid));
    }

    return ret;
}

static void shutdown_reply(DBusPendingCall *pending, void *data)
{
    DBusMessage *reply;
    int type;
    struct sbus_connection *conn;
    struct mt_svc *svc = talloc_get_type(data, struct mt_svc);

    conn = svc->mt_conn->conn;
    reply = dbus_pending_call_steal_reply(pending);
    if (!reply) {
        /* reply should never be null. This function shouldn't be called
         * until reply is valid or timeout has occurred. If reply is NULL
         * here, something is seriously wrong and we should bail out.
         */
        DEBUG(0, ("A reply callback was called but no reply was received"
                  " and no timeout occurred\n"));

        /* Destroy this connection */
        monitor_kill_service(svc);
        goto done;
    }

    type = dbus_message_get_type(reply);
    switch(type) {
    case DBUS_MESSAGE_TYPE_METHOD_RETURN:
        /* Ok, we received a confirmation of shutdown */
        break;

    default:
        /* Something went wrong on the client side
         * Time to forcibly kill the service
         */
        DEBUG(0, ("Received an error shutting down service.\n"));
        monitor_kill_service(svc);
    }

    /* No matter what happened here, we need to free the service */
    talloc_free(svc);

done:
    dbus_pending_call_unref(pending);
    dbus_message_unref(reply);
}

/* monitor_shutdown_service
 * Orders a monitored service to shut down cleanly
 * This function will free the memory for svc once it
 * completes.
 */
static int monitor_shutdown_service(struct mt_svc *svc)
{
    DBusConnection *dbus_conn;
    DBusMessage *msg;
    DBusPendingCall *pending_reply;
    dbus_bool_t dbret;

    /* Stop the service checker */

    dbus_conn = sbus_get_connection(svc->mt_conn->conn);

    /* Construct a shutdown message */
    msg = dbus_message_new_method_call(NULL,
                                       SERVICE_PATH,
                                       SERVICE_INTERFACE,
                                       SERVICE_METHOD_SHUTDOWN);
    if (!msg) {
        DEBUG(0,("Out of memory?!\n"));
        monitor_kill_service(svc);
        talloc_free(svc);
        return ENOMEM;
    }

    dbret = dbus_connection_send_with_reply(dbus_conn, msg, &pending_reply,
                                            svc->mt_ctx->service_id_timeout);
    if (!dbret || pending_reply == NULL) {
        DEBUG(0, ("D-BUS send failed.\n"));
        dbus_message_unref(msg);
        monitor_kill_service(svc);
        talloc_free(svc);
        return EIO;
    }

    /* Set up the reply handler */
    dbus_pending_call_set_notify(pending_reply, shutdown_reply, svc, NULL);
    dbus_message_unref(msg);

    return EOK;
}

static void reload_reply(DBusPendingCall *pending, void *data)
{
    DBusMessage *reply;
    struct sbus_connection *conn;
    struct mt_svc *svc = talloc_get_type(data, struct mt_svc);

    conn = svc->mt_conn->conn;
    reply = dbus_pending_call_steal_reply(pending);
    if (!reply) {
        /* reply should never be null. This function shouldn't be called
         * until reply is valid or timeout has occurred. If reply is NULL
         * here, something is seriously wrong and we should bail out.
         */
        DEBUG(0, ("A reply callback was called but no reply was received"
                  " and no timeout occurred\n"));

        /* Destroy this connection */
        sbus_disconnect(conn);
        goto done;
    }

    /* TODO: Handle cases where the call has timed out or returned
     * with an error.
     */
done:
    dbus_pending_call_unref(pending);
    dbus_message_unref(reply);
}

static int monitor_signal_reconf(struct config_file_ctx *file_ctx,
                                 const char *filename)
{
    int ret;
    DEBUG(1, ("Configuration has changed. Reloading.\n"));

    /* Update the confdb configuration */
    ret = confdb_init_db(filename, file_ctx->mt_ctx->cdb);
    if (ret != EOK) {
        DEBUG(0, ("Could not reload configuration!"));
        kill(getpid(), SIGTERM);
        return ret;
    }

    /* Update the monitor's configuration and signal children */
    return update_monitor_config(file_ctx->mt_ctx);
}

static int service_signal_dns_reload(struct mt_svc *svc);
static int monitor_update_resolv(struct config_file_ctx *file_ctx,
                          const char *filename)
{
    int ret;
    struct mt_svc *cur_svc;
    DEBUG(2, ("Resolv.conf has been updated. Reloading.\n"));

    ret = res_init();
    if(ret != 0) {
        return EIO;
    }

    /* Signal all services to reload their DNS configuration */
    for(cur_svc = file_ctx->mt_ctx->svc_list; cur_svc; cur_svc = cur_svc->next) {
        service_signal_dns_reload(cur_svc);
    }
    return EOK;
}

static int service_signal(struct mt_svc *svc, const char *svc_signal)
{
    DBusMessage *msg;
    dbus_bool_t dbret;
    DBusConnection *dbus_conn;
    DBusPendingCall *pending_reply;

    if (svc->provider && strcasecmp(svc->provider, "local") == 0) {
        /* The local provider requires no signaling */
        return EOK;
    }

    if (!svc->mt_conn) {
        /* Avoid a race condition where we are trying to
         * order a service to reload that hasn't started
         * yet.
         */
        DEBUG(1,("Could not signal service [%s].\n", svc->name));
        return EIO;
    }

    dbus_conn = sbus_get_connection(svc->mt_conn->conn);
    msg = dbus_message_new_method_call(NULL,
                                       SERVICE_PATH,
                                       SERVICE_INTERFACE,
                                       svc_signal);
    if (!msg) {
        DEBUG(0,("Out of memory?!\n"));
        monitor_kill_service(svc);
        talloc_free(svc);
        return ENOMEM;
    }

    dbret = dbus_connection_send_with_reply(dbus_conn, msg, &pending_reply,
                                            svc->mt_ctx->service_id_timeout);
    if (!dbret || pending_reply == NULL) {
        DEBUG(0, ("D-BUS send failed.\n"));
        dbus_message_unref(msg);
        monitor_kill_service(svc);
        talloc_free(svc);
        return EIO;
    }

    /* Set up the reply handler */
    dbus_pending_call_set_notify(pending_reply, reload_reply, svc, NULL);
    dbus_message_unref(msg);

    return EOK;
}

static int service_signal_reload(struct mt_svc *svc)
{
    return service_signal(svc, SERVICE_METHOD_RELOAD);
}
static int service_signal_dns_reload(struct mt_svc *svc)
{
    return service_signal(svc, SERVICE_METHOD_RES_INIT);
}

static int check_domain_ranges(struct sss_domain_info *domains)
{
    struct sss_domain_info *dom = domains, *other = NULL;
    uint32_t id_min, id_max;

    while (dom) {
        other = dom->next;
        if (dom->id_max && dom->id_min > dom->id_max) {
            DEBUG(1, ("Domain '%s' does not have a valid ID range\n",
                      dom->name));
            return EINVAL;
        }

        while (other) {
            id_min = MAX(dom->id_min, other->id_min);
            id_max = MIN((dom->id_max ? dom->id_max : UINT32_MAX),
                         (other->id_max ? other->id_max : UINT32_MAX));
            if (id_min <= id_max) {
                DEBUG(1, ("Domains '%s' and '%s' overlap in range %u - %u\n",
                          dom->name, other->name, id_min, id_max));
            }
            other = other->next;
        }
        dom = dom->next;
    }

    return EOK;
}

static int append_data_provider(struct mt_ctx *ctx)
{
    int i;
    char **new_services;

    for (i = 0; ctx->services[i]; i++) {
        if (strcasecmp(ctx->services[i], "dp") == 0) {
            return EOK;
        }
    }

    new_services = talloc_realloc(ctx, ctx->services, char *, i+2);
    if (new_services == NULL) {
        return ENOMEM;
    }
    ctx->services = new_services;

    ctx->services[i] = talloc_asprintf(ctx, "dp");
    if (ctx->services[i] == NULL) {
        return ENOMEM;
    }
    ctx->services[i+1] = NULL;
    DEBUG(4, ("Added mandatory service Data Provider\n"));

    return EOK;
}

int get_monitor_config(struct mt_ctx *ctx)
{
    int ret;

    ret = confdb_get_int(ctx->cdb, ctx,
                         MONITOR_CONF_ENTRY, "sbusTimeout",
                         -1, &ctx->service_id_timeout);
    if (ret != EOK) {
        return ret;
    }

    ctx->service_ctx = talloc_new(ctx);
    if(!ctx->service_ctx) {
        return ENOMEM;
    }
    ret = confdb_get_string_as_list(ctx->cdb, ctx->service_ctx,
                                    SERVICE_CONF_ENTRY, "activeServices",
                                    &ctx->services);
    if (ret != EOK) {
        DEBUG(0, ("No services configured!\n"));
        return EINVAL;
    }
    ret = append_data_provider(ctx);
    if (ret != EOK) {
        DEBUG(0, ("Could not add Data Provider to the list of services!\n"));
        return ret;
    }

    ctx->domain_ctx = talloc_new(ctx);
    if(!ctx->domain_ctx) {
        return ENOMEM;
    }
    ret = confdb_get_domains(ctx->cdb, ctx->domain_ctx, &ctx->domains);
    if (ret != EOK) {
        DEBUG(2, ("No domains configured. LOCAL should always exist!\n"));
        return ret;
    }

    /* Check UID/GID overlaps */
    ret = check_domain_ranges(ctx->domains);
    if (ret != EOK) {
        return ret;
    }

    return EOK;
}

static int get_service_config(struct mt_ctx *ctx, const char *name,
                              struct mt_svc **svc_cfg)
{
    int ret;
    char *path;
    struct mt_svc *svc;

    *svc_cfg = NULL;

    svc = talloc_zero(ctx, struct mt_svc);
    if (!svc) {
        return ENOMEM;
    }
    svc->mt_ctx = ctx;

    svc->name = talloc_strdup(svc, name);
    if (!svc->name) {
        talloc_free(svc);
        return ENOMEM;
    }

    svc->identity = talloc_strdup(svc, name);
    if (!svc->identity) {
        talloc_free(svc);
        return ENOMEM;
    }

    path = talloc_asprintf(svc, "config/services/%s", svc->name);
    if (!path) {
        talloc_free(svc);
        return ENOMEM;
    }

    ret = confdb_get_string(ctx->cdb, svc, path, "command",
                            NULL, &svc->command);
    if (ret != EOK) {
        DEBUG(0,("Failed to start service '%s'\n", svc->name));
        talloc_free(svc);
        return ret;
    }

    if (!svc->command) {
        svc->command = talloc_asprintf(svc, "%s/sssd_%s -d %d%s",
                                       SSSD_LIBEXEC_PATH,
                                       svc->name, debug_level,
                                       (debug_timestamps?
                                              " --debug-timestamps":""));
        if (!svc->command) {
            talloc_free(svc);
            return ENOMEM;
        }
    }

    ret = confdb_get_int(ctx->cdb, svc, path, "timeout",
                         MONITOR_DEF_PING_TIME, &svc->ping_time);
    if (ret != EOK) {
        DEBUG(0,("Failed to start service '%s'\n", svc->name));
        talloc_free(svc);
        return ret;
    }

    *svc_cfg = svc;
    talloc_free(path);

    return EOK;
}

static int add_new_service(struct mt_ctx *ctx, const char *name)
{
    int ret;
    struct mt_svc *svc;

    ret = get_service_config(ctx, name, &svc);

    ret = start_service(svc);
    if (ret != EOK) {
        DEBUG(0,("Failed to start service '%s'\n", svc->name));
        talloc_free(svc);
    }

    return ret;
}

static int get_provider_config(struct mt_ctx *ctx, const char *name,
                              struct mt_svc **svc_cfg)
{
    int ret;
    char *path;
    struct mt_svc *svc;

    *svc_cfg = NULL;

    svc = talloc_zero(ctx, struct mt_svc);
    if (!svc) {
        return ENOMEM;
    }
    svc->mt_ctx = ctx;

    svc->name = talloc_strdup(svc, name);
    if (!svc->name) {
        talloc_free(svc);
        return ENOMEM;
    }

    svc->identity = talloc_asprintf(svc, "%%BE_%s", svc->name);
    if (!svc->identity) {
        talloc_free(svc);
        return ENOMEM;
    }

    path = talloc_asprintf(svc, "config/domains/%s", name);
    if (!path) {
        talloc_free(svc);
        return ENOMEM;
    }

    ret = confdb_get_string(ctx->cdb, svc, path,
                            "provider", NULL, &svc->provider);
    if (ret != EOK) {
        DEBUG(0, ("Failed to find provider from [%s] configuration\n", name));
        talloc_free(svc);
        return ret;
    }

    ret = confdb_get_string(ctx->cdb, svc, path,
                            "command", NULL, &svc->command);
    if (ret != EOK) {
        DEBUG(0, ("Failed to find command from [%s] configuration\n", name));
        talloc_free(svc);
        return ret;
    }

    ret = confdb_get_int(ctx->cdb, svc, path, "timeout",
                         MONITOR_DEF_PING_TIME, &svc->ping_time);
    if (ret != EOK) {
        DEBUG(0,("Failed to start service '%s'\n", svc->name));
        talloc_free(svc);
        return ret;
    }

    talloc_free(path);

    /* if no provider is present do not run the domain */
    if (!svc->provider) {
        talloc_free(svc);
        return EIO;
    }

    /* if there are no custom commands, build a default one */
    if (!svc->command) {
        svc->command = talloc_asprintf(svc,
                            "%s/sssd_be -d %d%s --provider %s --domain %s",
                            SSSD_LIBEXEC_PATH, debug_level,
                            (debug_timestamps?" --debug-timestamps":""),
                            svc->provider, svc->name);
        if (!svc->command) {
            talloc_free(svc);
            return ENOMEM;
        }
    }

    *svc_cfg = svc;
    return EOK;
}

static int add_new_provider(struct mt_ctx *ctx, const char *name)
{
    int ret;
    struct mt_svc *svc;

    ret = get_provider_config(ctx, name, &svc);
    if (ret != EOK) {
        DEBUG(0, ("Could not get provider configuration for [%s]\n",
                  name));
        return ret;
    }

    ret = start_service(svc);
    if (ret != EOK) {
        DEBUG(0,("Failed to start service '%s'\n", svc->name));
        talloc_free(svc);
    }

    return ret;
}

static void remove_service(struct mt_ctx *ctx, const char *name)
{
    int ret;
    struct mt_svc *cur_svc;

    /* Locate the service object in the list */
    cur_svc = ctx->svc_list;
    while (cur_svc != NULL) {
        if (strcasecmp(name, cur_svc->name) == 0)
            break;
        cur_svc = cur_svc->next;
    }
    if (cur_svc != NULL) {
        /* Remove the service from the list */
        DLIST_REMOVE(ctx->svc_list, cur_svc);

        /* Shut it down */
        ret = monitor_shutdown_service(cur_svc);
        if (ret != EOK) {
            DEBUG(0, ("Unable to shut down service [%s]!",
                      name));
            /* TODO: Handle this better */
        }
    }
}

static int update_monitor_config(struct mt_ctx *ctx)
{
    int ret, i, j;
    struct mt_svc *cur_svc;
    struct mt_svc *new_svc;
    struct sss_domain_info *dom, *new_dom;
    struct mt_ctx *new_config;

    new_config = talloc_zero(NULL, struct mt_ctx);
    if(!new_config) {
        return ENOMEM;
    }

    new_config->ev = ctx->ev;
    new_config->cdb = ctx->cdb;
    ret = get_monitor_config(new_config);

    ctx->service_id_timeout = new_config->service_id_timeout;

    /* Compare the old and new active services */
    /* Have any services been shut down? */
    for (i = 0; ctx->services[i]; i++) {
        /* Search for this service in the new config */
        for (j = 0; new_config->services[j]; j++) {
            if (strcasecmp(ctx->services[i], new_config->services[j]) == 0)
                break;
        }
        if (new_config->services[j] == NULL) {
            /* This service is no longer configured.
             * Shut it down.
             */
            remove_service(ctx, ctx->services[i]);
        }
    }

    /* Have any services been added or changed? */
    for (i = 0; new_config->services[i]; i++) {
        /* Search for this service in the old config */
        for (j = 0; ctx->services[j]; j++) {
            if (strcasecmp(new_config->services[i], ctx->services[j]) == 0)
                break;
        }

        if (ctx->services[j] == NULL) {
            /* New service added */
            add_new_service(ctx, new_config->services[i]);
        }
        else {
            /* Service already enabled, check for changes */
            /* Locate the service object in the list */
            cur_svc = ctx->svc_list;
            for (cur_svc = ctx->svc_list; cur_svc; cur_svc = cur_svc->next) {
                if (strcasecmp(ctx->services[i], cur_svc->name) == 0)
                    break;
            }
            if (cur_svc == NULL) {
                DEBUG(0, ("Service entry missing data\n"));
                /* This shouldn't be possible, but if it happens
                 * we'll throw an error
                 */
                talloc_free(new_config);
                return EIO;
            }

            /* Read in the new configuration and compare it with the
             * old one.
             */
            ret = get_service_config(ctx, new_config->services[i], &new_svc);
            if (ret != EOK) {
                DEBUG(0, ("Unable to determine if service has changed.\n"));
                DEBUG(0, ("Disabling service [%s].\n",
                          new_config->services[i]));
                /* Not much we can do here, no way to know whether the
                 * current configuration is safe, and restarting the
                 * service won't work because the new startup requires
                 * this function to work. The only safe thing to do
                 * is stop the service.
                 */
                remove_service(ctx, new_config->services[i]);
                continue;
            }

            if (strcmp(cur_svc->command, new_svc->command) != 0) {
                /* The executable path has changed. We need to
                 * restart the binary completely. If we send a
                 * shutdown command, the monitor will automatically
                 * reload the process with the new command.
                 */
                talloc_free(cur_svc->command);
                talloc_steal(cur_svc, new_svc->command);
                cur_svc->command = new_svc->command;

                /* TODO: be more graceful about this */
                monitor_kill_service(cur_svc);
            }

            cur_svc->ping_time = new_svc->ping_time;

            talloc_free(new_svc);
        }
    }

    /* Replace the old service list with the new one */
    talloc_free(ctx->service_ctx);
    ctx->service_ctx = talloc_steal(ctx, new_config->service_ctx);
    ctx->services = new_config->services;

    /* Compare data providers */
    /* Have any providers been disabled? */
    for (dom = ctx->domains; dom; dom = dom->next) {
        for (new_dom = new_config->domains; new_dom; new_dom = new_dom->next) {
            if (strcasecmp(dom->name, new_dom->name) == 0) break;
        }
        if (new_dom == NULL) {
            /* This provider is no longer configured
             * Shut it down
             */
            remove_service(ctx, dom->name);
        }
    }

    /* Have we added or changed any providers? */
    for (new_dom = new_config->domains; new_dom; new_dom = new_dom->next) {
        /* Search for this service in the old config */
        for (dom = ctx->domains; dom; dom = dom->next) {
            if (strcasecmp(dom->name, new_dom->name) == 0) break;
        }

        if (dom == NULL) {
            /* New provider added */
            add_new_provider(ctx, new_dom->name);
        }
        else {
            /* Provider is already in the list.
             * Check for changes.
             */
            /* Locate the service object in the list */
            cur_svc = ctx->svc_list;
            while (cur_svc != NULL) {
                if (strcasecmp(new_dom->name, cur_svc->name) == 0)
                    break;
                cur_svc = cur_svc->next;
            }
            if (cur_svc == NULL) {
                DEBUG(0, ("Service entry missing data for [%s]\n", new_dom->name));
                /* This shouldn't be possible
                 */
                talloc_free(new_config);
                return EIO;
            }

            /* Read in the new configuration and compare it with
             * the old one.
             */
            ret = get_provider_config(ctx, new_dom->name, &new_svc);
            if (ret != EOK) {
                DEBUG(0, ("Unable to determine if service has changed.\n"));
                DEBUG(0, ("Disabling service [%s].\n",
                          new_config->services[i]));
                /* Not much we can do here, no way to know whether the
                 * current configuration is safe, and restarting the
                 * service won't work because the new startup requires
                 * this function to work. The only safe thing to do
                 * is stop the service.
                 */
                remove_service(ctx, dom->name);
                continue;
            }

            if ((strcmp(cur_svc->command, new_svc->command) != 0) ||
                (strcmp(cur_svc->provider, new_svc->provider) != 0)) {
                /* The executable path or the provider has changed.
                 * We need to restart the binary completely. If we
                 * send a shutdown command, the monitor will
                 * automatically reload the process with the new
                 * command.
                 */
                talloc_free(cur_svc->command);
                talloc_steal(cur_svc, new_svc->command);
                cur_svc->command = new_svc->command;

                /* TODO: be more graceful about this */
                monitor_kill_service(cur_svc);
            }

            cur_svc->ping_time = new_svc->ping_time;
        }

    }

    /* Replace the old domain list with the new one */
    talloc_free(ctx->domain_ctx);
    ctx->domain_ctx = talloc_steal(ctx, new_config->domain_ctx);
    ctx->domains = new_config->domains;

    /* Signal all services to reload their configuration */
    for(cur_svc = ctx->svc_list; cur_svc; cur_svc = cur_svc->next) {
        service_signal_reload(cur_svc);
    }

    talloc_free(new_config);
    return EOK;
}

static void monitor_hup(struct tevent_context *ev,
                        struct tevent_signal *se,
                        int signum,
                        int count,
                        void *siginfo,
                        void *private_data)
{
    struct mt_ctx *ctx = talloc_get_type(private_data, struct mt_ctx);

    DEBUG(1, ("Received SIGHUP. Rereading configuration.\n"));
    update_monitor_config(ctx);
}

static int monitor_cleanup(void)
{
    char *file;
    int ret;
    TALLOC_CTX *tmp_ctx;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) return ENOMEM;

    file = talloc_asprintf(tmp_ctx, "%s/%s.pid", PID_PATH, "sssd");
    if (file == NULL) {
        return ENOMEM;
    }

    errno = 0;
    ret = unlink(file);
    if (ret == -1) {
        ret = errno;
        DEBUG(0, ("Error removing pidfile! (%d [%s])\n",
                ret, strerror(ret)));
        talloc_free(file);
        return errno;
    }

    talloc_free(file);
    return EOK;
}

static void monitor_quit(struct tevent_context *ev,
                         struct tevent_signal *se,
                         int signum,
                         int count,
                         void *siginfo,
                         void *private_data)
{
    monitor_cleanup();

#if HAVE_GETPGRP
    if (getpgrp() == getpid()) {
        DEBUG(0,("%s: killing children\n", strsignal(signum)));
        kill(-getpgrp(), SIGTERM);
    }
#endif

    exit(0);
}

int read_config_file(const char *config_file)
{
    int ret;
    struct collection_item *sssd_config = NULL;
    struct collection_item *error_list = NULL;

    /* Read the configuration into a collection */
    ret = config_from_file("sssd", config_file, &sssd_config,
                           INI_STOP_ON_ANY, &error_list);
    if (ret != EOK) {
        DEBUG(0, ("Parse error reading configuration file [%s]\n",
                  config_file));
        print_file_parsing_errors(stderr, error_list);
    }

    free_ini_config_errors(error_list);
    free_ini_config(sssd_config);
    return ret;
}

#ifdef HAVE_SYS_INOTIFY_H
static void process_config_file(struct tevent_context *ev,
                                struct tevent_timer *te,
                                struct timeval t, void *ptr);

static void config_file_changed(struct tevent_context *ev,
                                struct tevent_fd *fde,
                                uint16_t flags, void *data)
{
    struct tevent_timer *te = NULL;
    struct timeval tv;
    struct config_file_ctx *file_ctx;

    file_ctx = talloc_get_type(data, struct config_file_ctx);
    if (file_ctx->needs_update) {
        /* Skip updating. It's already queued for update.
         */
        return;
    }

    /* We will queue the file for update in one second.
     * This way, if there is a script writing to the file
     * repeatedly, we won't be attempting to update multiple
     * times.
     */
    gettimeofday(&tv, NULL);
    tv.tv_sec += 1;

    te = tevent_add_timer(ev, ev, tv, process_config_file, file_ctx);
    if (!te) {
        DEBUG(0, ("Unable to queue config file update! Exiting."));
        kill(getpid(), SIGTERM);
        return;
    }
    file_ctx->needs_update = 1;
}

struct rewatch_ctx {
    struct config_file_callback *cb;
    struct config_file_ctx *file_ctx;
};
static void rewatch_config_file(struct tevent_context *ev,
                                struct tevent_timer *te,
                                struct timeval t, void *ptr);
static void process_config_file(struct tevent_context *ev,
                                struct tevent_timer *te,
                                struct timeval t, void *ptr)
{
    TALLOC_CTX *tmp_ctx;
    struct inotify_event *in_event;
    char *buf;
    char *name;
    ssize_t len, total_len;
    ssize_t event_size;
    struct config_file_ctx *file_ctx;
    struct config_file_callback *cb;
    struct rewatch_ctx *rw_ctx;

    event_size = sizeof(struct inotify_event);
    file_ctx = talloc_get_type(ptr, struct config_file_ctx);

    DEBUG(1, ("Processing config file changes\n"));

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) return;

    buf = talloc_size(tmp_ctx, event_size);
    if (!buf) {
        talloc_free(tmp_ctx);
        return;
    }

    total_len = 0;
    while (total_len < event_size) {
        len = read(file_ctx->mt_ctx->inotify_fd, buf+total_len,
                   event_size-total_len);
        if (len == -1 && errno != EINTR) {
            DEBUG(0, ("Critical error reading inotify file descriptor.\n"));
            talloc_free(tmp_ctx);
            return;
        }
        total_len += len;
    }

    in_event = (struct inotify_event *)buf;

    if (in_event->len > 0) {
        /* Read in the name, even though we don't use it,
         * so that read ptr is in the right place
         */
        name = talloc_size(tmp_ctx, len);
        if (!name) {
            talloc_free(tmp_ctx);
            return;
        }
        total_len = 0;
        while (total_len < in_event->len) {
            len = read(file_ctx->mt_ctx->inotify_fd, &name, in_event->len);
            if (len == -1 && errno != EINTR) {
                DEBUG(0, ("Critical error reading inotify file descriptor.\n"));
                talloc_free(tmp_ctx);
                return;
            }
            total_len += len;
        }
    }

    talloc_free(tmp_ctx);

    for (cb = file_ctx->callbacks; cb; cb = cb->next) {
        if (cb->wd == in_event->wd) {
            break;
        }
    }
    if (!cb) {
        DEBUG(0, ("Unknown watch descriptor\n"));
        return;
    }

    if (in_event->mask & IN_IGNORED) {
        /* Some text editors will move a new file on top of the
         * existing one instead of modifying it. In this case,
         * the kernel will send us an IN_IGNORE signal.
         * We will try to open a new watch descriptor on the
         * new file.
         */
        struct timeval tv;
        struct tevent_timer *tev;
        tv.tv_sec = t.tv_sec+5;
        tv.tv_usec = t.tv_usec;

        cb->retries = 0;
        rw_ctx = talloc(file_ctx, struct rewatch_ctx);
        if(!rw_ctx) {
            DEBUG(0, ("Could not restore inotify watch. Quitting!\n"));
            close(file_ctx->mt_ctx->inotify_fd);
            kill(getpid(), SIGTERM);
            return;
        }
        rw_ctx->cb = cb;
        rw_ctx->file_ctx = file_ctx;

        tev = tevent_add_timer(ev, rw_ctx, tv, rewatch_config_file, rw_ctx);
        if (te == NULL) {
            DEBUG(0, ("Could not restore inotify watch. Quitting!\n"));
            close(file_ctx->mt_ctx->inotify_fd);
            kill(getpid(), SIGTERM);
        }
        return;
    }

    /* Tell the monitor to signal the children */
    cb->fn(file_ctx, cb->filename);
    file_ctx->needs_update = 0;
}

static void rewatch_config_file(struct tevent_context *ev,
                                struct tevent_timer *te,
                                struct timeval t, void *ptr)
{
    int err;
    struct tevent_timer *tev = NULL;
    struct timeval tv;
    struct config_file_callback *cb;

    struct rewatch_ctx *rw_ctx;
    struct config_file_ctx *file_ctx;

    rw_ctx = talloc_get_type(ptr, struct rewatch_ctx);

    cb = rw_ctx->cb;
    file_ctx = rw_ctx->file_ctx;

    /* Retry six times at five-second intervals before giving up */
    cb->retries++;
    if (cb->retries > 6) {
        DEBUG(0, ("Could not restore inotify watch. Quitting!\n"));
        close(file_ctx->mt_ctx->inotify_fd);
        kill(getpid(), SIGTERM);
    }

    cb->wd = inotify_add_watch(file_ctx->mt_ctx->inotify_fd,
                               cb->filename, IN_MODIFY);
    if (cb->wd < 0) {
        err = errno;

        tv.tv_sec = t.tv_sec+5;
        tv.tv_usec = t.tv_usec;

        DEBUG(1, ("Could not add inotify watch for file [%s]. Error [%d:%s]\n",
                  cb->filename, err, strerror(err)));

        tev = tevent_add_timer(ev, ev, tv, rewatch_config_file, rw_ctx);
        if (te == NULL) {
            DEBUG(0, ("Could not restore inotify watch. Quitting!\n"));
            close(file_ctx->mt_ctx->inotify_fd);
            kill(getpid(), SIGTERM);
        }

        return;
    }
    cb->retries = 0;

    /* Tell the monitor to signal the children */
    cb->fn(file_ctx, cb->filename);

    talloc_free(rw_ctx);
    file_ctx->needs_update = 0;
}
#endif

static void poll_config_file(struct tevent_context *ev,
                                    struct tevent_timer *te,
                                    struct timeval t, void *ptr)
{
    int ret, err;
    struct stat file_stat;
    struct timeval tv;
    struct config_file_ctx *file_ctx;
    struct config_file_callback *cb;

    file_ctx = talloc_get_type(ptr,struct config_file_ctx);

    for (cb = file_ctx->callbacks; cb; cb = cb->next) {
        ret = stat(cb->filename, &file_stat);
        if (ret < 0) {
            err = errno;
            DEBUG(0, ("Could not stat file [%s]. Error [%d:%s]\n",
                      cb->filename, err, strerror(err)));
            /* TODO: If the config file is missing, should we shut down? */
            return;
        }

        if (file_stat.st_mtime != cb->modified) {
            /* Parse the configuration file and signal the children */
            /* Note: this will fire if the modification time changes into the past
             * as well as the future.
             */
            DEBUG(1, ("Config file changed\n"));
            cb->modified = file_stat.st_mtime;

            /* Tell the monitor to signal the children */
            cb->fn(file_ctx, cb->filename);
        }
    }

    gettimeofday(&tv, NULL);
    tv.tv_sec += CONFIG_FILE_POLL_INTERVAL;
    tv.tv_usec = 0;
    file_ctx->timer = tevent_add_timer(ev, file_ctx->parent_ctx, tv,
                             poll_config_file, file_ctx);
    if (!file_ctx->timer) {
        DEBUG(0, ("Error: Config file no longer monitored for changes!"));
    }
}

static int try_inotify(struct config_file_ctx *file_ctx, const char *filename,
                       monitor_reconf_fn fn)
{
#ifdef HAVE_SYS_INOTIFY_H
    int err, fd_args, ret;
    struct tevent_fd *tfd;
    struct config_file_callback *cb;

    /* Monitoring the file descriptor should be global */
    if (!file_ctx->mt_ctx->inotify_fd) {
        /* Set up inotify to monitor the config file for changes */
        file_ctx->mt_ctx->inotify_fd = inotify_init();
        if (file_ctx->mt_ctx->inotify_fd < 0) {
            err = errno;
            DEBUG(0, ("Could not initialize inotify, error [%d:%s]\n",
                      err, strerror(err)));
            return err;
        }

        fd_args = fcntl(file_ctx->mt_ctx->inotify_fd, F_GETFL, NULL);
        if (fd_args < 0) {
            /* Could not set nonblocking */
            close(file_ctx->mt_ctx->inotify_fd);
            return EINVAL;
        }

        fd_args |= O_NONBLOCK;
        ret = fcntl(file_ctx->mt_ctx->inotify_fd, F_SETFL, fd_args);
        if (ret < 0) {
            /* Could not set nonblocking */
            close(file_ctx->mt_ctx->inotify_fd);
            return EINVAL;
        }

        /* Add the inotify file descriptor to the TEvent context */
        tfd = tevent_add_fd(file_ctx->mt_ctx->ev, file_ctx,
                            file_ctx->mt_ctx->inotify_fd,
                            TEVENT_FD_READ, config_file_changed,
                            file_ctx);
        if (!tfd) {
            close(file_ctx->mt_ctx->inotify_fd);
            return EIO;
        }
    }

    cb = talloc_zero(file_ctx, struct config_file_callback);
    if(!cb) {
        close(file_ctx->mt_ctx->inotify_fd);
        return EIO;
    }

    cb->filename = talloc_strdup(cb, filename);
    if (!cb->filename) {
        close(file_ctx->mt_ctx->inotify_fd);
        return ENOMEM;
    }
    cb->wd = inotify_add_watch(file_ctx->mt_ctx->inotify_fd,
                               cb->filename, IN_MODIFY);
    if (cb->wd < 0) {
        err = errno;
        DEBUG(0, ("Could not add inotify watch for file [%s]. Error [%d:%s]\n",
                  cb->filename, err, strerror(err)));
        close(file_ctx->mt_ctx->inotify_fd);
        return err;
    }
    cb->fn = fn;

    DLIST_ADD(file_ctx->callbacks, cb);

    return EOK;
#else
    return EINVAL;
#endif
}

static int monitor_config_file(TALLOC_CTX *mem_ctx,
                               struct mt_ctx *ctx,
                               const char *file,
                               monitor_reconf_fn fn)
{
    int ret, err;
    struct timeval tv;
    struct stat file_stat;
    struct config_file_callback *cb = NULL;

    ret = stat(file, &file_stat);
    if (ret < 0) {
        err = errno;
        DEBUG(0, ("Could not stat file [%s]. Error [%d:%s]\n",
                  file, err, strerror(err)));
        return err;
    }
    if (!ctx->file_ctx) {
        ctx->file_ctx = talloc_zero(mem_ctx, struct config_file_ctx);
        if (!ctx->file_ctx) return ENOMEM;

        ctx->file_ctx->parent_ctx = mem_ctx;
        ctx->file_ctx->mt_ctx = ctx;
    }
    ret = try_inotify(ctx->file_ctx, file, fn);
    if (ret != EOK) {
        /* Could not monitor file with inotify, fall back to polling */
        cb = talloc_zero(ctx->file_ctx, struct config_file_callback);
        if (!cb) {
            talloc_free(ctx->file_ctx);
            return ENOMEM;
        }
        cb->filename = talloc_strdup(cb, file);
        if (!cb->filename) {
            talloc_free(ctx->file_ctx);
            return ENOMEM;
        }
        cb->fn = fn;
        cb->modified = file_stat.st_mtime;

        DLIST_ADD(ctx->file_ctx->callbacks, cb);

        if(!ctx->file_ctx->timer) {
            gettimeofday(&tv, NULL);
            tv.tv_sec += CONFIG_FILE_POLL_INTERVAL;
            tv.tv_usec = 0;
            ctx->file_ctx->timer = tevent_add_timer(ctx->ev, mem_ctx, tv,
                                   poll_config_file, ctx->file_ctx);
            if (!ctx->file_ctx->timer) {
                talloc_free(ctx->file_ctx);
                return EIO;
            }
        }
    }

    return EOK;
}

int monitor_process_init(TALLOC_CTX *mem_ctx,
                         struct tevent_context *event_ctx,
                         struct confdb_ctx *cdb,
                         const char *config_file)
{
    struct mt_ctx *ctx;
    struct sysdb_ctx *sysdb;
    struct tevent_signal *tes;
    int ret, i;
    char *cdb_file;
    struct sss_domain_info *dom;

    ctx = talloc_zero(mem_ctx, struct mt_ctx);
    if (!ctx) {
        DEBUG(0, ("fatal error initializing monitor!\n"));
        return ENOMEM;
    }
    ctx->ev = event_ctx;
    ctx->cdb = cdb;

    /* Initialize the CDB from the configuration file */
    ret = confdb_test(ctx->cdb);
    if (ret == ENOENT) {
        /* First-time setup */

        /* Purge any existing confdb in case an old
         * misconfiguration gets in the way
         */
        talloc_free(ctx->cdb);
        cdb_file = talloc_asprintf(ctx, "%s/%s", DB_PATH, CONFDB_FILE);
        if (cdb_file == NULL) {
            DEBUG(0,("Out of memory, aborting!\n"));
            talloc_free(ctx);
            return ENOMEM;
        }

        unlink(cdb_file);
        ret = confdb_init(ctx, ctx->ev, &ctx->cdb, cdb_file);
        if (ret != EOK) {
            DEBUG(0,("The confdb initialization failed\n"));
            talloc_free(ctx);
            return ret;
        }

        /* Load special entries */
        ret = confdb_create_base(ctx->cdb);
        if (ret != EOK) {
            talloc_free(ctx);
            return ret;
        }
    } else if (ret != EOK) {
        DEBUG(0, ("Fatal error initializing confdb\n"));
        talloc_free(ctx);
        return ret;
    }

    ret = confdb_init_db(config_file, ctx->cdb);
    if (ret != EOK) {
        DEBUG(0, ("ConfDB initialization has failed [%s]\n",
              strerror(ret)));
        talloc_free(ctx);
        return ret;
    }

    /* Read in the monitor's configuration */
    ret = get_monitor_config(ctx);
    if (ret != EOK) {
        talloc_free(ctx);
        return ret;
    }

    /* Watch for changes to the confdb config file */
    ret = monitor_config_file(ctx, ctx, config_file, monitor_signal_reconf);
    if (ret != EOK) {
        talloc_free(ctx);
        return ret;
    }

    /* Watch for changes to the DNS resolv.conf */
    ret = monitor_config_file(ctx, ctx, RESOLV_CONF_PATH,
                              monitor_update_resolv);
    if (ret != EOK) {
        talloc_free(ctx);
        return ret;
    }

    /* Avoid a startup race condition between InfoPipe
     * and NSS. If the sysdb doesn't exist yet, both
     * will try to create it at the same time. So
     * we'll have the monitor create it before either of
     * those processes start.
     */
    ret = sysdb_init(mem_ctx, ctx->ev, ctx->cdb,
                     NULL, &sysdb);
    if (ret != EOK) {
        talloc_free(ctx);
        return ret;
    }
    talloc_free(sysdb);

    /* Initialize D-BUS Server
     * The monitor will act as a D-BUS server for all
     * SSSD processes */
    ret = monitor_dbus_init(ctx);
    if (ret != EOK) {
        talloc_free(ctx);
        return ret;
    }

    /* start all services */
    for (i = 0; ctx->services[i]; i++) {
        add_new_service(ctx, ctx->services[i]);
    }

    /* now start the data providers */
    for (dom = ctx->domains; dom; dom = dom->next) {
        add_new_provider(ctx, dom->name);
    }

    /* now start checking for global events */
    set_global_checker(ctx);

    /* Set up an event handler for a SIGHUP */
    tes = tevent_add_signal(ctx->ev, ctx, SIGHUP, 0,
                            monitor_hup, ctx);
    if (tes == NULL) {
        talloc_free(ctx);
        return EIO;
    }

    /* Set up an event handler for a SIGINT */
    tes = tevent_add_signal(ctx->ev, ctx, SIGINT, 0,
                            monitor_quit, ctx);
    if (tes == NULL) {
        talloc_free(ctx);
        return EIO;
    }

    /* Set up an event handler for a SIGTERM */
    tes = tevent_add_signal(ctx->ev, ctx, SIGTERM, 0,
                            monitor_quit, ctx);
    if (tes == NULL) {
        talloc_free(ctx);
        return EIO;
    }

    return EOK;
}

static int mt_conn_destructor(void *ptr)
{
    struct mt_conn *mt_conn;
    struct mt_svc *svc;

    mt_conn = talloc_get_type(ptr, struct mt_conn);
    svc = mt_conn->svc_ptr;

    /* now clear up so that the rest of the code will know there
     * is no connection attached to the service anymore */
    svc->mt_conn = NULL;

    return 0;
}

/*
 * dbus_service_init
 * This function should initiate a query to the newly connected
 * service to discover the service's identity (invoke the getIdentity
 * method on the new client). The reply callback for this request
 * should set the connection destructor appropriately.
 */
static int dbus_service_init(struct sbus_connection *conn, void *data)
{
    struct mt_ctx *ctx;
    struct mt_svc *svc;
    struct mt_conn *mt_conn;
    DBusMessage *msg;
    DBusPendingCall *pending_reply;
    DBusConnection *dbus_conn;
    dbus_bool_t dbret;

    DEBUG(3, ("Initializing D-BUS Service\n"));

    ctx = talloc_get_type(data, struct mt_ctx);
    dbus_conn = sbus_get_connection(conn);

    /* hang off this memory to the connection so that when the connection
     * is freed we can call a destructor to clear up the structure and
     * have a way to know we need to restart the service */
    mt_conn = talloc(conn, struct mt_conn);
    if (!mt_conn) {
        DEBUG(0,("Out of memory?!\n"));
        talloc_free(conn);
        return ENOMEM;
    }
    mt_conn->conn = conn;

    /* at this stage we still do not know what service is this
     * we will know only after we get its identity, so we make
     * up a temporary fake service and complete the operation
     * when we receive the reply */
    svc = talloc_zero(mt_conn, struct mt_svc);
    if (!svc) {
        talloc_free(conn);
        return ENOMEM;
    }
    svc->mt_ctx = ctx;
    svc->mt_conn = mt_conn;

    mt_conn->svc_ptr = svc;
    talloc_set_destructor((TALLOC_CTX *)mt_conn, mt_conn_destructor);

    /*
     * Set up identity request
     * This should be a well-known path and method
     * for all services
     */
    msg = dbus_message_new_method_call(NULL,
                                       SERVICE_PATH,
                                       SERVICE_INTERFACE,
                                       SERVICE_METHOD_IDENTITY);
    if (msg == NULL) {
        DEBUG(0,("Out of memory?!\n"));
        talloc_free(conn);
        return ENOMEM;
    }
    dbret = dbus_connection_send_with_reply(dbus_conn, msg, &pending_reply,
                                            ctx->service_id_timeout);
    if (!dbret || pending_reply == NULL) {
        /*
         * Critical Failure
         * We can't communicate on this connection
         * We'll drop it using the default destructor.
         */
        DEBUG(0, ("D-BUS send failed.\n"));
        dbus_message_unref(msg);
        talloc_free(conn);
        return EIO;
    }

    /* Set up the reply handler */
    dbus_pending_call_set_notify(pending_reply, identity_check, svc, NULL);
    dbus_message_unref(msg);

    return EOK;
}

static void identity_check(DBusPendingCall *pending, void *data)
{
    struct mt_svc *fake_svc;
    struct mt_svc *svc;
    struct sbus_connection *conn;
    DBusMessage *reply;
    DBusError dbus_error;
    dbus_uint16_t svc_ver;
    char *svc_name;
    dbus_bool_t ret;
    int type;

    fake_svc = talloc_get_type(data, struct mt_svc);
    conn = fake_svc->mt_conn->conn;
    dbus_error_init(&dbus_error);

    reply = dbus_pending_call_steal_reply(pending);
    if (!reply) {
        /* reply should never be null. This function shouldn't be called
         * until reply is valid or timeout has occurred. If reply is NULL
         * here, something is seriously wrong and we should bail out.
         */
        DEBUG(0, ("Serious error. A reply callback was called but no reply was received and no timeout occurred\n"));

        /* Destroy this connection */
        sbus_disconnect(conn);
        goto done;
    }

    type = dbus_message_get_type(reply);
    switch (type) {
    case DBUS_MESSAGE_TYPE_METHOD_RETURN:
        ret = dbus_message_get_args(reply, &dbus_error,
                                    DBUS_TYPE_STRING, &svc_name,
                                    DBUS_TYPE_UINT16, &svc_ver,
                                    DBUS_TYPE_INVALID);
        if (!ret) {
            DEBUG(1,("Failed, to parse message, killing connection\n"));
            if (dbus_error_is_set(&dbus_error)) dbus_error_free(&dbus_error);
            sbus_disconnect(conn);
            goto done;
        }

        DEBUG(4,("Received ID reply: (%s,%d)\n", svc_name, svc_ver));

        /* search this service in the list */
        svc = fake_svc->mt_ctx->svc_list;
        while (svc) {
            ret = strcasecmp(svc->identity, svc_name);
            if (ret == 0) {
                break;
            }
            svc = svc->next;
        }
        if (!svc) {
            DEBUG(0,("Unable to find peer [%s] in list of services, killing connection!\n", svc_name));
            sbus_disconnect(conn);
            goto done;
        }

        /* transfer all from the fake service and get rid of it */
        fake_svc->mt_conn->svc_ptr = svc;
        svc->mt_conn = fake_svc->mt_conn;
        talloc_free(fake_svc);

        DEBUG(1, ("Service %s connected\n", svc->name));

        /* Set up the destructor for this service */
        break;

    case DBUS_MESSAGE_TYPE_ERROR:
        DEBUG(0,("getIdentity returned an error [%s], closing connection.\n",
                 dbus_message_get_error_name(reply)));
        /* Falling through to default intentionally*/
    default:
        /*
         * Timeout or other error occurred or something
         * unexpected happened.
         * It doesn't matter which, because either way we
         * know that this connection isn't trustworthy.
         * We'll destroy it now.
         */
        sbus_disconnect(conn);
        return;
    }

done:
    dbus_pending_call_unref(pending);
    dbus_message_unref(reply);
}

/* service_send_ping
 * this function send a dbus ping to a service.
 * It returns EOK if all is fine or ENXIO if the connection is
 * not available (either not yet set up or teared down).
 * Returns e generic error in other cases.
 */
static int service_send_ping(struct mt_svc *svc)
{
    DBusMessage *msg;
    DBusPendingCall *pending_reply;
    DBusConnection *dbus_conn;
    dbus_bool_t dbret;

    if (!svc->mt_conn) {
        return ENXIO;
    }

    DEBUG(4,("Pinging %s\n", svc->name));

    dbus_conn = sbus_get_connection(svc->mt_conn->conn);

    /*
     * Set up identity request
     * This should be a well-known path and method
     * for all services
     */
    msg = dbus_message_new_method_call(NULL,
                                       SERVICE_PATH,
                                       SERVICE_INTERFACE,
                                       SERVICE_METHOD_PING);
    if (!msg) {
        DEBUG(0,("Out of memory?!\n"));
        talloc_free(svc->mt_conn->conn);
        return ENOMEM;
    }

    dbret = dbus_connection_send_with_reply(dbus_conn, msg, &pending_reply,
                                            svc->mt_ctx->service_id_timeout);
    if (!dbret || pending_reply == NULL) {
        /*
         * Critical Failure
         * We can't communicate on this connection
         * We'll drop it using the default destructor.
         */
        DEBUG(0, ("D-BUS send failed.\n"));
        talloc_free(svc->mt_conn->conn);
        return EIO;
    }

    /* Set up the reply handler */
    dbus_pending_call_set_notify(pending_reply, ping_check, svc, NULL);
    dbus_message_unref(msg);

    return EOK;
}

static void ping_check(DBusPendingCall *pending, void *data)
{
    struct mt_svc *svc;
    struct sbus_connection *conn;
    DBusMessage *reply;
    const char *dbus_error_name;
    int type;

    svc = talloc_get_type(data, struct mt_svc);
    conn = svc->mt_conn->conn;

    reply = dbus_pending_call_steal_reply(pending);
    if (!reply) {
        /* reply should never be null. This function shouldn't be called
         * until reply is valid or timeout has occurred. If reply is NULL
         * here, something is seriously wrong and we should bail out.
         */
        DEBUG(0, ("A reply callback was called but no reply was received"
                  " and no timeout occurred\n"));

        /* Destroy this connection */
        sbus_disconnect(conn);
        goto done;
    }

    type = dbus_message_get_type(reply);
    switch (type) {
    case DBUS_MESSAGE_TYPE_METHOD_RETURN:
        /* ok peer replied,
         * set the reply timestamp into the service structure */

        DEBUG(4,("Service %s replied to ping\n", svc->name));

        svc->last_pong = time(NULL);
        break;

    case DBUS_MESSAGE_TYPE_ERROR:

        dbus_error_name = dbus_message_get_error_name(reply);

        /* timeouts are handled in the main service check function */
        if (strcmp(dbus_error_name, DBUS_ERROR_TIMEOUT) == 0)
            break;

        DEBUG(0,("A service PING returned an error [%s], closing connection.\n",
                 dbus_error_name));
        /* Falling through to default intentionally*/
    default:
        /*
         * Timeout or other error occurred or something
         * unexpected happened.
         * It doesn't matter which, because either way we
         * know that this connection isn't trustworthy.
         * We'll destroy it now.
         */
        sbus_disconnect(conn);
    }

done:
    dbus_pending_call_unref(pending);
    dbus_message_unref(reply);
}



/* service_check_alive
 * This function checks if the service child is still alive
 */
static int service_check_alive(struct mt_svc *svc)
{
    int status;
    pid_t pid;

    DEBUG(4,("Checking service %s(%d) is still alive\n", svc->name, svc->pid));

    pid = waitpid(svc->pid, &status, WNOHANG);
    if (pid == 0) {
        return EOK;
    }

    if (pid != svc->pid) {
        DEBUG(1, ("bad return (%d) from waitpid() waiting for %d\n",
                  pid, svc->pid));
        /* TODO: what do we do now ? */
        return EINVAL;
    }

    if (WIFEXITED(status)) { /* children exited on it's own */
        /* TODO: check configuration to see if it was removed
         * from the list of process to run */
        DEBUG(0,("Process [%s] exited\n", svc->name));
    }

    return ECHILD;
}

static void free_args(char **args)
{
    int i;

    if (args) {
        for (i = 0; args[i]; i++) free(args[i]);
        free(args);
    }
}


/* parse a string into arguments.
 * arguments are separated by a space
 * '\' is an escape character and can be used only to escape
 * itself or the white space.
 */
static char **parse_args(const char *str)
{
    const char *p;
    char **ret, **r;
    char *tmp;
    int num;
    int i, e;

    tmp = malloc(strlen(str) + 1);
    if (!tmp) return NULL;

    ret = NULL;
    num = 0;
    e = 0;
    i = 0;
    p = str;
    while (*p) {
        switch (*p) {
        case '\\':
            if (e) {
                tmp[i] = '\\';
                i++;
                e = 0;
            } else {
                e = 1;
            }
            break;
        case ' ':
            if (e) {
                tmp[i] = ' ';
                i++;
                e = 0;
            } else {
                tmp[i] = '\0';
                i++;
            }
            break;
        default:
            if (e) {
                tmp[i] = '\\';
                i++;
                e = 0;
            }
            tmp[i] = *p;
            i++;
            break;
        }

        p++;

        /* check if this was the last char */
        if (*p == '\0') {
            if (e) {
                tmp[i] = '\\';
                i++;
                e = 0;
            }
            tmp[i] = '\0';
            i++;
        }
        if (tmp[i-1] != '\0' || strlen(tmp) == 0) {
            /* check next char and skip multiple spaces */
            continue;
        }

        r = realloc(ret, (num + 2) * sizeof(char *));
        if (!r) goto fail;
        ret = r;
        ret[num+1] = NULL;
        ret[num] = strdup(tmp);
        if (!ret[num]) goto fail;
        num++;
        i = 0;
    }

    free(tmp);
    return ret;

fail:
    free(tmp);
    free_args(ret);
    return NULL;
}

static void service_startup_handler(struct tevent_context *ev,
                                    struct tevent_timer *te,
                                    struct timeval t, void *ptr);

static int start_service(struct mt_svc *svc)
{
    struct tevent_timer *te;
    struct timeval tv;

    DEBUG(4,("Queueing service %s for startup\n", svc->name));

    /* Add a timed event to start up the service.
     * We have to do this in order to avoid a race
     * condition where the service being started forks
     * and attempts to connect to the SBUS before
     * the monitor is serving it.
     */
    gettimeofday(&tv, NULL);
    te = tevent_add_timer(svc->mt_ctx->ev, svc, tv,
                         service_startup_handler, svc);
    if (te == NULL) {
        DEBUG(0, ("Unable to queue service %s for startup\n", svc->name));
        return ENOMEM;
    }
    return EOK;
}

static int delist_service(void *ptr) {
    struct mt_svc *svc =
        talloc_get_type(ptr, struct mt_svc);
    DLIST_REMOVE(svc->mt_ctx->svc_list, svc);
    return 0;
}

static void service_startup_handler(struct tevent_context *ev,
                                    struct tevent_timer *te,
                                    struct timeval t, void *ptr)
{
    struct mt_svc *mt_svc;
    char **args;

    mt_svc = talloc_get_type(ptr, struct mt_svc);
    if (mt_svc == NULL) {
        return;
    }

    if (mt_svc->provider && strcasecmp(mt_svc->provider, "local") == 0) {
        /* The LOCAL provider requires no back-end currently
         * We'll add it to the service list, but we don't need
         * to poll it.
         */
        DLIST_ADD(mt_svc->mt_ctx->svc_list, mt_svc);
        talloc_set_destructor((TALLOC_CTX *)mt_svc, delist_service);
        return;
    }

    mt_svc->pid = fork();
    if (mt_svc->pid != 0) {
        if (mt_svc->pid == -1) {
            DEBUG(0, ("Could not fork child to start service [%s]. Continuing.\n", mt_svc->name))
            return;
        }

        /* Parent */
        mt_svc->last_pong = time(NULL);
        DLIST_ADD(mt_svc->mt_ctx->svc_list, mt_svc);
        talloc_set_destructor((TALLOC_CTX *)mt_svc, delist_service);
        set_tasks_checker(mt_svc);

        return;
    }

    /* child */

    args = parse_args(mt_svc->command);
    execvp(args[0], args);

    /* If we are here, exec() has failed
     * Print errno and abort quickly */
    DEBUG(0,("Could not exec %s, reason: %s\n", mt_svc->command, strerror(errno)));

    /* We have to call _exit() instead of exit() here
     * because a bug in D-BUS will cause the server to
     * close its socket at exit() */
    _exit(1);
}

int main(int argc, const char *argv[])
{
    int opt;
    poptContext pc;
    int opt_daemon = 0;
    int opt_interactive = 0;
    char *opt_config_file = NULL;
    char *config_file = NULL;
    int flags = 0;
    struct main_context *main_ctx;
    int ret;

    struct poptOption long_options[] = {
        POPT_AUTOHELP
        SSSD_MAIN_OPTS
        {"daemon", 'D', POPT_ARG_NONE, &opt_daemon, 0, \
         "Become a daemon (default)", NULL }, \
        {"interactive",	'i', POPT_ARG_NONE, &opt_interactive, 0, \
         "Run interactive (not a daemon)", NULL}, \
        {"config", 'c', POPT_ARG_STRING, &opt_config_file, 0, \
         "Specify a non-default config file", NULL}, \
        { NULL }
    };

    pc = poptGetContext(argv[0], argc, argv, long_options, 0);
    while((opt = poptGetNextOpt(pc)) != -1) {
        switch(opt) {
        default:
            fprintf(stderr, "\nInvalid option %s: %s\n\n",
                    poptBadOption(pc, 0), poptStrerror(opt));
            poptPrintUsage(pc, stderr, 0);
            return 1;
        }
    }

    if (opt_daemon && opt_interactive) {
        fprintf(stderr, "Option -i|--interactive is not allowed together with -D|--daemon\n");
        poptPrintUsage(pc, stderr, 0);
        return 1;
    }

    poptFreeContext(pc);

    if (opt_daemon) flags |= FLAGS_DAEMON;
    if (opt_interactive) flags |= FLAGS_INTERACTIVE;

    if (opt_config_file)
        config_file = talloc_strdup(NULL, opt_config_file);
    else
        config_file = talloc_strdup(NULL, CONFDB_DEFAULT_CONFIG_FILE);
    if(!config_file)
        return 6;

    /* we want a pid file check */
    flags |= FLAGS_PID_FILE;

    /* Parse config file, fail if cannot be done */
    ret = read_config_file(config_file);
    if (ret != EOK) return 4;

    /* set up things like debug , signals, daemonization, etc... */
    ret = server_setup("sssd", flags, MONITOR_CONF_ENTRY, &main_ctx);
    if (ret != EOK) return 2;

    ret = monitor_process_init(main_ctx,
                               main_ctx->event_ctx,
                               main_ctx->confdb_ctx,
                               config_file);
    if (ret != EOK) return 3;
    talloc_free(config_file);

    /* loop on main */
    server_loop(main_ctx);

    ret = monitor_cleanup();
    if (ret != EOK) return 5;

    return 0;
}
