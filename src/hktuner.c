/*
 * SPDX-License-Identifier: MIT
 *
 * hktuner — chromatic tuner LV2 plugin for Haiku. Stereo pass-through: the
 * audio is copied to the outputs untouched while pitch is detected on the
 * (L+R)/2 mix.
 *
 * Pitch detection is a clean-room, time-domain implementation of the McLeod
 * Pitch Method (MPM): the normalized square difference function (NSDF) with
 * key-maximum picking and parabolic interpolation, per Philip McLeod and
 * Geoff Wyvill, "A Smarter Way to Find Pitch", ICMC 2005. No FFT is used;
 * at tuner window sizes the O(W * tau_max) correlation is cheap on the
 * host's non-realtime worker thread.
 *
 * Threading (LV2 worker extension, optional feature):
 *   - run() is in the audio threading class and lv2:hardRTCapable: no
 *     allocation, locking, I/O, or logging. It copies audio, feeds a
 *     power-of-two capture ring, and every ~100 ms snapshots the newest
 *     window and calls schedule_work() with a 4-byte token (RT-safe per the
 *     worker spec: the host copies the token into a lock-free ring).
 *   - work() runs in the host's non-RT context (serialized calls per the
 *     worker spec) and does all NSDF math on the snapshot.
 *   - work_response() is called in the RT context: two float stores + flag
 *     clear only.
 *   - `busy` is written only by run() (set) and work_response() (clear),
 *     both on the RT thread, so it needs no atomics. While busy == 1 run()
 *     never touches `snap`, so work() reads a stable buffer; the
 *     release/acquire edge is the host's SPSC request ring.
 *   - Without the schedule feature (e.g. offline hosts passing no
 *     features) the plugin is a pure pass-through and freq stays 0.
 */
#include "lv2/core/lv2.h"
#include "lv2/worker/worker.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HKTUNER_URI "https://github.com/rations/hktuner"

/* Detection calibration. Window ~85 ms and ~10 estimates/s are standard
   tuner values; the gate hysteresis pair is in mean-absolute-sample terms.
   Results above 1 kHz are rejected: lag resolution (and musical need) drop
   off there. A decaying pluck loses fundamental energy (clarity) and level
   long before it stops sounding, so the last good reading is held for
   HKT_HOLD_SEC after detection drops out — the hardware-tuner behavior —
   and only then does the display clear. */
#define HKT_WINDOW_SEC 0.085
#define HKT_HOP_SEC 0.1
#define HKT_HOLD_SEC 1.0
#define HKT_FREQ_MAX 1000.0f
#define HKT_FREQ_MIN 40.0
#define HKT_GATE_ON 0.0005f
#define HKT_GATE_OFF 0.00045f
#define HKT_CLARITY_MIN 0.60f
#define HKT_PEAK_FACTOR 0.9f

typedef enum {
    HKT_REF_FREQ = 0,
    HKT_FREQ = 1,
    HKT_CLARITY = 2,
    HKT_IN_L = 3,
    HKT_IN_R = 4,
    HKT_OUT_L = 5,
    HKT_OUT_R = 6
} PortIndex;

/* work() -> work_response() payload. */
typedef struct {
    float freq;
    float clarity;
} HktResult;

typedef struct {
    /* Ports. */
    const float *ref_freq;
    float *freq;
    float *clarity;
    const float *in_l;
    const float *in_r;
    float *out_l;
    float *out_r;

    /* Host features. */
    LV2_Worker_Schedule *sched; /* NULL = no worker; detection disabled */

    /* Pre-allocated analysis state (sized in instantiate()). */
    double rate;
    uint32_t win;     /* W: power-of-two window/capture size    */
    uint32_t mask;    /* win - 1                                */
    uint32_t tau_max; /* min(rate / 40 Hz, win / 2)             */
    uint32_t hop;     /* frames between estimates               */
    float *cap;       /* capture ring; RT writer only           */
    float *snap;      /* snapshot window; see `busy` contract   */
    float *nsdf;      /* worker scratch, tau_max + 1 entries    */
    uint32_t cap_pos; /* next write index (oldest sample)       */
    uint32_t hop_count;

    /* RT <-> worker handshake (see file header for the contract). */
    int busy;       /* RT-thread-only flag                    */
    float cur_freq; /* latest published result (RT copies)    */
    float cur_clarity;
    int gate_open;      /* worker-only hysteresis state           */
    uint32_t hold_hops; /* estimates the last reading is held for */
    uint32_t hold_left; /* worker-only hold countdown             */
    float held_freq;    /* worker-only last good reading          */
    float held_clarity;
} Hktuner;

