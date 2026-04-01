/** @file
    Generic RF data receiver and decoder for ISM band devices using RTL-SDR and SoapySDR.

    Copyright (C) 2019 Christian W. Zuckschwerdt <zany@triq.net>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "r_api.h"
#include "r_util.h"
#include "rtl_433.h"
#include "r_private.h"
#include "rtl_433_devices.h"
#include "r_device.h"
#include "pulse_slicer.h"
#include "pulse_detect_fsk.h"
#include "sdr.h"
#include "data.h"
#include "data_tag.h"
#include "list.h"
#include "optparse.h"
#include "output_file.h"
#include "output_log.h"
#include "output_udp.h"
#include "output_mqtt.h"
#include "output_influx.h"
#include "output_trigger.h"
#include "output_rtltcp.h"
#include "write_sigrok.h"
#include "mongoose.h"
#include "compat_time.h"
#include "logger.h"
#include "fatal.h"
#include "http_server.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#ifdef _MSC_VER
#define F_OK 0
#endif
#endif
#ifndef _MSC_VER
#include <unistd.h>
#endif

#ifndef _MSC_VER
#include <getopt.h>
#else
#include "getopt/getopt.h"
#endif

#include <signal.h>
#include "baseband.h"
#include "pulse_analyzer.h"
#include "pulse_detect.h"
#include "raw_output.h"
#include "samp_grab.h"
#include "am_analyze.h"
#include "fileformat.h"
#include "pulse_data.h"

char const *version_string(void)
{
    return "rtl_433"
#ifdef GIT_VERSION
#define STR_VALUE(arg) #arg
#define STR_EXPAND(s) STR_VALUE(s)
            " version " STR_EXPAND(GIT_VERSION)
#ifdef GIT_BRANCH
            " branch " STR_EXPAND(GIT_BRANCH)
#endif
#ifdef GIT_TIMESTAMP
            " at " STR_EXPAND(GIT_TIMESTAMP)
#endif
#undef STR_VALUE
#undef STR_EXPAND
#else
            " version unknown"
#endif
            " inputs file rtl_tcp"
#ifdef RTLSDR
            " RTL-SDR"
#endif
#ifdef SOAPYSDR
            " SoapySDR"
#endif
#ifdef OPENSSL
            " with TLS"
#endif
            ;
}

/* helper */

struct mg_mgr *get_mgr(r_cfg_t *cfg)
{
    if (!cfg->mgr) {
        cfg->mgr = calloc(1, sizeof(*cfg->mgr));
        if (!cfg->mgr)
            FATAL_CALLOC("get_mgr()");
        mg_mgr_init(cfg->mgr, NULL);
    }

    return cfg->mgr;
}

void set_center_freq(r_cfg_t *cfg, uint32_t center_freq)
{
    cfg->frequencies = 1;
    cfg->frequency_index = 0;
    cfg->frequency[0] = center_freq;
    // cfg->center_frequency = center_freq; // actually applied in the sdr event
    sdr_set_center_freq(cfg->dev, center_freq, 1);
}

void set_freq_correction(r_cfg_t *cfg, int freq_correction)
{
    // cfg->ppm_error = freq_correction; // actually applied in the sdr event
    sdr_set_freq_correction(cfg->dev, freq_correction, 0);
}

void set_sample_rate(r_cfg_t *cfg, uint32_t sample_rate)
{
    // cfg->samp_rate = sample_rate; // actually applied in the sdr event
    sdr_set_sample_rate(cfg->dev, sample_rate, 0);
}

void set_gain_str(struct r_cfg *cfg, char const *gain_str)
{
    free(cfg->gain_str);
    if (!gain_str) {
        cfg->gain_str = NULL; // auto gain
    }
    else {
        cfg->gain_str = strdup(gain_str);
        if (!cfg->gain_str)
            WARN_STRDUP("set_gain_str()");
    }
    sdr_set_tuner_gain(cfg->dev, gain_str, 0);
}

/* general */

void r_init_cfg(r_cfg_t *cfg)
{
    cfg->out_block_size  = DEFAULT_BUF_LENGTH;
    cfg->samp_rate       = DEFAULT_SAMPLE_RATE;
    cfg->conversion_mode = CONVERT_NATIVE;
    cfg->fsk_pulse_detect_mode = FSK_PULSE_DETECT_AUTO;
    // Default log level is to show all LOG_FATAL, LOG_ERROR, LOG_WARNING
    // abnormal messages and LOG_CRITICAL information.
    cfg->verbosity = LOG_WARNING;

    list_ensure_size(&cfg->in_files, 100);
    list_ensure_size(&cfg->output_handler, 16);

    // collect devices list, this should be a module
    r_device r_devices[] = {
#define DECL(name) name,
            DEVICES
#undef DECL
    };

    cfg->num_r_devices = sizeof(r_devices) / sizeof(*r_devices);
    for (unsigned i = 0; i < cfg->num_r_devices; i++) {
        r_devices[i].protocol_num = i + 1;
    }
    cfg->devices = malloc(sizeof(r_devices));
    if (!cfg->devices)
        FATAL_CALLOC("r_init_cfg()");

    memcpy(cfg->devices, r_devices, sizeof(r_devices));

    cfg->demod = calloc(1, sizeof(*cfg->demod));
    if (!cfg->demod)
        FATAL_CALLOC("r_init_cfg()");

    cfg->demod->level_limit = 0.0f;
    cfg->demod->min_level = -12.1442f;
    cfg->demod->min_snr = 9.0f;
    // Pulse detect will only print LOG_NOTICE and lower.
    cfg->demod->detect_verbosity = LOG_WARNING;

    // note: this should be optional
    cfg->demod->pulse_detect = pulse_detect_create();
    // initialize tables
    baseband_init();

    time(&cfg->running_since);
    time(&cfg->frames_since);
    get_time_now(&cfg->demod->now);

    list_ensure_size(&cfg->demod->r_devs, 100);
    list_ensure_size(&cfg->demod->dumper, 32);
}

r_cfg_t *r_create_cfg(void)
{
    r_cfg_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg)
        FATAL_CALLOC("r_create_cfg()");

    r_init_cfg(cfg);

    return cfg;
}

void r_free_cfg(r_cfg_t *cfg)
{
    if (cfg->dev) {
        sdr_deactivate(cfg->dev);
        sdr_close(cfg->dev);
        cfg->dev = NULL;
    }

    free(cfg->gain_str);
    cfg->gain_str = NULL;

    for (void **iter = cfg->demod->dumper.elems; iter && *iter; ++iter) {
        file_info_t const *dumper = *iter;
        if (dumper->file && (dumper->file != stdout))
            fclose(dumper->file);
    }
    list_free_elems(&cfg->demod->dumper, free);

    list_free_elems(&cfg->demod->r_devs, (list_elem_free_fn)free_protocol);

    if (cfg->demod->am_analyze)
        am_analyze_free(cfg->demod->am_analyze);
    cfg->demod->am_analyze = NULL;

    pulse_detect_free(cfg->demod->pulse_detect);
    cfg->demod->pulse_detect = NULL;

    list_free_elems(&cfg->raw_handler, (list_elem_free_fn)raw_output_free);

    r_logger_set_log_handler(NULL, NULL);

    list_free_elems(&cfg->output_handler, (list_elem_free_fn)data_output_free);

    list_free_elems(&cfg->data_tags, (list_elem_free_fn)data_tag_free);

    list_free_elems(&cfg->in_files, NULL);

    free(cfg->demod);
    cfg->demod = NULL;

    free(cfg->devices);
    cfg->devices = NULL;

    mg_mgr_free(cfg->mgr);
    free(cfg->mgr);
    cfg->mgr = NULL;

    //free(cfg);
}

/* device decoder protocols */

void register_protocol(r_cfg_t *cfg, r_device *r_dev, char *arg)
{
    // use arg of 'v', 'vv', 'vvv' as device verbosity
    int dev_verbose = 0;
    if (arg && *arg == 'v') {
        for (; *arg == 'v'; ++arg) {
            dev_verbose++;
        }
        if (*arg) {
            arg++; // skip separator
        }
    }

    // use any other arg as device parameter
    r_device *p;
    if (r_dev->create_fn) {
        p = r_dev->create_fn(arg);
    }
    else {
        if (arg && *arg) {
            fprintf(stderr, "Protocol [%u] \"%s\" does not take arguments \"%s\"!\n", r_dev->protocol_num, r_dev->name, arg);
        }
        p  = malloc(sizeof(*p));
        if (!p)
            FATAL_CALLOC("register_protocol()");
        *p = *r_dev; // copy
    }

    p->verbose      = dev_verbose ? dev_verbose : (cfg->verbosity > 4 ? cfg->verbosity - 5 : 0);
    p->verbose_bits = cfg->verbose_bits;
    p->log_fn       = log_device_handler;

    p->output_fn  = data_acquired_handler;
    p->output_ctx = cfg;

    list_push(&cfg->demod->r_devs, p);

    if (cfg->verbosity >= LOG_INFO) {
        fprintf(stderr, "Registering protocol [%u] \"%s\"\n", r_dev->protocol_num, r_dev->name);
    }
}

void free_protocol(r_device *r_dev)
{
    // free(r_dev->name);
    free(r_dev->decode_ctx);
    free(r_dev);
}

