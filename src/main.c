/*
 * gstreamill main.
 *
 * Copyright (C) Zhang Ping <dqzhangp@163.com>
 *
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <locale.h>
#include <pwd.h>
#include <grp.h>
#include <signal.h>

#include "gstreamill.h"
#include "httpstreaming.h"
#include "httpmgmt.h"
#include "parson.h"
#include "jobdesc.h"
#include "tssegment.h"
#include "log.h"

#define GSTREAMILL_USER "gstreamill"
#define GSTREAMILL_GROUP "gstreamill"
#define PID_FILE "/run/gstreamill/gstreamill.pid"

GST_DEBUG_CATEGORY(ACCESS);

GST_DEBUG_CATEGORY(GSTREAMILL);
#define GST_CAT_DEFAULT GSTREAMILL

Log *_log;

static void sighandler (gint number)
{
    _log->log_hd = freopen (_log->log_path, "w", _log->log_hd);
}

static void stop_job (gint number)
{
    GDateTime *datetime;
    gchar *date;

    datetime = g_date_time_new_now_local ();
    date = g_date_time_format (datetime, "%b %d %H:%M:%S");
    GST_WARNING ("\n\n*** %s : job stoped ***", date);
    g_free (date);
    g_date_time_unref (datetime);

    exit (0);
}

/*
 * idle thread for signal SIGTERM - stop the gstreamill.
 * other thread for signal SIGTERM would cause dead lock.
 */
static gpointer idle_thread (gpointer data)
{
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    if (pthread_sigmask (SIG_UNBLOCK, &set, NULL) != 0) {
        g_printf ("sigprocmask failure: %s", strerror (errno));
        exit (-1);
    }

    for (;;) {
        sleep (3600);
    }
}

static void print_version_info ()
{
    guint major, minor, micro, nano;
    const gchar *nano_str;

    gst_version (&major, &minor, &micro, &nano);
    if (nano == 1) {
        nano_str = "(git)";

    } else if (nano == 2) {
        nano_str = "(Prerelease)";

    } else {
        nano_str = "";
    }

    g_print ("gstreamill version: %s\n", VERSION);
    g_print ("gstreamill build: %s %s\n", __DATE__, __TIME__);
    g_print ("gstreamer version : %d.%d.%d %s\n", major, minor, micro, nano_str);
}

#if 0
static gint set_user_and_group ()
{
    struct passwd *pwd;
    struct group *grp;

    /* set group id */
    errno = 0;
    grp = getgrnam (GSTREAMILL_GROUP);
    if ((grp == NULL) && (errno == -1)) {
        perror ("getgrnam failed");
        return 4;

    } else if (grp == NULL) {
        g_print ("getgrnam(\"%s\") failed\n", GSTREAMILL_GROUP);
        return 5;
    }
    if (setgid (grp->gr_gid) == -1) {
        perror ("setuid failure");
        return 6;
    }

    /* set user id */
    errno = 0;
    pwd = getpwnam (GSTREAMILL_USER);
    if ((pwd == NULL) && (errno == -1)) {
        perror ("getpwnam failed");
        return 1;

    } else if (pwd == NULL) {
        g_print ("getpwnam(\"%s\") failed\n", GSTREAMILL_USER);
        return 2;
    }
    if (setuid (pwd->pw_uid) == -1) {
        perror ("setuid failure");
        return 3;
    }

    return 0;
}
#endif

static gint prepare_gstreamill_run_dir ()
{
    gchar *run_dir;
    struct group *grp;

    /* mkdir */
    run_dir = g_path_get_dirname (PID_FILE);
    if (g_file_test (run_dir, G_FILE_TEST_EXISTS)) {
        g_free (run_dir);
        return 0;
    }
    if (g_mkdir (run_dir, 0755) == -1) {
        perror ("chown of gstreamill run directory failure");
        g_free (run_dir);
        return 1;
    }

    /* set own and mod of dir */
    grp = getgrnam (GSTREAMILL_GROUP);
    if ((grp == NULL) && (errno == -1)) {
        perror ("getgrnam failed");
        return 2;

    } else if (grp == NULL) {
        g_print ("getgrnam(\"%s\") failed\n", GSTREAMILL_GROUP);
        return 3;
    }
    if (chown (run_dir, -1, grp->gr_gid) == -1) {
        perror ("chown of gstreamill run directory failure");
        g_free (run_dir);
        return 4;
    }
    if (chmod (run_dir, 0775) == -1) {
        perror ("chmod of gstreamill run directory failure");
        g_free (run_dir);
        return 5;
    }
    g_free (run_dir);

    return 0;
}

