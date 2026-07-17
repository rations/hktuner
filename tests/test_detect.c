/*
 * SPDX-License-Identifier: MIT
 *
 * Offline detection test for hktuner: drives the plugin through the public
 * LV2 descriptor API with a synchronous worker (the schedule feature calls
 * work() immediately and the response is applied right after run(), which
 * the worker spec explicitly allows for freewheeling hosts). Feeds
 * synthesized tones and checks the detected frequency in cents.
 *
 * Build and run on the target; exits non-zero on any failure.
 */
#include "lv2/core/lv2.h"
#include "lv2/worker/worker.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TEST_URI "https://github.com/rations/hktuner"
#define RATE 48000.0
#define BLOCK 256
#define N_PORTS 7

extern const LV2_Descriptor *lv2_descriptor(uint32_t index);

/* ---- synchronous worker shim ---------------------------------------------- */

static const LV2_Worker_Interface *g_iface = NULL;
static LV2_Handle g_handle = NULL;
static char g_response[64];
static uint32_t g_response_size = 0;
static int g_response_pending = 0;

static LV2_Worker_Status respond_cb(LV2_Worker_Respond_Handle handle, uint32_t size,
                                    const void *data)
{
    (void)handle;
    if (size > sizeof(g_response)) {
        return LV2_WORKER_ERR_NO_SPACE;
    }
    memcpy(g_response, data, size);
    g_response_size = size;
    g_response_pending = 1;
    return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status schedule_cb(LV2_Worker_Schedule_Handle handle, uint32_t size,
                                     const void *data)
{
    (void)handle;
    if (!g_iface || !g_iface->work) {
        return LV2_WORKER_ERR_UNKNOWN;
    }
    return g_iface->work(g_handle, respond_cb, NULL, size, data);
}

/* ---- harness --------------------------------------------------------------- */

typedef struct {
    const LV2_Descriptor *desc;
    LV2_Handle inst;
    float ref_freq;
    float freq;
    float clarity;
    float in_l[BLOCK];
    float in_r[BLOCK];
    float out_l[BLOCK];
    float out_r[BLOCK];
} Harness;

/* Run `seconds` of a mono tone (or silence when hz == 0) through the plugin
   and return the final detected frequency. */
static float run_tone(Harness *h, double hz, double amp, double seconds)
{
    const uint32_t total = (uint32_t)(RATE * seconds);
    double phase = 0.0;
    const double step = 2.0 * M_PI * hz / RATE;

    for (uint32_t done = 0; done < total; done += BLOCK) {
        for (uint32_t i = 0; i < BLOCK; i++) {
            const float s = (hz > 0.0) ? (float)(amp * sin(phase)) : 0.0f;
            phase += step;
            h->in_l[i] = s;
            h->in_r[i] = s;
        }
        h->desc->run(h->inst, BLOCK);
        if (g_response_pending && g_iface && g_iface->work_response) {
            g_iface->work_response(g_handle, g_response_size, g_response);
            g_response_pending = 0;
        }
    }
    return h->freq;
}

static double cents_off(double got, double want)
{
    if (got <= 0.0 || want <= 0.0) {
        return 1e9;
    }
    return 1200.0 * log2(got / want);
}

int main(void)
{
    static LV2_Worker_Schedule sched = {NULL, schedule_cb};
    static const LV2_Feature sched_feature = {LV2_WORKER__schedule, &sched};
    static const LV2_Feature *features[] = {&sched_feature, NULL};

    Harness h;
    memset(&h, 0, sizeof(h));
    h.desc = lv2_descriptor(0);
    if (!h.desc || strcmp(h.desc->URI, TEST_URI) != 0) {
        fprintf(stderr, "FAIL: descriptor missing or wrong URI\n");
        return 1;
    }

    h.inst = h.desc->instantiate(h.desc, RATE, "/nonexistent/", features);
    if (!h.inst) {
        fprintf(stderr, "FAIL: instantiate returned NULL\n");
        return 1;
    }
    g_handle = h.inst;
    g_iface = (const LV2_Worker_Interface *)h.desc->extension_data(LV2_WORKER__interface);
    if (!g_iface) {
        fprintf(stderr, "FAIL: no worker interface\n");
        return 1;
    }

    h.ref_freq = 440.0f;
    h.desc->connect_port(h.inst, 0, &h.ref_freq);
    h.desc->connect_port(h.inst, 1, &h.freq);
    h.desc->connect_port(h.inst, 2, &h.clarity);
    h.desc->connect_port(h.inst, 3, h.in_l);
    h.desc->connect_port(h.inst, 4, h.in_r);
    h.desc->connect_port(h.inst, 5, h.out_l);
    h.desc->connect_port(h.inst, 6, h.out_r);
    if (h.desc->activate) {
        h.desc->activate(h.inst);
    }

    int failures = 0;

    /* Musical pitches across the range (E2 low guitar string .. B5). */
    static const double tones[] = {82.41, 110.0, 220.0, 440.0, 659.26, 987.77};
    for (size_t i = 0; i < sizeof(tones) / sizeof(tones[0]); i++) {
        const float got = run_tone(&h, tones[i], 0.5, 1.0);
        const double off = cents_off(got, tones[i]);
        const int ok = fabs(off) <= 1.0 && h.clarity >= 0.8f;
        printf("%s: tone %8.2f Hz -> detected %8.3f Hz (%+6.3f cents, clarity %.3f)\n",
               ok ? "PASS" : "FAIL", tones[i], (double)got, off, (double)h.clarity);
        failures += !ok;
    }

    /* Silence must gate to zero. */
    {
        const float got = run_tone(&h, 0.0, 0.0, 1.0);
        const int ok = got == 0.0f && h.clarity == 0.0f;
        printf("%s: silence -> freq %.3f clarity %.3f\n", ok ? "PASS" : "FAIL", (double)got,
               (double)h.clarity);
        failures += !ok;
    }

    /* Below the gate-on threshold the tuner must stay silent (mean abs of a
       sine is 2/pi * amp; amp 0.001 -> ~0.00064 < 0.001). */
    {
        const float got = run_tone(&h, 440.0, 0.001, 1.0);
        const int ok = got == 0.0f;
        printf("%s: sub-gate 440 Hz at amp 0.001 -> freq %.3f\n", ok ? "PASS" : "FAIL",
               (double)got);
        failures += !ok;
    }

    /* Pass-through must remain bit-exact while detecting. */
    {
        int exact = 1;
        for (uint32_t i = 0; i < BLOCK; i++) {
            h.in_l[i] = (float)sin(0.01 * (double)i) * 0.25f;
            h.in_r[i] = (float)cos(0.02 * (double)i) * 0.25f;
        }
        h.desc->run(h.inst, BLOCK);
        for (uint32_t i = 0; i < BLOCK; i++) {
            exact = exact && h.out_l[i] == h.in_l[i] && h.out_r[i] == h.in_r[i];
        }
        printf("%s: pass-through bit-exact under load\n", exact ? "PASS" : "FAIL");
        failures += !exact;
    }

    if (h.desc->deactivate) {
        h.desc->deactivate(h.inst);
    }
    h.desc->cleanup(h.inst);

    printf(failures ? "RESULT: %d failure(s)\n" : "RESULT: all tests passed\n", failures);
    return failures ? 1 : 0;
}