void unregister_protocol(r_cfg_t *cfg, r_device *r_dev)
{
    for (size_t i = 0; i < cfg->demod->r_devs.len; ++i) { // list might contain NULLs
        r_device *p = cfg->demod->r_devs.elems[i];
        if (!strcmp(p->name, r_dev->name)) {
            list_remove(&cfg->demod->r_devs, i, (list_elem_free_fn)free_protocol);
            i--; // so we don't skip the next elem now shifted down
        }
    }
}

void register_all_protocols(r_cfg_t *cfg, unsigned disabled)
{
    for (int i = 0; i < cfg->num_r_devices; i++) {
        // register all device protocols that are not disabled
        if (cfg->devices[i].disabled <= disabled) {
            register_protocol(cfg, &cfg->devices[i], NULL);
        }
    }
}

/* output helper */

void calc_rssi_snr(r_cfg_t *cfg, pulse_data_t *pulse_data)
{
    float ook_high_estimate = pulse_data->ook_high_estimate > 0 ? pulse_data->ook_high_estimate : 1;
    float ook_low_estimate = pulse_data->ook_low_estimate > 0 ? pulse_data->ook_low_estimate : 1;
    int const OOK_MAX_HIGH_LEVEL = DB_TO_AMP(0); // Maximum estimate for high level (-0 dB)
    float ook_max_estimate = ook_high_estimate < OOK_MAX_HIGH_LEVEL ? ook_high_estimate : OOK_MAX_HIGH_LEVEL;
    float asnr   = ook_max_estimate / ook_low_estimate;
    float foffs1 = (float)pulse_data->fsk_f1_est / INT16_MAX * cfg->samp_rate / 2.0f;
    float foffs2 = (float)pulse_data->fsk_f2_est / INT16_MAX * cfg->samp_rate / 2.0f;
    pulse_data->freq1_hz = (foffs1 + cfg->center_frequency);
    pulse_data->freq2_hz = (foffs2 + cfg->center_frequency);
    pulse_data->centerfreq_hz = cfg->center_frequency;
    pulse_data->depth_bits    = cfg->demod->sample_size * 4;
    // NOTE: for (CU8) amplitude is 10x (because it's squares)
    if (cfg->demod->sample_size == 2 && !cfg->demod->use_mag_est) { // amplitude (CU8)
        pulse_data->range_db = 42.1442f; // 10*log10f(16384.0f) == 20*log10f(128.0f)
        pulse_data->rssi_db  = 10.0f * log10f(ook_high_estimate) - 42.1442f; // 10*log10f(16384.0f)
        pulse_data->noise_db = 10.0f * log10f(ook_low_estimate) - 42.1442f; // 10*log10f(16384.0f)
        pulse_data->snr_db   = 10.0f * log10f(asnr);
    }
    else { // magnitude (CU8, CS16)
        pulse_data->range_db = 84.2884f; // 20*log10f(16384.0f)
        // lowest (scaled x128) reading at  8 bit is -20*log10(128) = -42.1442 (eff. -36 dB)
        // lowest (scaled div2) reading at 12 bit is -20*log10(1024) = -60.2060 (eff. -54 dB)
        // lowest (scaled div2) reading at 16 bit is -20*log10(16384) = -84.2884 (eff. -78 dB)
        pulse_data->rssi_db  = 20.0f * log10f(ook_high_estimate) - 84.2884f; // 20*log10f(16384.0f)
        pulse_data->noise_db = 20.0f * log10f(ook_low_estimate) - 84.2884f; // 20*log10f(16384.0f)
        pulse_data->snr_db   = 20.0f * log10f(asnr);
    }
}

char *time_pos_str(r_cfg_t *cfg, unsigned samples_ago, char *buf)
{
    if (cfg->report_time == REPORT_TIME_SAMPLES) {
        double s_per_sample = 1.0f / cfg->samp_rate;
        return sample_pos_str(cfg->demod->sample_file_pos - samples_ago * s_per_sample, buf);
    }
    else {
        struct timeval ago = cfg->demod->now;
        double us_per_sample = 1e6 / cfg->samp_rate;
        unsigned usecs_ago   = samples_ago * us_per_sample;
        while (ago.tv_usec < (int)usecs_ago) {
            ago.tv_sec -= 1;
            ago.tv_usec += 1000000;
        }
        ago.tv_usec -= usecs_ago;

        char const *format = NULL;
        if (cfg->report_time == REPORT_TIME_UNIX)
            format = "%s";
        else if (cfg->report_time == REPORT_TIME_ISO)
            format = "%Y-%m-%dT%H:%M:%S";

        if (cfg->report_time_hires)
            return usecs_time_str(buf, format, cfg->report_time_tz, &ago);
        else
            return format_time_str(buf, format, cfg->report_time_tz, ago.tv_sec);
    }
}

// well-known fields "time", "msg" and "codes" are used to output general decoder messages
// well-known field "bits" is only used when verbose bits (-M bits) is requested
// well-known field "tag" is only used when output tagging is requested
// well-known field "protocol" is only used when model protocol is requested
// well-known field "description" is only used when model description is requested
// well-known fields "mod", "freq", "freq1", "freq2", "rssi", "snr", "noise" are used by meta report option
char const **well_known_output_fields(r_cfg_t *cfg)
{
    list_t field_list = {0};
    list_ensure_size(&field_list, 15);

    list_push(&field_list, "time");
    list_push(&field_list, "msg");
    list_push(&field_list, "codes");

    if (cfg->verbose_bits)
        list_push(&field_list, "bits");

    for (void **iter = cfg->data_tags.elems; iter && *iter; ++iter) {
        data_tag_t *tag = *iter;
        if (tag->key) {
            list_push(&field_list, (void *)tag->key);
        }
        else {
            list_push_all(&field_list, (void **)tag->includes);
        }
    }

    if (cfg->report_protocol)
        list_push(&field_list, "protocol");
    if (cfg->report_description)
        list_push(&field_list, "description");
    if (cfg->report_meta) {
        list_push(&field_list, "mod");
        list_push(&field_list, "freq");
        list_push(&field_list, "freq1");
        list_push(&field_list, "freq2");
        list_push(&field_list, "rssi");
        list_push(&field_list, "snr");
        list_push(&field_list, "noise");
    }

    return (char const **)field_list.elems;
}

/** Convert CSV keys according to selected conversion mode. Replacement is static but in-place. */
static char const **convert_csv_fields(r_cfg_t *cfg, char const **fields)
{
    if (cfg->conversion_mode == CONVERT_SI) {
        for (char const **p = fields; *p; ++p) {
            if (!strcmp(*p, "temperature_F")) *p = "temperature_C";
            else if (!strcmp(*p, "pressure_PSI")) *p = "pressure_kPa";
            else if (!strcmp(*p, "rain_in")) *p = "rain_mm";
            else if (!strcmp(*p, "rain_rate_in_h")) *p = "rain_rate_mm_h";
            else if (!strcmp(*p, "wind_avg_mi_h")) *p = "wind_avg_km_h";
            else if (!strcmp(*p, "wind_max_mi_h")) *p = "wind_max_km_h";
        }
    }

    if (cfg->conversion_mode == CONVERT_CUSTOMARY) {
        for (char const **p = fields; *p; ++p) {
            if (!strcmp(*p, "temperature_C")) *p = "temperature_F";
            else if (!strcmp(*p, "temperature_1_C")) *p = "temperature_1_F";
            else if (!strcmp(*p, "temperature_2_C")) *p = "temperature_2_F";
            else if (!strcmp(*p, "setpoint_C")) *p = "setpoint_F";
            else if (!strcmp(*p, "pressure_hPa")) *p = "pressure_inHg";
            else if (!strcmp(*p, "pressure_kPa")) *p = "pressure_PSI";
            else if (!strcmp(*p, "rain_mm")) *p = "rain_in";
            else if (!strcmp(*p, "rain_rate_mm_h")) *p = "rain_rate_in_h";
            else if (!strcmp(*p, "wind_avg_km_h")) *p = "wind_avg_mi_h";
            else if (!strcmp(*p, "wind_max_km_h")) *p = "wind_max_mi_h";
        }
    }
    return fields;
}

// find the fields output for CSV
char const **determine_csv_fields(r_cfg_t *cfg, char const *const *well_known, int *num_fields)
{
    list_t field_list = {0};
    list_ensure_size(&field_list, 100);

    // always add well-known fields
    list_push_all(&field_list, (void **)well_known);

    list_t *r_devs = &cfg->demod->r_devs;
    for (void **iter = r_devs->elems; iter && *iter; ++iter) {
        r_device *r_dev = *iter;
        if (r_dev->fields)
            list_push_all(&field_list, (void **)r_dev->fields);
        else
            fprintf(stderr, "rtl_433: warning: %u \"%s\" does not support CSV output\n",
                    r_dev->protocol_num, r_dev->name);
    }
    convert_csv_fields(cfg, (char const **)field_list.elems);

    if (num_fields)
        *num_fields = field_list.len;
    return (char const **)field_list.elems;
}