static gboolean stop = FALSE;
static gboolean debug = FALSE;
static gboolean version = FALSE;
static gchar *job_file = NULL;
static gchar *log_dir = "/var/log/gstreamill";
static gchar *http_mgmt = "0.0.0.0:20118";
static gchar *http_streaming = "0.0.0.0:20119";
static gchar *shm_name = NULL;
static gint job_length = -1;
static gint shm_length = -1;
static GOptionEntry options[] = {
    {"job", 'j', 0, G_OPTION_ARG_FILENAME, &job_file, ("-j /full/path/to/job.file: Specify a job file, full path is must."), NULL},
    {"log", 'l', 0, G_OPTION_ARG_FILENAME, &log_dir, ("-l /full/path/to/log: Specify log path, full path is must."), NULL},
    {"httpmgmt", 'm', 0, G_OPTION_ARG_STRING, &http_mgmt, ("-m http managment address, default is 0.0.0.0:20118."), NULL},
    {"httpstreaming", 'a', 0, G_OPTION_ARG_STRING, &http_streaming, ("-a http streaming address, default is 0.0.0.0:20119."), NULL},
    {"name", 'n', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &shm_name, NULL, NULL},
    {"joblength", 'q', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_INT, &job_length, NULL, NULL},
    {"shmlength", 't', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_INT, &shm_length, NULL, NULL},
    {"stop", 's', 0, G_OPTION_ARG_NONE, &stop, ("Stop gstreamill."), NULL},
    {"debug", 'd', 0, G_OPTION_ARG_NONE, &debug, ("Debug mode, run in foreground."), NULL},
    {"version", 'v', 0, G_OPTION_ARG_NONE, &version, ("display version information and exit."), NULL},
    {NULL}
};

static gint create_pid_file ()
{
    gchar *pid;
    GError *err = NULL;

    pid = g_strdup_printf ("%d", getpid ());
    if (!g_file_set_contents (PID_FILE, pid, strlen (pid), &err)) {
        GST_ERROR ("write pid %s failure: %s\n", PID_FILE, err->message);
        g_error_free (err);
        g_free (pid);
        return 1;
    }
    g_free (pid);

    return 0;
}

static void remove_pid_file ()
{
    if (g_unlink (PID_FILE) == -1) {
        GST_ERROR ("unlink pid file error: %s", g_strerror (errno));
    }
}

Gstreamill *gstreamill;

static void stop_gstreamill (gint number)
{
    /* run in SINGLE_JOB_MODE? just exit */
    if (gstreamill->mode == SINGLE_JOB_MODE) {
        g_printf ("Interrupt signal received\n");
        exit (0);
    }

    /* run in background, stop gstreamill and remove pid file. */
    gstreamill_stop (gstreamill);
    if (number == SIGTERM) {
        remove_pid_file ();
    }
}


static int isrunning_gstreamill()
{
    int ret = 0;

    if (g_file_test (PID_FILE, G_FILE_TEST_EXISTS)) {
        gchar* cmd = g_malloc(512);
        g_sprintf(cmd, "cat %s | head -n 1 | xargs -i ps {} 2>&1 >/dev/null", PID_FILE);
        ret = WEXITSTATUS(system(cmd));

        //exist daemon
        if(ret == 0){
            ret = 1;
        }else{
            g_sprintf(cmd, "rm -f %s", PID_FILE);
            ret = system(cmd);
            ret = 0;
        }

        g_free (cmd);
    }else{
        //do nothing
    }

    return ret;
}

