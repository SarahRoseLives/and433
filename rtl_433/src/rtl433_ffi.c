/** @file
    Flutter dart:ffi C wrapper for rtl_433.

    Sets up an rtl_433 config to receive I/Q data from an rtl_tcp server, registers all
    device decoders, routes decoded output through a pipe-connected JSON handler, and
    exposes a simple start/stop/status API suitable for use as a shared library.

    Copyright (C) 2026 and433 contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "rtl433_ffi.h"

#include "r_api.h"
#include "rtl_433.h"
#include "list.h"
#include "output_file.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#ifdef ANDROID
#include <android/log.h>
#endif

/* ---- internal state ---- */

#define FFI_STATE_STOPPED  0
#define FFI_STATE_RUNNING  1
#define FFI_STATE_ERROR   -1

typedef struct {
    r_cfg_t        *cfg;
    rtl433_data_cb  user_cb;
    void           *user_ctx;
    rtl433_log_cb   log_cb;
    void           *log_ctx;
    int             pipe_rd;      /* read end – reader thread consumes JSON lines */
    int             pipe_wr;      /* write end – rtl_433 JSON output writes here */
    FILE           *pipe_wr_file; /* FILE* wrapper around pipe_wr */
    pthread_t       run_thread;
    pthread_t       reader_thread;
    volatile int    state;
    char            dev_query[256];
} ffi_state_t;

static ffi_state_t g_ffi;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---- reader thread: reads JSON lines from the pipe and calls the user callback ---- */

static void *reader_thread_fn(void *arg)
{
    ffi_state_t *s = (ffi_state_t *)arg;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    FILE *rf = fdopen(s->pipe_rd, "r");
    if (!rf) {
        s->state = FFI_STATE_ERROR;
        return NULL;
    }
    while ((n = getline(&line, &cap, rf)) > 0) {
        /* strip trailing newline */
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
        if (line[0] != '\0') {
            s->user_cb(line, s->user_ctx);
        }
    }
    free(line);
    fclose(rf); /* also closes pipe_rd fd */
    return NULL;
}

/* ---- run thread: drives the rtl_433 event loop ---- */

static void *run_thread_fn(void *arg)
{
    ffi_state_t *s = (ffi_state_t *)arg;
    r_cfg_t *cfg = s->cfg;

    int r = r_start(cfg);
    if (r < 0) {
        s->state = FFI_STATE_ERROR;
        /* close write end so reader thread exits its getline loop */
        fclose(s->pipe_wr_file);
        s->pipe_wr_file = NULL;
        s->pipe_wr = -1;
        return NULL;
    }

    r_run(cfg); /* blocks until cfg->exit_async is set */

    /* flush and close the JSON output FILE so the reader thread sees EOF */
    if (s->pipe_wr_file) {
        fflush(s->pipe_wr_file);
        fclose(s->pipe_wr_file);
        s->pipe_wr_file = NULL;
        s->pipe_wr = -1;
    }

    s->state = FFI_STATE_STOPPED;
    return NULL;
}

/* ---- internal rtl_433 log handler — forwards to the user log callback ---- */

static void ffi_log_handler(log_level_t level, char const *src, char const *msg, void *userdata)
{
    (void)userdata;
#ifdef ANDROID
    /* Always mirror to Android logcat for adb debugging */
    int prio = level <= LOG_ERROR ? ANDROID_LOG_ERROR
             : level == LOG_WARNING ? ANDROID_LOG_WARN
             : level == LOG_NOTICE  ? ANDROID_LOG_INFO
             : ANDROID_LOG_DEBUG;
    __android_log_print(prio, "rtl433", "[%s] %s", src, msg);
#endif
    if (g_ffi.log_cb)
        g_ffi.log_cb((int)level, src, msg, g_ffi.log_ctx);
}

/* ---- public API ---- */

void rtl433_ffi_set_log_cb(rtl433_log_cb cb, void *ctx)
{
    g_ffi.log_cb  = cb;
    g_ffi.log_ctx = ctx;
    if (cb)
        r_logger_set_log_handler(ffi_log_handler, NULL);
    else
        r_logger_set_log_handler(NULL, NULL);
}