int run_ook_demods(list_t *r_devs, pulse_data_t *pulse_data)
{
    int p_events = 0;

    unsigned next_priority = 0; // next smallest on each loop through decoders
    // run all decoders of each priority, stop if an event is produced
    for (unsigned priority = 0; !p_events && priority < UINT_MAX; priority = next_priority) {
        next_priority = UINT_MAX;
        for (void **iter = r_devs->elems; iter && *iter; ++iter) {
            r_device *r_dev = *iter;

            // Find next smallest priority
            if (r_dev->priority > priority && r_dev->priority < next_priority)
                next_priority = r_dev->priority;
            // Run only current priority
            if (r_dev->priority != priority)
                continue;

            switch (r_dev->modulation) {
            case OOK_PULSE_PCM:
            // case OOK_PULSE_RZ:
                p_events += pulse_slicer_pcm(pulse_data, r_dev);
                break;
            case OOK_PULSE_PPM:
                p_events += pulse_slicer_ppm(pulse_data, r_dev);
                break;
            case OOK_PULSE_PWM:
                p_events += pulse_slicer_pwm(pulse_data, r_dev);
                break;
            case OOK_PULSE_MANCHESTER_ZEROBIT:
                p_events += pulse_slicer_manchester_zerobit(pulse_data, r_dev);
                break;
            case OOK_PULSE_PIWM_RAW:
                p_events += pulse_slicer_piwm_raw(pulse_data, r_dev);
                break;
            case OOK_PULSE_PIWM_DC:
                p_events += pulse_slicer_piwm_dc(pulse_data, r_dev);
                break;
            case OOK_PULSE_DMC:
                p_events += pulse_slicer_dmc(pulse_data, r_dev);
                break;
            case OOK_PULSE_PWM_OSV1:
                p_events += pulse_slicer_osv1(pulse_data, r_dev);
                break;
            case OOK_PULSE_NRZS:
                p_events += pulse_slicer_nrzs(pulse_data, r_dev);
                break;
            // FSK decoders
            case FSK_PULSE_PCM:
            case FSK_PULSE_PWM:
            case FSK_PULSE_MANCHESTER_ZEROBIT:
                break;
            default:
                fprintf(stderr, "Unknown modulation %u in protocol!\n", r_dev->modulation);
            }
        }
    }

    return p_events;
}

int run_fsk_demods(list_t *r_devs, pulse_data_t *fsk_pulse_data)
{
    int p_events = 0;

    unsigned next_priority = 0; // next smallest on each loop through decoders
    // run all decoders of each priority, stop if an event is produced
    for (unsigned priority = 0; !p_events && priority < UINT_MAX; priority = next_priority) {
        next_priority = UINT_MAX;
        for (void **iter = r_devs->elems; iter && *iter; ++iter) {
            r_device *r_dev = *iter;

            // Find next smallest priority
            if (r_dev->priority > priority && r_dev->priority < next_priority)
                next_priority = r_dev->priority;
            // Run only current priority
            if (r_dev->priority != priority)
                continue;

            switch (r_dev->modulation) {
            // OOK decoders
            case OOK_PULSE_PCM:
            // case OOK_PULSE_RZ:
            case OOK_PULSE_PPM:
            case OOK_PULSE_PWM:
            case OOK_PULSE_MANCHESTER_ZEROBIT:
            case OOK_PULSE_PIWM_RAW:
            case OOK_PULSE_PIWM_DC:
            case OOK_PULSE_DMC:
            case OOK_PULSE_PWM_OSV1:
            case OOK_PULSE_NRZS:
                break;
            case FSK_PULSE_PCM:
                p_events += pulse_slicer_pcm(fsk_pulse_data, r_dev);
                break;
            case FSK_PULSE_PWM:
                p_events += pulse_slicer_pwm(fsk_pulse_data, r_dev);
                break;
            case FSK_PULSE_MANCHESTER_ZEROBIT:
                p_events += pulse_slicer_manchester_zerobit(fsk_pulse_data, r_dev);
                break;
            default:
                fprintf(stderr, "Unknown modulation %u in protocol!\n", r_dev->modulation);
            }
        }
    }

    return p_events;
}

/* handlers */

static void log_handler(log_level_t level, char const *src, char const *msg, void *userdata)
{
    r_cfg_t *cfg = userdata;

    if (cfg->verbosity < (int)level) {
        return;
    }
    /* clang-format off */
    data_t *data = data_make(
            "src",     "",     DATA_STRING, src,
            "lvl",      "",     DATA_INT,    level,
            "msg",      "",     DATA_STRING, msg,
            NULL);
    /* clang-format on */

    // prepend "time" if requested
    if (cfg->report_time != REPORT_TIME_OFF) {
        char time_str[LOCAL_TIME_BUFLEN];
        time_pos_str(cfg, 0, time_str);
        data = data_prepend(data,
                data_str(NULL, "time", "", NULL, time_str));
    }

    for (size_t i = 0; i < cfg->output_handler.len; ++i) { // list might contain NULLs
        data_output_t *output = cfg->output_handler.elems[i];
        if (output && output->log_level >= (int)level) {
            data_output_print(output, data);
        }
    }
    data_free(data);
}

void r_redirect_logging(r_cfg_t *cfg)
{
    r_logger_set_log_handler(log_handler, cfg);
}

/** Pass the data structure to all output handlers. Frees data afterwards. */
void event_occurred_handler(r_cfg_t *cfg, data_t *data)
{
    // prepend "time" if requested
    if (cfg->report_time != REPORT_TIME_OFF) {
        char time_str[LOCAL_TIME_BUFLEN];
        time_pos_str(cfg, 0, time_str);
        data = data_prepend(data,
                data_str(NULL, "time", "", NULL, time_str));
    }

    for (size_t i = 0; i < cfg->output_handler.len; ++i) { // list might contain NULLs
        data_output_t *output = cfg->output_handler.elems[i];
        data_output_print(output, data);
    }
    data_free(data);
}

/** Pass the data structure to all output handlers. Frees data afterwards. */
void log_device_handler(r_device *r_dev, int level, data_t *data)
{
    r_cfg_t *cfg = r_dev->output_ctx;

    // prepend "time" if requested
    if (cfg->report_time != REPORT_TIME_OFF) {
        char time_str[LOCAL_TIME_BUFLEN];
        time_pos_str(cfg, cfg->demod->pulse_data.start_ago, time_str);
        data = data_prepend(data,
                data_str(NULL, "time", "", NULL, time_str));
    }

    for (size_t i = 0; i < cfg->output_handler.len; ++i) { // list might contain NULLs
        data_output_t *output = cfg->output_handler.elems[i];
        if (output && output->log_level >= level) {
            data_output_print(output, data);
        }
    }
    data_free(data);
}