static uint32_t next_pow2(uint32_t v)
{
    uint32_t p = 1;
    while (p < v && p < (1u << 30)) {
        p <<= 1;
    }
    return p;
}

static LV2_Handle instantiate(const LV2_Descriptor *descriptor, double rate,
                              const char *bundle_path, const LV2_Feature *const *features)
{
    (void)descriptor;
    (void)bundle_path;

    if (rate <= 0.0 || rate > 1000000.0) {
        return NULL;
    }

    Hktuner *t = (Hktuner *)calloc(1, sizeof(Hktuner));
    if (!t) {
        return NULL;
    }

    for (uint32_t i = 0; features && features[i]; i++) {
        if (!strcmp(features[i]->URI, LV2_WORKER__schedule)) {
            t->sched = (LV2_Worker_Schedule *)features[i]->data;
        }
    }

    t->rate = rate;
    t->win = next_pow2((uint32_t)(rate * HKT_WINDOW_SEC));
    t->mask = t->win - 1;
    t->tau_max = (uint32_t)(rate / HKT_FREQ_MIN);
    if (t->tau_max > t->win / 2) {
        t->tau_max = t->win / 2;
    }
    t->hop = (uint32_t)(rate * HKT_HOP_SEC);
    t->hold_hops = (uint32_t)(HKT_HOLD_SEC / HKT_HOP_SEC + 0.5);
    if (t->tau_max < 16) {
        free(t);
        return NULL;
    }

    t->cap = (float *)calloc(t->win, sizeof(float));
    t->snap = (float *)calloc(t->win, sizeof(float));
    t->nsdf = (float *)calloc((size_t)t->tau_max + 1, sizeof(float));
    if (!t->cap || !t->snap || !t->nsdf) {
        free(t->cap);
        free(t->snap);
        free(t->nsdf);
        free(t);
        return NULL;
    }

    return (LV2_Handle)t;
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
    Hktuner *t = (Hktuner *)instance;

    /* Non-RT per the LV2 threading rules: safe to clear buffers here. */
    memset(t->cap, 0, t->win * sizeof(float));
    t->cap_pos = 0;
    t->hop_count = 0;
    t->busy = 0;
    t->cur_freq = 0.0f;
    t->cur_clarity = 0.0f;
    t->gate_open = 0;
    t->hold_left = 0;
    t->held_freq = 0.0f;
    t->held_clarity = 0.0f;
}

static void run(LV2_Handle instance, uint32_t n_samples)
{
    Hktuner *t = (Hktuner *)instance;

    /* A conforming host connects every port before run(), but a port left
       unconnected must not crash the plugin. */
    if (!t->ref_freq || !t->freq || !t->clarity || !t->in_l || !t->in_r || !t->out_l || !t->out_r) {
        return;
    }

    /* Bit-exact pass-through: the tuner never colors the signal path. */
    for (uint32_t pos = 0; pos < n_samples; pos++) {
        t->out_l[pos] = t->in_l[pos];
        t->out_r[pos] = t->in_r[pos];
        t->cap[(t->cap_pos + pos) & t->mask] = 0.5f * (t->in_l[pos] + t->in_r[pos]);
    }
    t->cap_pos = (t->cap_pos + n_samples) & t->mask;

    t->hop_count += n_samples;
    if (t->sched && t->hop_count >= t->hop && !t->busy) {
        /* Linearize the ring, oldest sample first: cap_pos is the next
           write slot, i.e. the oldest sample. Two bounded memcpys. */
        const uint32_t head = t->win - t->cap_pos;
        memcpy(t->snap, t->cap + t->cap_pos, head * sizeof(float));
        memcpy(t->snap + head, t->cap, t->cap_pos * sizeof(float));
        t->busy = 1;
        t->hop_count = 0;
        const uint32_t token = 1;
        if (t->sched->schedule_work(t->sched->handle, sizeof(token), &token) !=
            LV2_WORKER_SUCCESS) {
            t->busy = 0; /* ring full or unavailable: retry next hop */
        }
    }

    /* Output control ports are rewritten every cycle. */
    *t->freq = t->cur_freq;
    *t->clarity = t->cur_clarity;
}