int rtl433_ffi_start(const char *dev_query,
                     uint32_t freq_hz, uint32_t sample_rate,
                     const char *gain_str, int bias_t,
                     rtl433_data_cb cb, void *ctx)
{
    pthread_mutex_lock(&g_mutex);

    if (g_ffi.state == FFI_STATE_RUNNING) {
        pthread_mutex_unlock(&g_mutex);
        return -EALREADY;
    }

    /* Save log callback across the reset — it is set before start() by the caller */
    rtl433_log_cb saved_log_cb  = g_ffi.log_cb;
    void         *saved_log_ctx = g_ffi.log_ctx;

    memset(&g_ffi, 0, sizeof(g_ffi));

    g_ffi.log_cb  = saved_log_cb;
    g_ffi.log_ctx = saved_log_ctx;
    g_ffi.pipe_rd = -1;
    g_ffi.pipe_wr = -1;

    /* Store the device query string */
    strncpy(g_ffi.dev_query, dev_query, sizeof(g_ffi.dev_query) - 1);

    /* Create the pipe for JSON output */
    int fds[2];
    if (pipe(fds) != 0) {
        pthread_mutex_unlock(&g_mutex);
        return -errno;
    }
    g_ffi.pipe_rd = fds[0];
    g_ffi.pipe_wr = fds[1];

    /* Wrap the write end in a FILE* for the JSON output handler */
    g_ffi.pipe_wr_file = fdopen(g_ffi.pipe_wr, "w");
    if (!g_ffi.pipe_wr_file) {
        int err = errno;
        close(g_ffi.pipe_rd);
        close(g_ffi.pipe_wr);
        pthread_mutex_unlock(&g_mutex);
        return -err;
    }
    /* Line-buffer so each decoded packet reaches the reader thread promptly */
    setvbuf(g_ffi.pipe_wr_file, NULL, _IOLBF, 0);

    /* Build rtl_433 config */
    r_cfg_t *cfg = r_create_cfg();
    if (!cfg) {
        fclose(g_ffi.pipe_wr_file);
        close(g_ffi.pipe_rd);
        pthread_mutex_unlock(&g_mutex);
        return -ENOMEM;
    }
    r_init_cfg(cfg);

    /* Install log handler (also activated by rtl433_ffi_set_log_cb, but set it
     * here too so verbosity log messages during r_start are captured) */
    r_logger_set_log_handler(ffi_log_handler, NULL);

    cfg->dev_query        = g_ffi.dev_query;
    cfg->frequency[0]     = freq_hz;
    cfg->frequencies      = 1;
    cfg->center_frequency = freq_hz;
    cfg->samp_rate        = sample_rate;
    cfg->dev_mode         = DEVICE_MODE_QUIT; /* exit on error rather than restart */
    cfg->verbosity        = LOG_NOTICE;        /* surface device-open + tuning messages */
    /* gain_str NULL or "" → AGC; otherwise e.g. "40" = 40 dB */
    cfg->gain_str         = (gain_str && *gain_str) ? strdup(gain_str) : NULL;
    /* bias-T: pass as sdr settings kwargs string */
    cfg->settings_str     = bias_t ? strdup("biastee=1") : NULL;

    /* Register all 200+ device decoders */
    register_all_protocols(cfg, 0);

    /* Add a JSON output that writes to our pipe */
    const char *const *well_known = well_known_output_fields(cfg);
    list_push(&cfg->output_handler,
              data_output_json_create(LOG_WARNING, g_ffi.pipe_wr_file));
    start_outputs(cfg, well_known);

    g_ffi.cfg      = cfg;
    g_ffi.user_cb  = cb;
    g_ffi.user_ctx = ctx;
    g_ffi.state    = FFI_STATE_RUNNING;

    /* Start the reader thread first so it is ready before any data arrives */
    if (pthread_create(&g_ffi.reader_thread, NULL, reader_thread_fn, &g_ffi) != 0) {
        int err = errno;
        r_free_cfg(cfg);
        fclose(g_ffi.pipe_wr_file);
        close(g_ffi.pipe_rd);
        g_ffi.state = FFI_STATE_STOPPED;
        pthread_mutex_unlock(&g_mutex);
        return -err;
    }

    /* Start the run thread (opens SDR + mongoose event loop) */
    if (pthread_create(&g_ffi.run_thread, NULL, run_thread_fn, &g_ffi) != 0) {
        int err = errno;
        g_ffi.state = FFI_STATE_ERROR;
        r_stop(cfg); /* signal reader thread to exit via pipe close in run_thread_fn */
        pthread_join(g_ffi.reader_thread, NULL);
        r_free_cfg(cfg);
        pthread_mutex_unlock(&g_mutex);
        return -err;
    }

    pthread_mutex_unlock(&g_mutex);
    return 0;
}

void rtl433_ffi_stop(void)
{
    pthread_mutex_lock(&g_mutex);

    if (g_ffi.state == FFI_STATE_STOPPED && g_ffi.cfg == NULL) {
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    if (g_ffi.cfg) {
        r_stop(g_ffi.cfg);
    }

    pthread_mutex_unlock(&g_mutex);

    /* Join outside the lock to avoid deadlock with callbacks */
    pthread_join(g_ffi.run_thread, NULL);
    pthread_join(g_ffi.reader_thread, NULL);

    pthread_mutex_lock(&g_mutex);
    if (g_ffi.cfg) {
        r_free_cfg(g_ffi.cfg);
        g_ffi.cfg = NULL;
    }
    g_ffi.state = FFI_STATE_STOPPED;
    pthread_mutex_unlock(&g_mutex);
}

int rtl433_ffi_status(void)
{
    return g_ffi.state;
}