/** Pass the data structure to all output handlers. Frees data afterwards. */
void data_acquired_handler(r_device *r_dev, data_t *data)
{
    r_cfg_t *cfg = r_dev->output_ctx;

#ifndef NDEBUG
    // check for undeclared csv fields
    for (data_t *d = data; d; d = d->next) {
        int found = 0;
        for (char const *const *p = r_dev->fields; *p; ++p) {
            if (!strcmp(d->key, *p)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "WARNING: Undeclared field \"%s\" in [%u] \"%s\"\n", d->key, r_dev->protocol_num, r_dev->name);
        }
    }
#endif

    if (cfg->conversion_mode == CONVERT_SI) {
        for (data_t *d = data; d; d = d->next) {
            // Convert double type fields ending in _F to _C
            if ((d->type == DATA_DOUBLE) && str_endswith(d->key, "_F")) {
                d->value.v_dbl = fahrenheit2celsius(d->value.v_dbl);
                char *new_label = str_replace(d->key, "_F", "_C");
                free(d->key);
                d->key = new_label;
                char *pos;
                if (d->format && (pos = strrchr(d->format, 'F'))) {
                    *pos = 'C';
                }
            }
            // Convert double type fields ending in _mi_h to _km_h
            else if ((d->type == DATA_DOUBLE) && str_endswith(d->key, "_mi_h")) {
                d->value.v_dbl = mph2kmph(d->value.v_dbl);
                char *new_label = str_replace(d->key, "_mi_h", "_km_h");
                free(d->key);
                d->key = new_label;
                char *new_format_label = str_replace(d->format, "mi/h", "km/h");
                free(d->format);
                d->format = new_format_label;
            }
            // Convert double type fields ending in _in to _mm
            else if ((d->type == DATA_DOUBLE) && str_endswith(d->key, "_in")) {
                d->value.v_dbl = inch2mm(d->value.v_dbl);
                char *new_label = str_replace(d->key, "_in", "_mm");
                free(d->key);
                d->key = new_label;
                char *new_format_label = str_replace(d->format, "in", "mm");
                free(d->format);
                d->format = new_format_label;
            }
            // Convert double type fields ending in _in_h to _mm_h
            else if ((d->type == DATA_DOUBLE) && str_endswith(d->key, "_in_h")) {
                d->value.v_dbl = inch2mm(d->value.v_dbl);
                char *new_label = str_replace(d->key, "_in_h", "_mm_h");
                free(d->key);
                d->key = new_label;
                char *new_format_label = str_replace(d->format, "in/h", "mm/h");
                free(d->format);
                d->format = new_format_label;
            }
            // Convert double type fields ending in _inHg to _hPa
            else if ((d->type == DATA_DOUBLE) && str_endswith(d->key, "_inHg")) {
                d->value.v_dbl = inhg2hpa(d->value.v_dbl);
                char *new_label = str_replace(d->key, "_inHg", "_hPa");
                free(d->key);
                d->key = new_label;
                char *new_format_label = str_replace(d->format, "inHg", "hPa");
                free(d->format);
                d->format = new_format_label;
            }
            // Convert double type fields ending in _PSI to _kPa
            else if ((d->type == DATA_DOUBLE) && str_endswith(d->key, "_PSI")) {
                d->value.v_dbl = psi2kpa(d->value.v_dbl);
                char *new_label = str_replace(d->key, "_PSI", "_kPa");
                free(d->key);
                d->key = new_label;
                char *new_format_label = str_replace(d->format, "PSI", "kPa");
                free(d->format);
                d->format = new_format_label;
            }
        }
    }
    if (cfg->conversion_mode == CONVERT_CUSTOMARY) {
        for (data_t *d = data; d; d = d->next) {
            // Convert double type fields ending in _C to _F
            if ((d->type == DATA_DOUBLE) && str_endswith(d->key, "_C")) {
                d->value.v_dbl = celsius2fahrenheit(d->value.v_dbl);
                char *new_label = str_replace(d->key, "_C", "_F");
                free(d->key);
                d->key = new_label;
                char *pos;
                if (d->format && (pos = strrchr(d->format, 'C'))) {
                    *pos = 'F';
                }
            }
            // Convert double type fields ending in _km_h to _mi_h
            else if ((d->type == DATA_DOUBLE) && str_endswith(d->key, "_km_h")) {
                d->value.v_dbl = kmph2mph(d->value.v_dbl);
                char *new_label = str_replace(d->key, "_km_h", "_mi_h");
                free(d->key);
                d->key = new_label;
                char *new_format_label = str_replace(d->format, "km/h", "mi/h");
                free(d->format);
                d->format = new_format_label;
            }
            // Convert double type fields ending in _mm to _in
            else if ((d->type == DATA_DOUBLE) && str_endswith(d->key, "_mm")) {
                d->value.v_dbl = mm2inch(d->value.v_dbl);
                char *new_label = str_replace(d->key, "_mm", "_in");
                free(d->key);
                d->key = new_label;
                char *new_format_label = str_replace(d->format, "mm", "in");
                free(d->format);
                d->format = new_format_label;
            }
            // Convert double type fields ending in _mm_h to _in_h
            else if ((d->type == DATA_DOUBLE) && str_endswith(d->key, "_mm_h")) {
                d->value.v_dbl = mm2inch(d->value.v_dbl);
                char *new_label = str_replace(d->key, "_mm_h", "_in_h");
                free(d->key);
                d->key = new_label;
                char *new_format_label = str_replace(d->format, "mm/h", "in/h");
                free(d->format);
                d->format = new_format_label;
            }
            // Convert double type fields ending in _hPa to _inHg
            else if ((d->type == DATA_DOUBLE) && str_endswith(d->key, "_hPa")) {
                d->value.v_dbl = hpa2inhg(d->value.v_dbl);
                char *new_label = str_replace(d->key, "_hPa", "_inHg");
                free(d->key);
                d->key = new_label;
                char *new_format_label = str_replace(d->format, "hPa", "inHg");
                free(d->format);
                d->format = new_format_label;
            }
            // Convert double type fields ending in _kPa to _PSI
            else if ((d->type == DATA_DOUBLE) && str_endswith(d->key, "_kPa")) {
                d->value.v_dbl = kpa2psi(d->value.v_dbl);
                char *new_label = str_replace(d->key, "_kPa", "_PSI");
                free(d->key);
                d->key = new_label;
                char *new_format_label = str_replace(d->format, "kPa", "PSI");
                free(d->format);
                d->format = new_format_label;
            }
        }
    }

    // prepend "description" if requested
    if (cfg->report_description) {
        data = data_prepend(data,
                data_str(NULL, "description", "Description", NULL, r_dev->name));
    }

    // prepend "protocol" if requested
    if (cfg->report_protocol && r_dev->protocol_num) {
        data = data_prepend(data,
                data_int(NULL, "protocol", "Protocol", NULL, r_dev->protocol_num));
    }

    if (cfg->report_meta && cfg->demod->fsk_pulse_data.fsk_f2_est) {
        data = data_str(data, "mod",   "Modulation",  NULL,         "FSK");
        data = data_dbl(data, "freq1", "Freq1",       "%.1f MHz",   cfg->demod->fsk_pulse_data.freq1_hz / 1000000.0);
        data = data_dbl(data, "freq2", "Freq2",       "%.1f MHz",   cfg->demod->fsk_pulse_data.freq2_hz / 1000000.0);
        data = data_dbl(data, "rssi",  "RSSI",        "%.1f dB",    cfg->demod->fsk_pulse_data.rssi_db);
        data = data_dbl(data, "snr",   "SNR",         "%.1f dB",    cfg->demod->fsk_pulse_data.snr_db);
        data = data_dbl(data, "noise", "Noise",       "%.1f dB",    cfg->demod->fsk_pulse_data.noise_db);
    }
    else if (cfg->report_meta) {
        data = data_str(data, "mod",   "Modulation",  NULL,         "ASK");
        data = data_dbl(data, "freq",  "Freq",        "%.1f MHz",   cfg->demod->pulse_data.freq1_hz / 1000000.0);
        data = data_dbl(data, "rssi",  "RSSI",        "%.1f dB",    cfg->demod->pulse_data.rssi_db);
        data = data_dbl(data, "snr",   "SNR",         "%.1f dB",    cfg->demod->pulse_data.snr_db);
        data = data_dbl(data, "noise", "Noise",       "%.1f dB",    cfg->demod->pulse_data.noise_db);
    }

    // prepend "time" if requested
    if (cfg->report_time != REPORT_TIME_OFF) {
        char time_str[LOCAL_TIME_BUFLEN];
        time_pos_str(cfg, cfg->demod->pulse_data.start_ago, time_str);
        data = data_prepend(data,
                data_str(NULL, "time", "", NULL, time_str));
    }

    // apply all tags
    for (void **iter = cfg->data_tags.elems; iter && *iter; ++iter) {
        data_tag_t *tag = *iter;
        data            = data_tag_apply(tag, data, cfg->in_filename);
    }

    for (size_t i = 0; i < cfg->output_handler.len; ++i) { // list might contain NULLs
        data_output_t *output = cfg->output_handler.elems[i];
        data_output_print(output, data);
    }
    data_free(data);
}

// level 0: do not report (don't call this), 1: report successful devices, 2: report active devices, 3: report all
data_t *create_report_data(r_cfg_t *cfg, int level)
{
    list_t *r_devs = &cfg->demod->r_devs;
    data_t *data;
    list_t dev_data_list = {0};
    list_ensure_size(&dev_data_list, r_devs->len);

    for (void **iter = r_devs->elems; iter && *iter; ++iter) {
        r_device *r_dev = *iter;
        if (level <= 2 && r_dev->decode_events == 0)
            continue;
        if (level <= 1 && r_dev->decode_ok == 0)
            continue;
        if (level <= 0)
            continue;

        data = data_make(
                "device",       "", DATA_INT, r_dev->protocol_num,
                "name",         "", DATA_STRING, r_dev->name,
                "events",       "", DATA_INT, r_dev->decode_events,
                "ok",           "", DATA_INT, r_dev->decode_ok,
                "messages",     "", DATA_INT, r_dev->decode_messages,
                NULL);

        if (r_dev->decode_fails[-DECODE_FAIL_OTHER])
            data = data_int(data, "fail_other",   "", NULL, r_dev->decode_fails[-DECODE_FAIL_OTHER]);
        if (r_dev->decode_fails[-DECODE_ABORT_LENGTH])
            data = data_int(data, "abort_length", "", NULL, r_dev->decode_fails[-DECODE_ABORT_LENGTH]);
        if (r_dev->decode_fails[-DECODE_ABORT_EARLY])
            data = data_int(data, "abort_early",  "", NULL, r_dev->decode_fails[-DECODE_ABORT_EARLY]);
        if (r_dev->decode_fails[-DECODE_FAIL_MIC])
            data = data_int(data, "fail_mic",     "", NULL, r_dev->decode_fails[-DECODE_FAIL_MIC]);
        if (r_dev->decode_fails[-DECODE_FAIL_SANITY])
            data = data_int(data, "fail_sanity",  "", NULL, r_dev->decode_fails[-DECODE_FAIL_SANITY]);

        list_push(&dev_data_list, data);
    }

    data = data_make(
            "count",            "", DATA_INT, cfg->frames_ook,
            "fsk",              "", DATA_INT, cfg->frames_fsk,
            "events",           "", DATA_INT, cfg->frames_events,
            NULL);

    char since_str[LOCAL_TIME_BUFLEN];
    format_time_str(since_str, "%Y-%m-%dT%H:%M:%S", cfg->report_time_tz, cfg->frames_since);

    data = data_make(
            "enabled",          "", DATA_INT, r_devs->len,
            "since",            "", DATA_STRING, since_str,
            "frames",           "", DATA_DATA, data,
            "stats",            "", DATA_ARRAY, data_array(dev_data_list.len, DATA_DATA, dev_data_list.elems),
            NULL);

    list_free_elems(&dev_data_list, NULL);
    return data;
}

void flush_report_data(r_cfg_t *cfg)
{
    list_t *r_devs = &cfg->demod->r_devs;

    time(&cfg->frames_since);
    cfg->frames_ook = 0;
    cfg->frames_fsk = 0;
    cfg->frames_events = 0;

    for (void **iter = r_devs->elems; iter && *iter; ++iter) {
        r_device *r_dev = *iter;

        r_dev->decode_events = 0;
        r_dev->decode_ok = 0;
        r_dev->decode_messages = 0;
        r_dev->decode_fails[0] = 0;
        r_dev->decode_fails[1] = 0;
        r_dev->decode_fails[2] = 0;
        r_dev->decode_fails[3] = 0;
        r_dev->decode_fails[4] = 0;
    }
}