/* ---- Worker side (non-RT) ------------------------------------------------ */

/* NSDF per McLeod & Wyvill: nsdf(tau) = 2 * r(tau) / m(tau), where r is the
   autocorrelation and m the sum of squares of both windows. m is updated
   incrementally: m(tau + 1) = m(tau) - x[tau]^2 - x[W - 1 - tau]^2. The
   function is computed from lag 0 so the peak picker sees the zero-lag lobe
   and its negative-going crossing; out-of-range pitches are rejected by
   frequency afterwards. */
static void compute_nsdf(const float *x, uint32_t w, uint32_t tau_max, float *nsdf)
{
    double sumsq = 0.0;
    for (uint32_t i = 0; i < w; i++) {
        sumsq += (double)x[i] * (double)x[i];
    }

    double m = 2.0 * sumsq;

    for (uint32_t tau = 0; tau <= tau_max; tau++) {
        double r = 0.0;
        const uint32_t n = w - tau;
        for (uint32_t i = 0; i < n; i++) {
            r += (double)x[i] * (double)x[i + tau];
        }
        nsdf[tau] = (m > 1e-12) ? (float)(2.0 * r / m) : 0.0f;
        m -= (double)x[tau] * (double)x[tau];
        m -= (double)x[w - 1 - tau] * (double)x[w - 1 - tau];
    }
}

/* Key-maximum picking per the paper: after the first negative-going zero
   crossing, take the highest NSDF value in each positive run; accept the
   first key maximum >= HKT_PEAK_FACTOR * (global key maximum). The scan
   starts at lag 0, inside the zero-lag lobe, whose crossing arms the state
   machine. Returns 0 if no key maximum exists. */
static uint32_t pick_peak(const float *nsdf, uint32_t tau_max)
{
    const uint32_t tau_min = 0;
    enum { WAIT_NEG, WAIT_POS, IN_RUN } state = WAIT_NEG;
    /* Worst case one key maximum per two lags; bound the list statically by
       scanning twice instead of storing: pass 1 finds the global key max,
       pass 2 the first one over threshold. */
    float global_max = 0.0f;

    for (int pass = 0; pass < 2; pass++) {
        const float threshold = HKT_PEAK_FACTOR * global_max;
        float run_max = 0.0f;
        uint32_t run_tau = 0;
        state = WAIT_NEG;
        for (uint32_t tau = tau_min; tau <= tau_max; tau++) {
            const float v = nsdf[tau];
            switch (state) {
                case WAIT_NEG:
                    if (v < 0.0f) {
                        state = WAIT_POS;
                    }
                    break;
                case WAIT_POS:
                    if (v > 0.0f) {
                        state = IN_RUN;
                        run_max = v;
                        run_tau = tau;
                    }
                    break;
                case IN_RUN:
                    if (v > run_max) {
                        run_max = v;
                        run_tau = tau;
                    }
                    if (v < 0.0f || tau == tau_max) {
                        if (pass == 0) {
                            if (run_max > global_max) {
                                global_max = run_max;
                            }
                        } else if (run_max >= threshold && run_max > 0.0f) {
                            return run_tau;
                        }
                        state = WAIT_POS;
                    }
                    break;
            }
        }
        if (pass == 0 && global_max <= 0.0f) {
            return 0;
        }
    }
    return 0;
}