int main (int argc, char *argv[])
{
    HTTPMgmt *httpmgmt;
    HTTPStreaming *httpstreaming;
    GMainLoop *loop;
    GOptionContext *ctx;
    GError *err = NULL;
    gint mode;
    struct rlimit rlim;
    GDateTime *datetime;
    gchar exe_path[512], *date;
    Log *log;

    ctx = g_option_context_new (NULL);
    g_option_context_add_main_entries (ctx, options, NULL);
    g_option_context_add_group (ctx, gst_init_get_option_group ());
    if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
        g_print ("Error initializing: %s\n", GST_STR_NULL (err->message));
        exit (1);
    }
    g_option_context_free (ctx);
    GST_DEBUG_CATEGORY_INIT (ACCESS, "access", 0, "gstreamill access");
    GST_DEBUG_CATEGORY_INIT (GSTREAMILL, "gstreamill", 0, "gstreamill log");

    if (version) {
        print_version_info ();
        exit (0);
    }

    /* stop gstreamill. */
    if (stop) {
        gchar *pid_str;
        gint pid;

        g_file_get_contents (PID_FILE, &pid_str, NULL, NULL);
        if (pid_str == NULL) {
            g_print ("File %s not found, check if gstreamill is running.\n", PID_FILE);
            exit (1);
        }
        pid = atoi (pid_str);
        g_free (pid_str);
        g_print ("stoping gstreamill with pid %d ...\n", pid);
        kill (pid, SIGTERM);
        exit (0);
    }

    /* readlink exe path before setuid, on CentOS, readlink exe path after setgid/setuid failure on permission denied */
    memset (exe_path, '\0', sizeof (exe_path));
    if (readlink ("/proc/self/exe", exe_path, sizeof (exe_path)) == -1) {
        g_print ("Read /proc/self/exe error: %s", g_strerror (errno));
        exit (2);
    }

    if (prepare_gstreamill_run_dir () != 0) {
        g_print ("Can't create gstreamill run directory\n");
        exit (3);
    }
    /*
       if (set_user_and_group () != 0) {
       g_print ("set user and group failure\n");
       exit (4);
       }
     */

    mode = DAEMON_MODE;
    if (job_file != NULL) {
        /* gstreamill command with job, running in single job mode */
        mode = SINGLE_JOB_MODE;

    } else if (debug) {
        /* gstreamill command without job, run in background */
        mode = DEBUG_MODE;
    }

    if (gst_debug_get_default_threshold () < GST_LEVEL_WARNING) {
        gst_debug_set_default_threshold (GST_LEVEL_WARNING);
    }

    /* initialize ts segment static plugin */
    if (!gst_plugin_register_static (GST_VERSION_MAJOR,
                GST_VERSION_MINOR,
                "tssegment",
                "ts segment plugin",
                ts_segment_plugin_init,
                "0.1.0",
                "GPL",
                "GStreamer",
                "GStreamer",
                "http://gstreamer.net/")) {
        GST_ERROR ("registe tssegment error");
        exit (17);
    }

    /* subprocess, create_job_process */
    if (shm_name != NULL) {
        gint fd;
        gchar *job_desc, *p;
        Job *job;
        gchar *log_path, *name;
        gint ret;
        sigset_t set;

        sigemptyset(&set);
        sigaddset(&set, SIGTERM);
        sigaddset(&set, SIGUSR1);
        if (sigprocmask (SIG_UNBLOCK, &set, NULL) != 0) {
            GST_ERROR ("sigprocmask failure: %s", strerror (errno));
            exit (-1);
        }

        /* set subprocess maximum of core file */
        rlim.rlim_cur = 0;
        rlim.rlim_max = 0;
        if (setrlimit (RLIMIT_CORE, &rlim) == -1) {
            GST_ERROR ("setrlimit error: %s", g_strerror (errno));
        }

        /* read job description from share memory */
        job_desc = NULL;
        fd = shm_open (shm_name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
        if (fd == -1) {
            GST_ERROR ("shm_open error");
            exit (5);
        }
        if (ftruncate (fd, shm_length) == -1) {
            GST_ERROR ("ftruncate error");
            exit (5);
        }
        p = mmap (NULL, shm_length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            GST_ERROR ("mmap error: %s", g_strerror (errno));
            exit (5);
        }
        job_desc = g_strndup (p, job_length);

        if ((job_desc != NULL) && (!jobdesc_is_valid (job_desc))) {
            exit (6);
        }

        /* initialize log */
        name = (gchar *)jobdesc_get_name (job_desc);
        if (!jobdesc_is_live (job_desc)) {
            gchar *path;

            path = jobdesc_get_log_path (job_desc);
            log_path = g_build_filename (path, "gstreamill.log", NULL);
            g_free (path);

        } else {
            log_path = g_build_filename (log_dir, name, "gstreamill.log", NULL);
        }

        /* initialize log */
        _log = log_new ("log_path", log_path, NULL);
        g_free (log_path);
        ret = log_set_log_handler (_log);
        if (ret != 0) {
            exit (7);
        }

        /* remove gstInfo default handler. */
        gst_debug_remove_log_function (gst_debug_log_default);

        /* launch a job. */
        datetime = g_date_time_new_now_local ();
        date = g_date_time_format (datetime, "%b %d %H:%M:%S");
        fprintf (_log->log_hd, "\n*** %s : job %s starting ***\n", date, name);
        g_date_time_unref (datetime);
        g_free (date);
        job = job_new ("name", name, "job", job_desc, NULL);
        job->is_live = jobdesc_is_live (job_desc);
        job->eos = FALSE;
        loop = g_main_loop_new (NULL, FALSE);

        GST_INFO ("Initializing job ...");
        if (job_initialize (job, TRUE, fd, p) != 0) {
            GST_ERROR ("initialize job failure, exit");
            exit (8);
        }
        GST_INFO ("Initializing job done");

        GST_INFO ("Initializing job's encoders output ...");
        if (job_encoders_output_initialize (job) != 0) {
            GST_ERROR ("initialize job encoders' output failure, exit");
            exit (8);
        }
        GST_INFO ("Initializing job's encoders output done");

        GST_INFO ("Starting job ...");
        if (job_start (job) != 0) {
            GST_WARNING ("start livejob failure, exit");
            exit (9);
        }
        datetime = g_date_time_new_now_local ();
        date = g_date_time_format (datetime, "%b %d %H:%M:%S");
        fprintf (_log->log_hd, "\n*** %s : job %s started ***\n", date, name);
        g_date_time_unref (datetime);
        g_free (date);
        g_free (name);
        g_free (job_desc);

        signal (SIGPIPE, SIG_IGN);
        signal (SIGUSR1, sighandler);
        signal (SIGTERM, stop_job);

        g_main_loop_run (loop);

    } else {
        /* set parent process maximum of core file */
        rlim.rlim_cur = RLIM_INFINITY;
        rlim.rlim_max = RLIM_INFINITY;
        if (setrlimit (RLIMIT_CORE, &rlim) == -1) {
            GST_ERROR ("setrlimit error: %s", g_strerror (errno));
        }
    }

    /* gstreamill is running? */
    if (isrunning_gstreamill()) {
        g_print ("gstreamill already running !!!\n");
        exit (10);
    }

    /* not SINGLE_JOB_MODE and DEAMON_MODE or DEBUG_MODE? */
    if (mode != SINGLE_JOB_MODE) {
        gchar *path, *log_path, *access_path;
        gint ret;
        sigset_t set;

        sigemptyset(&set);
        sigaddset(&set, SIGTERM);
        if (sigprocmask (SIG_BLOCK, &set, NULL) != 0) {
            g_printf ("sigprocmask failure: %s", strerror (errno));
            exit (-1);
        }
#if 0
        /* pid file exist? */
        if (g_file_test (PID_FILE, G_FILE_TEST_EXISTS)) {
            g_print ("file %s found, gstreamill already running !!!\n", PID_FILE);
            exit (10);
        }
#endif
        /* media directory */
        path = g_strdup_printf ("%s/dvr", MEDIA_LOCATION);
        if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
            g_printf ("Create DVR directory: %s", path);
            if (g_mkdir_with_parents (path, 0755) != 0) {
                g_printf ("Create DVR directory failure: %s", path);
            }
        }
        g_free (path);
        path = g_strdup_printf ("%s/transcode/in", MEDIA_LOCATION);
        if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
            g_printf ("Create transcode directory: %s", path);
            if (g_mkdir_with_parents (path, 0755) != 0) {
                g_printf ("Create transcode directory failure: %s", path);
            }
        }
        g_free (path);
        path = g_strdup_printf ("%s/transcode/out", MEDIA_LOCATION);
        if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
            g_printf ("Create transcode directory: %s", path);
            if (g_mkdir_with_parents (path, 0755) != 0) {
                g_printf ("Create transcode directory failure: %s", path);
            }
        }
        g_free (path);

        /* if DAEMON_MODE or DEBUG_MODE then initialize log */
        if ((mode == DAEMON_MODE) || (mode == DEBUG_MODE)) {
            /* initialize log */
            log_path = g_build_filename (log_dir, "gstreamill.log", NULL);
            access_path = g_build_filename (log_dir, "access.log", NULL);
            log = log_new ("log_path", log_path, "access_path", access_path, NULL);
            g_free (log_path);
            g_free (access_path);
            ret = log_set_log_handler (log);
            if (ret != 0) {
                g_print ("Init log error, ret %d.\n", ret);
                exit (11);
            }

            /* remove gstInfo default handler. */
            gst_debug_remove_log_function (gst_debug_log_default);
        }

        /* if DAEMON_MODE then daemonize */
        if (mode == DAEMON_MODE) {
            /* daemonize */
            if (daemon (0, 0) != 0) {
                fprintf (log->log_hd, "Failed to daemonize");
                remove_pid_file ();
                exit (1);
            }

            /* create pid file */
            if (create_pid_file () != 0) {
                exit (1);
            }
        }

        /* customize signal */
        signal (SIGTERM, stop_gstreamill);

        datetime = g_date_time_new_now_local ();
        date = g_date_time_format (datetime, "%b %d %H:%M:%S");
        GST_WARNING ("\n\n*** %s : gstreamill started ***", date);
        g_free (date);
        g_date_time_unref (datetime);
    }

    /* ignore SIGPIPE */
    signal (SIGPIPE, SIG_IGN);

    loop = g_main_loop_new (NULL, FALSE);

    /* start gstreamill */
    if ((mode == DAEMON_MODE) || (mode == DEBUG_MODE)) {
        gstreamill = gstreamill_new ("mode", mode, "log_dir", log_dir, "log", log, "exe_path", exe_path, NULL);

    } else {
        gstreamill = gstreamill_new ("mode", mode, "log_dir", log_dir, "exe_path", exe_path, NULL);
    }
    if (gstreamill_start (gstreamill) != 0) {
        GST_ERROR ("start gstreamill error, exit.");
        remove_pid_file ();
        exit (12);
    }

    g_thread_new ("idle_thread", idle_thread, NULL);

    /* httpstreaming, pull */
    httpstreaming = httpstreaming_new ("gstreamill", gstreamill, "address", http_streaming, NULL);
    if (httpstreaming_start (httpstreaming, 10) != 0) {
        GST_ERROR ("start httpstreaming error, exit.");
        remove_pid_file ();
        exit (13);
    }

    if (mode != SINGLE_JOB_MODE) {
        /* run in background, management via http */
        httpmgmt = httpmgmt_new ("gstreamill", gstreamill, "address", http_mgmt, NULL);
        if (httpmgmt_start (httpmgmt) != 0) {
            GST_ERROR ("start http mangment error, exit.");
            remove_pid_file ();
            exit (14);
        }

    } else {
        /* run in foreground, start job */
        gchar *job, *p, *result;
        JSON_Value *val;
        JSON_Object *obj;

        /* ctrl-c, stop gstreamill */
        signal (SIGINT, stop_gstreamill);

        /* ctrl-\, stop gstreamill */
        signal (SIGQUIT, stop_gstreamill);

        if (!g_file_get_contents (job_file, &job, NULL, NULL)) {
            GST_ERROR ("Read job file %s error.", job_file);
            exit (15);
        }
        p = gstreamill_job_start (gstreamill, job);
        val = json_parse_string (p);
        obj = json_value_get_object (val);
        result = (gchar *)json_object_get_string (obj, "result");
        GST_INFO ("start job result: %s.", result);
        if (g_strcmp0 (result, "success") != 0) {
            exit (16);
        }
        json_value_free (val);
        g_free (p);
    }

    g_main_loop_run (loop);

    return 0;
}