/* setup */

static int lvlarg_param(char **param, int default_verb)
{
    if (!param || !*param) {
        return default_verb;
    }
    // parse ", v = %d"
    char *p = *param;
    if (*p != ',') {
        return default_verb;
    }
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != 'v') {
        fprintf(stderr, "Unknown output option \"%s\"\n", *param);
        exit(1);
    }
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '=') {
        fprintf(stderr, "Unknown output option \"%s\"\n", *param);
        exit(1);
    }
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    char *endptr;
    int val = strtol(p, &endptr, 10);
    if (p == endptr) {
        fprintf(stderr, "Invalid output option \"%s\"\n", *param);
        exit(1);
    }
    *param = endptr;
    return val;
}

/// Opens the path @p param (or STDOUT if empty or `-`) for append writing, removes leading `,` and `:` from path name.
static FILE *fopen_output(char const *param)
{
    if (!param || !*param) {
        return stdout; // No path given
    }
    while (*param == ',') {
        param++; // Skip all leading `,`
    }
    if (*param == ':') {
        param++; // Skip one leading `:`
    }
    if (*param == '-' && param[1] == '\0') {
        return stdout; // STDOUT requested
    }
    FILE *file = fopen(param, "a");
    if (!file) {
        fprintf(stderr, "rtl_433: failed to open output file\n");
        exit(1);
    }
    return file;
}

void add_json_output(r_cfg_t *cfg, char *param)
{
    int log_level = lvlarg_param(&param, 0);
    list_push(&cfg->output_handler, data_output_json_create(log_level, fopen_output(param)));
}

void add_csv_output(r_cfg_t *cfg, char *param)
{
    int log_level = lvlarg_param(&param, 0);
    list_push(&cfg->output_handler, data_output_csv_create(log_level, fopen_output(param)));
}

void start_outputs(r_cfg_t *cfg, char const *const *well_known)
{
    int num_output_fields;
    char const **output_fields = determine_csv_fields(cfg, well_known, &num_output_fields);

    for (size_t i = 0; i < cfg->output_handler.len; ++i) { // list might contain NULLs
        data_output_t *output = cfg->output_handler.elems[i];
        data_output_start(output, output_fields, num_output_fields);
    }

    free((void *)output_fields);
}

void add_log_output(r_cfg_t *cfg, char *param)
{
    int log_level = lvlarg_param(&param, LOG_TRACE);
    list_push(&cfg->output_handler, data_output_log_create(log_level, fopen_output(param)));
}

void add_kv_output(r_cfg_t *cfg, char *param)
{
    int log_level = lvlarg_param(&param, LOG_TRACE);
    list_push(&cfg->output_handler, data_output_kv_create(log_level, fopen_output(param)));
}

void add_mqtt_output(r_cfg_t *cfg, char *param)
{
    list_push(&cfg->output_handler, data_output_mqtt_create(get_mgr(cfg), param, cfg->dev_query));
}

void add_influx_output(r_cfg_t *cfg, char *param)
{
    list_push(&cfg->output_handler, data_output_influx_create(get_mgr(cfg), param));
}

void add_syslog_output(r_cfg_t *cfg, char *param)
{
    int log_level = lvlarg_param(&param, LOG_WARNING);
    char const *host = "localhost";
    char const *port = "514";
    char const *extra = hostport_param(param, &host, &port);
    if (extra && *extra) {
        print_logf(LOG_FATAL, "Syslog UDP", "Unknown parameters \"%s\"", extra);
    }
    print_logf(LOG_CRITICAL, "Syslog UDP", "Sending datagrams to %s port %s", host, port);

    list_push(&cfg->output_handler, data_output_syslog_create(log_level, host, port));
}

void add_http_output(r_cfg_t *cfg, char *param)
{
    // Note: no log_level, the HTTP-API consumes all log levels.
    char const *host = "0.0.0.0";
    char const *port = "8433";
    char const *extra = hostport_param(param, &host, &port);
    if (extra && *extra) {
        print_logf(LOG_FATAL, "HTTP server", "Unknown parameters \"%s\"", extra);
    }
    print_logf(LOG_CRITICAL, "HTTP server", "Starting HTTP server at %s port %s", host, port);

    list_push(&cfg->output_handler, data_output_http_create(get_mgr(cfg), host, port, cfg));
}

void add_trigger_output(r_cfg_t *cfg, char *param)
{
    // Note: no log_level, we never trigger on logs.
    list_push(&cfg->output_handler, data_output_trigger_create(fopen_output(param)));
}

void add_null_output(r_cfg_t *cfg, char *param)
{
    UNUSED(param);
    list_push(&cfg->output_handler, NULL);
}

void add_rtltcp_output(r_cfg_t *cfg, char *param)
{
    char const *host = "localhost";
    char const *port = "1234";
    char const *extra = hostport_param(param, &host, &port);
    if (extra && *extra) {
        print_logf(LOG_FATAL, "rtl_tcp server", "Unknown parameters \"%s\"", extra);
    }
    print_logf(LOG_CRITICAL, "rtl_tcp server", "Starting rtl_tcp server at %s port %s", host, port);

    list_push(&cfg->raw_handler, raw_output_rtltcp_create(host, port, extra, cfg));
}

void add_sr_dumper(r_cfg_t *cfg, char const *spec, int overwrite)
{
    // create channels
    add_dumper(cfg, "U8:LOGIC:logic-1-1", overwrite);
    add_dumper(cfg, "F32:I:analog-1-4-1", overwrite);
    add_dumper(cfg, "F32:Q:analog-1-5-1", overwrite);
    add_dumper(cfg, "F32:AM:analog-1-6-1", overwrite);
    add_dumper(cfg, "F32:FM:analog-1-7-1", overwrite);
    cfg->sr_filename = spec;
    cfg->sr_execopen = overwrite;
}

void reopen_dumpers(struct r_cfg *cfg)
{
#ifndef _WIN32
    for (void **iter = cfg->demod->dumper.elems; iter && *iter; ++iter) {
        file_info_t *dumper = *iter;
        if (dumper->file && (dumper->file != stdout)) {
            // Get current file inode
            struct stat old_st = {0};
            int ret = fstat(fileno(dumper->file), &old_st);
            if (ret) {
                fprintf(stderr, "Failed to fstat %s (%d)\n", dumper->path, errno);
                exit(1);
            }

            // Get new path inode if available
            struct stat new_st = {0};
            stat(dumper->path, &new_st);
            // ok for stat() to fail, the file might not exist
            if (old_st.st_ino == new_st.st_ino) {
                continue;
            }

            // Reopen the file
            print_logf(LOG_INFO, "Dumper", "Reopening \"%s\"", dumper->path);
            fclose(dumper->file);
            dumper->file = fopen(dumper->path, "wb");
            if (!dumper->file) {
                fprintf(stderr, "Failed to open %s\n", dumper->path);
                exit(1);
            }
            if (dumper->format == VCD_LOGIC) {
                pulse_data_print_vcd_header(dumper->file, cfg->samp_rate);
            }
            if (dumper->format == PULSE_OOK) {
                pulse_data_print_pulse_header(dumper->file);
            }
        }
    }
#endif
}

void close_dumpers(struct r_cfg *cfg)
{
    for (void **iter = cfg->demod->dumper.elems; iter && *iter; ++iter) {
        file_info_t *dumper = *iter;
        if (dumper->file && (dumper->file != stdout)) {
            fclose(dumper->file);
            dumper->file = NULL;
        }
    }

    char const *labels[] = {
            "FRAME", // probe1
            "ASK", // probe2
            "FSK", // probe3
            "I", // analog4
            "Q", // analog5
            "AM", // analog6
            "FM", // analog7
    };
    if (cfg->sr_filename) {
        write_sigrok(cfg->sr_filename, cfg->samp_rate, 3, 4, labels);
    }
    if (cfg->sr_execopen) {
        open_pulseview(cfg->sr_filename);
    }
}

void add_dumper(r_cfg_t *cfg, char const *spec, int overwrite)
{
    size_t spec_len = strlen(spec);
    if (spec_len >= 3 && !strcmp(&spec[spec_len - 3], ".sr")) {
        add_sr_dumper(cfg, spec, overwrite);
        return;
    }

    file_info_t *dumper = calloc(1, sizeof(*dumper));
    if (!dumper)
        FATAL_CALLOC("add_dumper()");
    list_push(&cfg->demod->dumper, dumper);

    file_info_parse_filename(dumper, spec);
    if (strcmp(dumper->path, "-") == 0) { /* Write samples to stdout */
        dumper->file = stdout;
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
    }
    else {
        if (access(dumper->path, F_OK) == 0 && !overwrite) {
            fprintf(stderr, "Output file %s already exists, exiting\n", spec);
            exit(1);
        }
        dumper->file = fopen(dumper->path, "wb");
        if (!dumper->file) {
            fprintf(stderr, "Failed to open %s\n", spec);
            exit(1);
        }
    }
    if (dumper->format == VCD_LOGIC) {
        pulse_data_print_vcd_header(dumper->file, cfg->samp_rate);
    }
    if (dumper->format == PULSE_OOK) {
        pulse_data_print_pulse_header(dumper->file);
    }
}

void add_infile(r_cfg_t *cfg, char *in_file)
{
    list_push(&cfg->in_files, in_file);
}

void add_data_tag(struct r_cfg *cfg, char *param)
{
    list_push(&cfg->data_tags, data_tag_create(param, get_mgr(cfg)));
}