static LV2_Worker_Status work(LV2_Handle instance, LV2_Worker_Respond_Function respond,
                              LV2_Worker_Respond_Handle handle, uint32_t size, const void *data)
{
    Hktuner *t = (Hktuner *)instance;
    HktResult res = {0.0f, 0.0f};

    /* The token is our own 4-byte marker; anything else is malformed. */
    if (!respond || size != sizeof(uint32_t) || !data) {
        return LV2_WORKER_ERR_UNKNOWN;
    }

    /* Amplitude gate with hysteresis (worker-only state). */
    double acc = 0.0;
    for (uint32_t i = 0; i < t->win; i++) {
        acc += fabs((double)t->snap[i]);
    }
    const float mean_abs = (float)(acc / (double)t->win);
    if (t->gate_open) {
        t->gate_open = mean_abs > HKT_GATE_OFF;
    } else {
        t->gate_open = mean_abs > HKT_GATE_ON;
    }

    if (t->gate_open) {
        compute_nsdf(t->snap, t->win, t->tau_max, t->nsdf);
        const uint32_t tau = pick_peak(t->nsdf, t->tau_max);
        if (tau >= 1 && tau < t->tau_max) {
            /* Parabolic interpolation around the key maximum. */
            const float a = t->nsdf[tau - 1];
            const float b = t->nsdf[tau];
            const float c = t->nsdf[tau + 1];
            const float den = a - 2.0f * b + c;
            float delta = 0.0f;
            if (fabsf(den) > 1e-9f) {
                delta = 0.5f * (a - c) / den;
                if (delta > 0.5f) {
                    delta = 0.5f;
                } else if (delta < -0.5f) {
                    delta = -0.5f;
                }
            }
            const float freq = (float)(t->rate / ((double)tau + (double)delta));
            float clarity = b;
            if (clarity > 1.0f) {
                clarity = 1.0f;
            }
            if (clarity >= HKT_CLARITY_MIN && freq <= HKT_FREQ_MAX) {
                res.freq = freq;
                res.clarity = clarity;
            }
        }
    }

    /* Display hold (worker-only state): keep the last good reading through
       short detection dropouts, then clear. */
    if (res.freq > 0.0f) {
        t->held_freq = res.freq;
        t->held_clarity = res.clarity;
        t->hold_left = t->hold_hops;
    } else if (t->hold_left > 0) {
        t->hold_left--;
        res.freq = t->held_freq;
        res.clarity = t->held_clarity;
    }

    return respond(handle, sizeof(res), &res);
}

/* Called by the host in the RT context: plain stores only. */
static LV2_Worker_Status work_response(LV2_Handle instance, uint32_t size, const void *body)
{
    Hktuner *t = (Hktuner *)instance;

    if (size == sizeof(HktResult) && body) {
        const HktResult *res = (const HktResult *)body;
        t->cur_freq = res->freq;
        t->cur_clarity = res->clarity;
    }
    t->busy = 0;
    return LV2_WORKER_SUCCESS;
}

static const LV2_Worker_Interface worker_iface = {work, work_response, NULL};

static void deactivate(LV2_Handle instance)
{
    (void)instance;
}

static void cleanup(LV2_Handle instance)
{
    Hktuner *t = (Hktuner *)instance;

    if (t) {
        free(t->cap);
        free(t->snap);
        free(t->nsdf);
        free(t);
    }
}

static const void *extension_data(const char *uri)
{
    if (uri && !strcmp(uri, LV2_WORKER__interface)) {
        return &worker_iface;
    }
    return NULL;
}

static const LV2_Descriptor descriptor = {HKTUNER_URI, instantiate, connect_port, activate,
                                          run,         deactivate,  cleanup,      extension_data};

LV2_SYMBOL_EXPORT
const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
