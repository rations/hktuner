/*
 * SPDX-License-Identifier: MIT
 *
 * hktuner — chromatic tuner LV2 plugin for Haiku. Stereo pass-through: the
 * audio is copied to the outputs untouched; pitch detection (time-domain
 * MPM/NSDF per McLeod & Wyvill, "A Smarter Way to Find Pitch", ICMC 2005)
 * runs on the host's worker thread via the LV2 worker extension.
 *
 * run() is in the audio threading class and lv2:hardRTCapable: no
 * allocation, locking, I/O, or logging here.
 *
 * TODO(phase-2): detection is not wired yet — freq/clarity output 0.0 and
 * the worker interface is not exposed. This revision is the pass-through
 * milestone only.
 */
#include "lv2/core/lv2.h"

#include <stdint.h>
#include <stdlib.h>

#define HKTUNER_URI "https://github.com/rations/hktuner"

typedef enum {
    HKT_REF_FREQ = 0,
    HKT_FREQ = 1,
    HKT_CLARITY = 2,
    HKT_IN_L = 3,
    HKT_IN_R = 4,
    HKT_OUT_L = 5,
    HKT_OUT_R = 6
} PortIndex;

typedef struct {
    const float *ref_freq;
    float *freq;
    float *clarity;
    const float *in_l;
    const float *in_r;
    float *out_l;
    float *out_r;
} Hktuner;

static LV2_Handle instantiate(const LV2_Descriptor *descriptor, double rate,
                              const char *bundle_path, const LV2_Feature *const *features)
{
    (void)descriptor;
    (void)rate;
    (void)bundle_path;
    (void)features;

    return (LV2_Handle)calloc(1, sizeof(Hktuner));
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data)
{
    Hktuner *t = (Hktuner *)instance;

    switch ((PortIndex)port) {
        case HKT_REF_FREQ:
            t->ref_freq = (const float *)data;
            break;
        case HKT_FREQ:
            t->freq = (float *)data;
            break;
        case HKT_CLARITY:
            t->clarity = (float *)data;
            break;
        case HKT_IN_L:
            t->in_l = (const float *)data;
            break;
        case HKT_IN_R:
            t->in_r = (const float *)data;
            break;
        case HKT_OUT_L:
            t->out_l = (float *)data;
            break;
        case HKT_OUT_R:
            t->out_r = (float *)data;
            break;
    }
}

static void activate(LV2_Handle instance)
{
    (void)instance;
}

static void run(LV2_Handle instance, uint32_t n_samples)
{
    const Hktuner *t = (const Hktuner *)instance;

    /* A conforming host connects every port before run(), but a port left
       unconnected must not crash the plugin. */
    if (!t->ref_freq || !t->freq || !t->clarity || !t->in_l || !t->in_r || !t->out_l || !t->out_r) {
        return;
    }

    /* Bit-exact pass-through: the tuner never colors the signal path. */
    for (uint32_t pos = 0; pos < n_samples; pos++) {
        t->out_l[pos] = t->in_l[pos];
        t->out_r[pos] = t->in_r[pos];
    }

    /* Output control ports are rewritten every cycle. */
    *t->freq = 0.0f;
    *t->clarity = 0.0f;
}

static void deactivate(LV2_Handle instance)
{
    (void)instance;
}

static void cleanup(LV2_Handle instance)
{
    free(instance);
}

static const void *extension_data(const char *uri)
{
    (void)uri;
    return NULL;
}

static const LV2_Descriptor descriptor = {HKTUNER_URI, instantiate, connect_port, activate,
                                          run,         deactivate,  cleanup,      extension_data};

LV2_SYMBOL_EXPORT
const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