/* ---- run loop (promoted from rtl_433.c) ---- */

static volatile sig_atomic_t r_sig_hup = 0;

void r_notify_sighup(void)
{
    r_sig_hup = 1;
}

static void reset_sdr_callback(r_cfg_t *cfg)
{
    struct dm_state *demod = cfg->demod;

    get_time_now(&demod->now);

    demod->frame_start_ago   = 0;
    demod->frame_end_ago     = 0;
    demod->frame_event_count = 0;

    demod->min_level_auto = 0.0f;
    demod->noise_level    = 0.0f;

    baseband_low_pass_filter_reset(&demod->lowpass_filter_state);
    baseband_demod_FM_reset(&demod->demod_FM_state);

    pulse_detect_reset(demod->pulse_detect);
}

static void sdr_callback(unsigned char *iq_buf, uint32_t len, void *ctx)
{
    //fprintf(stderr, "sdr_callback... %u\n", len);
    r_cfg_t *cfg = ctx;
    struct dm_state *demod = cfg->demod;
    char time_str[LOCAL_TIME_BUFLEN];
    unsigned long n_samples;

    if (!demod) {
        // might happen when the demod closed and we get a last data frame
        return; // ignore the data
    }

    // do this here and not in sdr_handler so realtime replay can use rtl_tcp output
    for (void **iter = cfg->raw_handler.elems; iter && *iter; ++iter) {
        raw_output_t *output = *iter;
        raw_output_frame(output, iq_buf, len);
    }

    if ((cfg->bytes_to_read > 0) && (cfg->bytes_to_read <= len)) {
        len = cfg->bytes_to_read;
        cfg->exit_async = 1;
    }

    // save last frame time to see if a new second started
    time_t last_frame_sec = demod->now.tv_sec;
    get_time_now(&demod->now);

    n_samples = len / demod->sample_size;
    if (n_samples * demod->sample_size != len) {
        print_log(LOG_WARNING, __func__, "Sample buffer length not aligned to sample size!");
    }
    if (!n_samples) {
        print_log(LOG_WARNING, __func__, "Sample buffer too short!");
        return; // keep the watchdog timer running
    }

    // age the frame position if there is one
    if (demod->frame_start_ago)
        demod->frame_start_ago += n_samples;
    if (demod->frame_end_ago)
        demod->frame_end_ago += n_samples;

    cfg->watchdog++; // reset the frame acquire watchdog

    if (demod->samp_grab) {
        samp_grab_push(demod->samp_grab, iq_buf, len);
    }

    // AM demodulation
    float avg_db;
    if (demod->sample_size == 2) { // CU8
        if (demod->use_mag_est) {
            //magnitude_true_cu8(iq_buf, demod->buf.temp, n_samples);
            avg_db = magnitude_est_cu8(iq_buf, demod->buf.temp, n_samples);
        }
        else { // amp est
            avg_db = envelope_detect(iq_buf, demod->buf.temp, n_samples);
        }
    } else { // CS16
        //magnitude_true_cs16((int16_t *)iq_buf, demod->buf.temp, n_samples);
        avg_db = magnitude_est_cs16((int16_t *)iq_buf, demod->buf.temp, n_samples);
    }

    //fprintf(stderr, "noise level: %.1f dB current: %.1f dB min level: %.1f dB\n", demod->noise_level, avg_db, demod->min_level_auto);
    if (demod->min_level_auto == 0.0f) {
        demod->min_level_auto = demod->min_level;
    }
    if (demod->noise_level == 0.0f) {
        demod->noise_level = demod->min_level_auto - 3.0f;
    }
    int noise_only = avg_db < demod->noise_level + 3.0f; // or demod->min_level_auto?
    // always process frames if loader, dumper, or analyzers are in use, otherwise skip silent frames
    int process_frame = demod->squelch_offset <= 0 || !noise_only || demod->load_info.format || demod->analyze_pulses || demod->dumper.len || demod->samp_grab;
    cfg->total_frames_count += 1;
    if (noise_only) {
        cfg->total_frames_squelch += 1;
        demod->noise_level = (demod->noise_level * 7 + avg_db) / 8; // fast fall over 8 frames
        // If auto_level and noise level well below min_level and significant change in noise level
        if (demod->auto_level > 0 && demod->noise_level < demod->min_level - 3.0f
                && fabsf(demod->min_level_auto - demod->noise_level - 3.0f) > 1.0f) {
            demod->min_level_auto = demod->noise_level + 3.0f;
            print_logf(LOG_WARNING, "Auto Level", "Estimated noise level is %.1f dB, adjusting minimum detection level to %.1f dB",
                    demod->noise_level, demod->min_level_auto);
            pulse_detect_set_levels(demod->pulse_detect, demod->use_mag_est, demod->level_limit, demod->min_level_auto, demod->min_snr, demod->detect_verbosity);
        }
    } else {
        demod->noise_level = (demod->noise_level * 31 + avg_db) / 32; // slow rise over 32 frames
    }
    // Report noise every report_noise seconds, but only for the first frame that second
    if (cfg->report_noise && last_frame_sec != demod->now.tv_sec && demod->now.tv_sec % cfg->report_noise == 0) {
        print_logf(LOG_WARNING, "Auto Level", "Current %s level %.1f dB, estimated noise %.1f dB",
                noise_only ? "noise" : "signal", avg_db, demod->noise_level);
    }

    if (process_frame) {
        baseband_low_pass_filter(&demod->lowpass_filter_state, demod->buf.temp, demod->am_buf, n_samples);
    }

    // FM demodulation
    // Select the correct fsk pulse detector
    unsigned fpdm = cfg->fsk_pulse_detect_mode;
    if (cfg->fsk_pulse_detect_mode == FSK_PULSE_DETECT_AUTO) {
        if (cfg->frequency[cfg->frequency_index] > FSK_PULSE_DETECTOR_LIMIT)
            fpdm = FSK_PULSE_DETECT_NEW;
        else
            fpdm = FSK_PULSE_DETECT_OLD;
    }

    if (demod->enable_FM_demod && process_frame) {
        float low_pass = demod->low_pass != 0.0f ? demod->low_pass : fpdm ? 0.2f : 0.1f;
        if (demod->sample_size == 2) { // CU8
            baseband_demod_FM(&demod->demod_FM_state, iq_buf, demod->buf.fm, n_samples, cfg->samp_rate, low_pass);
        } else { // CS16
            baseband_demod_FM_cs16(&demod->demod_FM_state, (int16_t *)iq_buf, demod->buf.fm, n_samples, cfg->samp_rate, low_pass);
        }
    }

    // Handle special input formats
    if (demod->load_info.format == S16_AM) { // The IQ buffer is really AM demodulated data
        if (len > sizeof(demod->am_buf))
            FATAL("Buffer too small");
        memcpy(demod->am_buf, iq_buf, len);
    } else if (demod->load_info.format == S16_FM) { // The IQ buffer is really FM demodulated data
        // we would need AM for the envelope too
        if (len > sizeof(demod->buf.fm))
            FATAL("Buffer too small");
        memcpy(demod->buf.fm, iq_buf, len);
    }

    int d_events = 0; // Sensor events successfully detected
    if (demod->r_devs.len || demod->analyze_pulses || demod->dumper.len || demod->samp_grab) {
        // Detect a package and loop through demodulators with pulse data
        int package_type = PULSE_DATA_OOK;  // Just to get us started
        for (void **iter = demod->dumper.elems; iter && *iter; ++iter) {
            file_info_t const *dumper = *iter;
            if (dumper->format == U8_LOGIC) {
                memset(demod->u8_buf, 0, n_samples);
                break;
            }
        }
        while (package_type && process_frame) {
            int p_events = 0; // Sensor events successfully detected per package
            package_type = pulse_detect_package(demod->pulse_detect, demod->am_buf, demod->buf.fm, n_samples, cfg->samp_rate, cfg->input_pos, &demod->pulse_data, &demod->fsk_pulse_data, fpdm);
            if (package_type) {
                // new package: set a first frame start if we are not tracking one already
                if (!demod->frame_start_ago)
                    demod->frame_start_ago = demod->pulse_data.start_ago;
                // always update the last frame end
                demod->frame_end_ago = demod->pulse_data.end_ago;
            }
            if (package_type == PULSE_DATA_OOK) {
                calc_rssi_snr(cfg, &demod->pulse_data);
                if (demod->analyze_pulses) fprintf(stderr, "Detected OOK package\t%s\n", time_pos_str(cfg, demod->pulse_data.start_ago, time_str));

                p_events += run_ook_demods(&demod->r_devs, &demod->pulse_data);
                cfg->total_frames_ook += 1;
                cfg->total_frames_events += p_events > 0;
                cfg->frames_ook +=1;
                cfg->frames_events += p_events > 0;

                for (void **iter = demod->dumper.elems; iter && *iter; ++iter) {
                    file_info_t const *dumper = *iter;
                    if (dumper->format == VCD_LOGIC) pulse_data_print_vcd(dumper->file, &demod->pulse_data, '\'');
                    if (dumper->format == U8_LOGIC) pulse_data_dump_raw(demod->u8_buf, n_samples, cfg->input_pos, &demod->pulse_data, 0x02);
                    if (dumper->format == PULSE_OOK) pulse_data_dump(dumper->file, &demod->pulse_data);
                }

                if (cfg->verbosity >= LOG_TRACE) pulse_data_print(&demod->pulse_data);
                if (cfg->raw_mode == 1 || (cfg->raw_mode == 2 && p_events == 0) || (cfg->raw_mode == 3 && p_events > 0)) {
                    data_t *data = pulse_data_print_data(&demod->pulse_data);
                    event_occurred_handler(cfg, data);
                }
                if (demod->analyze_pulses && (cfg->grab_mode <= 1 || (cfg->grab_mode == 2 && p_events == 0) || (cfg->grab_mode == 3 && p_events > 0)) ) {
                    r_device device = {.log_fn = log_device_handler, .output_ctx = cfg};
                    pulse_analyzer(&demod->pulse_data, package_type, &device);
                }

            } else if (package_type == PULSE_DATA_FSK) {
                calc_rssi_snr(cfg, &demod->fsk_pulse_data);
                if (demod->analyze_pulses) fprintf(stderr, "Detected FSK package\t%s\n", time_pos_str(cfg, demod->fsk_pulse_data.start_ago, time_str));

                p_events += run_fsk_demods(&demod->r_devs, &demod->fsk_pulse_data);
                cfg->total_frames_fsk +=1;
                cfg->total_frames_events += p_events > 0;
                cfg->frames_fsk += 1;
                cfg->frames_events += p_events > 0;

                for (void **iter = demod->dumper.elems; iter && *iter; ++iter) {
                    file_info_t const *dumper = *iter;
                    if (dumper->format == VCD_LOGIC) pulse_data_print_vcd(dumper->file, &demod->fsk_pulse_data, '"');
                    if (dumper->format == U8_LOGIC) pulse_data_dump_raw(demod->u8_buf, n_samples, cfg->input_pos, &demod->fsk_pulse_data, 0x04);
                    if (dumper->format == PULSE_OOK) pulse_data_dump(dumper->file, &demod->fsk_pulse_data);
                }

                if (cfg->verbosity >= LOG_TRACE) pulse_data_print(&demod->fsk_pulse_data);
                if (cfg->raw_mode == 1 || (cfg->raw_mode == 2 && p_events == 0) || (cfg->raw_mode == 3 && p_events > 0)) {
                    data_t *data = pulse_data_print_data(&demod->fsk_pulse_data);
                    event_occurred_handler(cfg, data);
                }
                if (demod->analyze_pulses && (cfg->grab_mode <= 1 || (cfg->grab_mode == 2 && p_events == 0) || (cfg->grab_mode == 3 && p_events > 0))) {
                    r_device device = {.log_fn = log_device_handler, .output_ctx = cfg};
                    pulse_analyzer(&demod->fsk_pulse_data, package_type, &device);
                }
            } // if (package_type == ...
            d_events += p_events;
        } // while (package_type)...

        // add event counter to the frames currently tracked
        demod->frame_event_count += d_events;

        // end frame tracking if older than a whole buffer
        if (demod->frame_start_ago && demod->frame_end_ago > n_samples) {
            if (demod->samp_grab) {
                if (cfg->grab_mode == 1
                        || (cfg->grab_mode == 2 && demod->frame_event_count == 0)
                        || (cfg->grab_mode == 3 && demod->frame_event_count > 0)) {
                    unsigned frame_pad = n_samples / 8; // this could also be a fixed value, e.g. 10000 samples
                    unsigned start_padded = demod->frame_start_ago + frame_pad;
                    unsigned end_padded = demod->frame_end_ago - frame_pad;
                    unsigned len_padded = start_padded - end_padded;
                    samp_grab_write(demod->samp_grab, len_padded, end_padded);
                }
            }
            demod->frame_start_ago = 0;
            demod->frame_event_count = 0;
        }

        // dump partial pulse_data for this buffer
        for (void **iter = demod->dumper.elems; iter && *iter; ++iter) {
            file_info_t const *dumper = *iter;
            if (dumper->format == U8_LOGIC) {
                pulse_data_dump_raw(demod->u8_buf, n_samples, cfg->input_pos, &demod->pulse_data, 0x02);
                pulse_data_dump_raw(demod->u8_buf, n_samples, cfg->input_pos, &demod->fsk_pulse_data, 0x04);
                break;
            }
        }
    }

    if (demod->am_analyze) {
        am_analyze(demod->am_analyze, demod->am_buf, n_samples, cfg->verbosity >= LOG_INFO, NULL);
    }

    for (void **iter = demod->dumper.elems; iter && *iter; ++iter) {
        file_info_t const *dumper = *iter;
        if (!dumper->file
                || dumper->format == VCD_LOGIC
                || dumper->format == PULSE_OOK)
            continue;
        uint8_t *out_buf = iq_buf;  // Default is to dump IQ samples
        unsigned long out_len = n_samples * demod->sample_size;

        if (dumper->format == CU8_IQ) {
            if (demod->sample_size == 4) {
                for (unsigned long n = 0; n < n_samples * 2; ++n)
                    ((uint8_t *)demod->buf.temp)[n] = (((int16_t *)iq_buf)[n] / 256) + 128; // scale Q0.15 to Q0.7
                out_buf = (uint8_t *)demod->buf.temp;
                out_len = n_samples * 2 * sizeof(uint8_t);
            }
        }
        else if (dumper->format == CS16_IQ) {
            if (demod->sample_size == 2) {
                for (unsigned long n = 0; n < n_samples * 2; ++n)
                    ((int16_t *)demod->buf.temp)[n] = (iq_buf[n] * 256) - 32768; // scale Q0.7 to Q0.15
                out_buf = (uint8_t *)demod->buf.temp; // this buffer is too small if out_block_size is large
                out_len = n_samples * 2 * sizeof(int16_t);
            }
        }
        else if (dumper->format == CS8_IQ) {
            if (demod->sample_size == 2) {
                for (unsigned long n = 0; n < n_samples * 2; ++n)
                    ((int8_t *)demod->buf.temp)[n] = (iq_buf[n] - 128);
            }
            else if (demod->sample_size == 4) {
                for (unsigned long n = 0; n < n_samples * 2; ++n)
                    ((int8_t *)demod->buf.temp)[n] = ((int16_t *)iq_buf)[n] >> 8;
            }
            out_buf = (uint8_t *)demod->buf.temp;
            out_len = n_samples * 2 * sizeof(int8_t);
        }
        else if (dumper->format == CF32_IQ) {
            if (demod->sample_size == 2) {
                for (unsigned long n = 0; n < n_samples * 2; ++n)
                    ((float *)demod->buf.temp)[n] = (iq_buf[n] - 128) / 128.0f;
            }
            else if (demod->sample_size == 4) {
                for (unsigned long n = 0; n < n_samples * 2; ++n)
                    ((float *)demod->buf.temp)[n] = ((int16_t *)iq_buf)[n] / 32768.0f;
            }
            out_buf = (uint8_t *)demod->buf.temp; // this buffer is too small if out_block_size is large
            out_len = n_samples * 2 * sizeof(float);
        }
        else if (dumper->format == S16_AM) {
            out_buf = (uint8_t *)demod->am_buf;
            out_len = n_samples * sizeof(int16_t);
        }
        else if (dumper->format == S16_FM) {
            out_buf = (uint8_t *)demod->buf.fm;
            out_len = n_samples * sizeof(int16_t);
        }
        else if (dumper->format == F32_AM) {
            for (unsigned long n = 0; n < n_samples; ++n)
                demod->f32_buf[n] = demod->am_buf[n] * (1.0f / 0x8000); // scale from Q0.15
            out_buf = (uint8_t *)demod->f32_buf;
            out_len = n_samples * sizeof(float);
        }
        else if (dumper->format == F32_FM) {
            for (unsigned long n = 0; n < n_samples; ++n)
                demod->f32_buf[n] = demod->buf.fm[n] * (1.0f / 0x8000); // scale from Q0.15
            out_buf = (uint8_t *)demod->f32_buf;
            out_len = n_samples * sizeof(float);
        }
        else if (dumper->format == F32_I) {
            if (demod->sample_size == 2)
                for (unsigned long n = 0; n < n_samples; ++n)
                    demod->f32_buf[n] = (iq_buf[n * 2] - 128) * (1.0f / 0x80); // scale from Q0.7
            else
                for (unsigned long n = 0; n < n_samples; ++n)
                    demod->f32_buf[n] = ((int16_t *)iq_buf)[n * 2] * (1.0f / 0x8000); // scale from Q0.15
            out_buf = (uint8_t *)demod->f32_buf;
            out_len = n_samples * sizeof(float);
        }
        else if (dumper->format == F32_Q) {
            if (demod->sample_size == 2)
                for (unsigned long n = 0; n < n_samples; ++n)
                    demod->f32_buf[n] = (iq_buf[n * 2 + 1] - 128) * (1.0f / 0x80); // scale from Q0.7
            else
                for (unsigned long n = 0; n < n_samples; ++n)
                    demod->f32_buf[n] = ((int16_t *)iq_buf)[n * 2 + 1] * (1.0f / 0x8000); // scale from Q0.15
            out_buf = (uint8_t *)demod->f32_buf;
            out_len = n_samples * sizeof(float);
        }
        else if (dumper->format == U8_LOGIC) { // state data
            out_buf = demod->u8_buf;
            out_len = n_samples;
        }

        if (fwrite(out_buf, 1, out_len, dumper->file) != out_len) {
            print_log(LOG_ERROR, __func__, "Short write, samples lost, exiting!");
            cfg->exit_async = 1;
        }
    }

    cfg->input_pos += n_samples;
    if (cfg->bytes_to_read > 0)
        cfg->bytes_to_read -= len;

    if (cfg->after_successful_events_flag && (d_events > 0)) {
        if (cfg->after_successful_events_flag == 1) {
            cfg->exit_async = 1;
        }
        else {
            cfg->hop_now = 1;
        }
    }

    time_t rawtime;
    time(&rawtime);
    // choose hop_index as frequency_index, if there are too few hop_times use the last one
    int hop_index = cfg->hop_times > cfg->frequency_index ? cfg->frequency_index : cfg->hop_times - 1;
    if (cfg->hop_times > 0 && cfg->frequencies > 1
            && difftime(rawtime, cfg->hop_start_time) >= cfg->hop_time[hop_index]) {
        cfg->hop_now = 1;
    }
    if (cfg->duration > 0 && rawtime >= cfg->stop_time) {
        cfg->exit_async = 1;
        print_log(LOG_CRITICAL, __func__, "Time expired, exiting!");
    }
    if (cfg->stats_now || (cfg->report_stats && cfg->stats_interval && rawtime >= cfg->stats_time)) {
        event_occurred_handler(cfg, create_report_data(cfg, cfg->stats_now ? 3 : cfg->report_stats));
        flush_report_data(cfg);
        if (rawtime >= cfg->stats_time)
            cfg->stats_time += cfg->stats_interval;
        if (cfg->stats_now)
            cfg->stats_now--;
    }

    if (cfg->hop_now && !cfg->exit_async) {
        cfg->hop_now = 0;
        time(&cfg->hop_start_time);
        cfg->frequency_index = (cfg->frequency_index + 1) % cfg->frequencies;
        sdr_set_center_freq(cfg->dev, cfg->frequency[cfg->frequency_index], 1);
    }
}

void r_reset_callback(r_cfg_t *cfg)
{
    reset_sdr_callback(cfg);
}

void r_process_buf(r_cfg_t *cfg, unsigned char *buf, uint32_t len)
{
    sdr_callback(buf, len, cfg);
}

static void timer_handler(struct mg_connection *nc, int ev, void *ev_data);

// called by mg_mgr_poll() for each connection.
static void sdr_handler(struct mg_connection *nc, int ev_type, void *ev_data)
{
    if (nc->sock != INVALID_SOCKET || ev_type != MG_EV_POLL) {
        return;
    }
    if (nc->handler != timer_handler) {
        return;
    }

    r_cfg_t *cfg     = nc->user_data;
    sdr_event_t *ev = ev_data;

    data_t *data = NULL;
    if (ev->ev & SDR_EV_RATE) {
        data = data_int(data, "sample_rate", "", NULL, ev->sample_rate);
    }
    if (ev->ev & SDR_EV_CORR) {
        data = data_int(data, "freq_correction", "", NULL, ev->freq_correction);
    }
    if (ev->ev & SDR_EV_FREQ) {
        data = data_int(data, "center_frequency", "", NULL, ev->center_frequency);
        if (cfg->frequencies > 1) {
            data = data_ary(data, "frequencies", "", NULL, data_array(cfg->frequencies, DATA_INT, cfg->frequency));
            data = data_ary(data, "hop_times", "", NULL, data_array(cfg->hop_times, DATA_INT, cfg->hop_time));
        }
    }
    if (ev->ev & SDR_EV_GAIN) {
        data = data_str(data, "gain", "", NULL, ev->gain_str);
    }
    if (data) {
        event_occurred_handler(cfg, data);
    }

    if (ev->ev == SDR_EV_DATA) {
        cfg->samp_rate        = ev->sample_rate;
        cfg->center_frequency = ev->center_frequency;
        sdr_callback((unsigned char *)ev->buf, ev->len, cfg);
    }

    if (cfg->exit_async) {
        if (cfg->verbosity >= 2)
            print_log(LOG_INFO, "Input", "sdr_handler exit");
        sdr_stop(cfg->dev);
        cfg->exit_async++;
    }
}

// note that this function is called in a different thread
static void acquire_callback(sdr_event_t *ev, void *ctx)
{
    struct mg_mgr *mgr = ctx;
    mg_broadcast(mgr, sdr_handler, (void *)ev, sizeof(*ev));
}

static void timer_handler(struct mg_connection *nc, int ev, void *ev_data)
{
    r_cfg_t *cfg = (r_cfg_t *)nc->user_data;
    if (r_sig_hup) {
        reopen_dumpers(cfg);
        r_sig_hup = 0;
    }
    switch (ev) {
    case MG_EV_TIMER: {
        double now  = *(double *)ev_data;
        (void) now;
        double next = mg_time() + 1.5;
        mg_set_timer(nc, next);

        if (cfg->watchdog != 0) {
            if (cfg->dev_state == DEVICE_STATE_STARTING
                    || cfg->dev_state == DEVICE_STATE_GRACE) {
                cfg->dev_state = DEVICE_STATE_STARTED;
                time(&cfg->sdr_since);
            }
            cfg->watchdog = 0;
            break;
        }

        if (cfg->dev_state == DEVICE_STATE_STARTING) {
            cfg->dev_state = DEVICE_STATE_GRACE;
            break;
        }
        if (cfg->dev_state == DEVICE_STATE_GRACE) {
            if (cfg->dev_mode == DEVICE_MODE_QUIT) {
                print_log(LOG_ERROR, "Input", "Input device start failed, exiting!");
            }
            else if (cfg->dev_mode == DEVICE_MODE_RESTART) {
                print_log(LOG_WARNING, "Input", "Input device start failed, restarting!");
            }
            else {
                print_log(LOG_WARNING, "Input", "Input device start failed, pausing!");
            }
        }
        else if (cfg->dev_state == DEVICE_STATE_STARTED) {
            if (cfg->dev_mode == DEVICE_MODE_QUIT) {
                print_log(LOG_ERROR, "Input", "Async read stalled, exiting!");
            }
            else if (cfg->dev_mode == DEVICE_MODE_RESTART) {
                print_log(LOG_WARNING, "Input", "Async read stalled, restarting!");
            }
            else {
                print_log(LOG_WARNING, "Input", "Async read stalled, pausing!");
            }
        }
        if (cfg->dev_state != DEVICE_STATE_STOPPED) {
            cfg->exit_async = 1;
            cfg->exit_code = 3;
            sdr_stop(cfg->dev);
            cfg->dev_state = DEVICE_STATE_STOPPED;
        }
        if (cfg->dev_mode == DEVICE_MODE_QUIT) {
            cfg->exit_async = 1;
        }
        if (cfg->dev_mode == DEVICE_MODE_RESTART) {
            r_start(cfg);
        }
        break;
    }
    }
}

int r_start(r_cfg_t *cfg)
{
    int r;
    if (cfg->dev) {
        r = sdr_close(cfg->dev);
        cfg->dev = NULL;
        if (r < 0) {
            print_logf(LOG_ERROR, "Input", "Closing SDR failed (%d)", r);
        }
    }
    r = sdr_open(&cfg->dev, cfg->dev_query, cfg->verbosity);
    if (r < 0) {
        return -1;
    }
    cfg->dev_info = sdr_get_dev_info(cfg->dev);
    cfg->demod->sample_size = sdr_get_sample_size(cfg->dev);

    sdr_set_sample_rate(cfg->dev, cfg->samp_rate, 1);

    if (cfg->verbosity || cfg->demod->level_limit < 0.0)
        print_logf(LOG_NOTICE, "Input", "Bit detection level set to %.1f%s.", cfg->demod->level_limit, (cfg->demod->level_limit < 0.0 ? "" : " (Auto)"));

    sdr_apply_settings(cfg->dev, cfg->settings_str, 1);
    sdr_set_tuner_gain(cfg->dev, cfg->gain_str, 1);

    if (cfg->ppm_error) {
        sdr_set_freq_correction(cfg->dev, cfg->ppm_error, 1);
    }

    r = sdr_reset(cfg->dev, cfg->verbosity);
    if (r < 0) {
        print_log(LOG_ERROR, "Input", "Failed to reset buffers.");
    }
    sdr_activate(cfg->dev);

    if (cfg->verbosity) {
        print_log(LOG_NOTICE, "Input", "Reading samples in async mode...");
    }

    sdr_set_center_freq(cfg->dev, cfg->center_frequency, 1);

    r = sdr_start(cfg->dev, acquire_callback, (void *)get_mgr(cfg),
            DEFAULT_ASYNC_BUF_NUMBER, cfg->out_block_size);
    if (r < 0) {
        print_logf(LOG_ERROR, "Input", "async start failed (%d).", r);
    }

    cfg->dev_state = DEVICE_STATE_STARTING;
    return r;
}

void r_run(r_cfg_t *cfg)
{
    time(&cfg->hop_start_time);

    struct mg_add_sock_opts opts = {.user_data = cfg};
    struct mg_connection *nc = mg_add_sock_opt(get_mgr(cfg), INVALID_SOCKET, timer_handler, opts);
    mg_set_timer(nc, mg_time() + 2.5);

    while (!cfg->exit_async) {
        mg_mgr_poll(cfg->mgr, 500);
    }
    if (cfg->verbosity >= LOG_INFO)
        print_log(LOG_INFO, "rtl_433", "stopping...");
    sdr_stop(cfg->dev);
}

void r_stop(r_cfg_t *cfg)
{
    cfg->exit_async = 1;
}
