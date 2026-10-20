// SPDX-License-Identifier: MIT
// Copyright (c) 2026 thesammy58
// Written with Claude, Anthropic's AI model; see "How this was made" in README.md.
/*
 * leveler.h - Radio loudness leveler for The Sims 3 (RadioLeveler.asi)
 *
 * Block-based (256 frames), planar float, up to LV_MAXCH channels.
 * Pure C, no allocation, no locks: safe to call from the audio mixer thread.
 *
 * Signal flow per block:
 *   K-weighted power -> short (0.4 s) / medium (3 s) / per-song (adaptive) detectors
 *   -> song-boundary detection (silence gap or sustained level jump)
 *   -> gain = clamp(target - songLoudness, -maxCut, +maxBoost), slew-limited
 *   -> gain applied with per-sample interpolation
 *   -> grouped lookahead peak limiter (sliding-min + box smoothing + release)
 */
#ifndef RADIO_LEVELER_H
#define RADIO_LEVELER_H

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LV_MAXCH 8
#define LV_BLOCK 256
#define LV_MAXLA 256

typedef struct LvParams {
    float targetLufs;      /* loudness every song is steered to (pre-volume), LUFS             */
    float maxBoostDb;      /* largest upward correction                                        */
    float maxCutDb;        /* largest downward correction                                      */
    float gateLufs;        /* absolute gate: quieter momentary audio never moves the estimate  */
    float relGateDb;       /* relative gate below the current estimate (intros, fades)         */
    float silenceLufs;     /* fast (50 ms) level that counts as a gap between songs            */
    float silenceMs;       /* how long that gap must last                                      */
    float forgetSec;       /* song estimate memory cap (limits damage of a missed song change) */
    float priorUpSec;      /* new song quieter than the last: seconds of trust kept in the old */
                           /* estimate (large = boost slowly, quiet intros are not lifted)     */
    float priorDownSec;    /* new song louder than the last: same, small = cut quickly         */
    float upDbps;          /* gain slew limit upwards, dB per second                           */
    float downDbps;        /* gain slew limit downwards, dB per second                         */
    float jumpDb;          /* sustained deviation that counts as a new song when there is no gap */
    float jumpSec;         /* ...for this long                                                 */
    float ceilingDb;       /* limiter ceiling, dBFS (pre-volume)                                */
    float lookaheadMs;     /* limiter lookahead                                                */
    float releaseMs;       /* limiter release time constant                                    */
    int   limiterOn;
} LvParams;

/* Shipped baseline ("standard" preset): the settings tuned in-game for v1.3.0. */
static inline void lv_default_params(LvParams* p) {
    p->targetLufs = -16.0f;
    p->maxBoostDb = 10.0f;
    p->maxCutDb = 12.0f;
    p->gateLufs = -50.0f;
    p->relGateDb = 20.0f;
    p->silenceLufs = -45.0f;
    p->silenceMs = 250.0f;
    p->forgetSec = 8.0f;
    p->priorUpSec = 1.5f;
    p->priorDownSec = 0.5f;
    p->upDbps = 6.0f;
    p->downDbps = 24.0f;
    p->jumpDb = 6.0f;
    p->jumpSec = 4.0f;
    p->ceilingDb = -1.0f;
    p->lookaheadMs = 3.0f;
    p->releaseMs = 120.0f;
    p->limiterOn = 1;
}

typedef struct LvBiquad { double b0, b1, b2, a1, a2; } LvBiquad;

typedef struct LvState {
    LvParams p;
    float  fs;
    int    nch;
    int    ready;          /* 0 = passthrough (bad fs / channel count) */
    double blockSec;

    /* K-weighting */
    LvBiquad k1, k2;
    double kz[LV_MAXCH][4];

    /* detectors (mean-square power, linear) */
    double aF, aS, aM, aForget; /* EMA coefficients: 50 ms, 0.4 s, 3 s, forget */
    double pF, pS, pM;
    double cumP;           /* sum of gated block powers since song start */
    double cumN;           /* number of gated blocks since song start    */
    double priorDb;        /* loudness estimate carried over from the previous song */
    int    havePrior;
    double estDb;          /* current song loudness estimate (LUFS) */
    int    haveEst;
    double tSong;          /* seconds of gated audio since song start */
    double silenceT;
    int    inGap;
    double jumpT;

    /* gain */
    float  gDb;            /* current leveler gain, dB */
    float  gLinPrev;       /* gain at end of previous block (for interpolation) */

    /* limiter */
    int    la;             /* lookahead in samples */
    float  dly[LV_MAXCH][LV_MAXLA];
    int    dpos;
    float  rq[LV_MAXLA + 1];      /* required-gain ring (for box sum) */
    int    rqpos;
    double boxSum;
    int    dqIdx[LV_MAXLA + 2];   /* monotonic deque of sample indices (sliding min) */
    float  dqVal[LV_MAXLA + 2];
    int    dqHead, dqTail;
    unsigned int sampleIdx;
    float  relCoef;
    float  limG;
    float  ceilLin;
    int    boxRecalc;

    /* telemetry (read by the monitor thread; benign races) */
    volatile float tmShort, tmSong, tmGainDb, tmLimDb, tmMinLimDb;
    volatile unsigned int tmBlocks, tmSongs;
    volatile float tmPrevSong, tmPrevGain, tmPrevMinLim; /* finished song: estimate, final gain, worst limiting */
    volatile unsigned int tmReason;                     /* what started the current song: 1 gap, 2 level jump */
    float  songMinLim;                                  /* worst limiter gain (linear) in the current song    */
    int    pendingSong;    /* host saw the player switch to a new song: start one on the next block */
    int    playerTracked;  /* host song signal has proven itself: silence/jump guessing turned off  */
    float  wet, wetTarget; /* 1 = leveling applied, 0 = A/B bypass (still measuring, same latency)  */
    int    seeded;         /* started from another voice's last song                                */
} LvState;

static inline double lv_db2pow(double db) { return pow(10.0, db / 10.0); }
static inline double lv_lufs(double ms) { return ms > 1e-20 ? -0.691 + 10.0 * log10(ms) : -200.0; }

static inline void lv_kweight(LvBiquad* s1, LvBiquad* s2, double fs) {
    /* BS.1770 pre-filter + RLB high-pass, re-derived for arbitrary fs (as libebur128) */
    double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    double K = tan(M_PI * f0 / fs);
    double Vh = pow(10.0, G / 20.0);
    double Vb = pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    s1->b0 = (Vh + Vb * K / Q + K * K) / a0;
    s1->b1 = 2.0 * (K * K - Vh) / a0;
    s1->b2 = (Vh - Vb * K / Q + K * K) / a0;
    s1->a1 = 2.0 * (K * K - 1.0) / a0;
    s1->a2 = (1.0 - K / Q + K * K) / a0;
    f0 = 38.13547087602444; Q = 0.5003270373238773;
    K = tan(M_PI * f0 / fs);
    a0 = 1.0 + K / Q + K * K;
    s2->b0 = 1.0; s2->b1 = -2.0; s2->b2 = 1.0;
    s2->a1 = 2.0 * (K * K - 1.0) / a0;
    s2->a2 = (1.0 - K / Q + K * K) / a0;
}

static inline void lv_reset_limiter(LvState* s) {
    memset(s->dly, 0, sizeof(s->dly));
    s->dpos = 0;
    for (int i = 0; i <= LV_MAXLA; i++) s->rq[i] = 1.0f;
    s->rqpos = 0;
    s->boxSum = (double)s->la;
    s->dqHead = s->dqTail = 0;
    s->sampleIdx = 0;
    s->limG = 1.0f;
    s->boxRecalc = 0;
}

/* Full (re)initialisation. Safe to call on a zeroed or garbage block. */
static inline void lv_init(LvState* s, const LvParams* p, float fs, int nch) {
    memset(s, 0, sizeof(*s));
    s->p = *p;
    s->fs = fs;
    s->nch = nch;
    s->ready = (fs >= 8000.0f && fs <= 384000.0f && nch >= 1 && nch <= LV_MAXCH);
    if (!s->ready) return;
    s->blockSec = (double)LV_BLOCK / fs;
    lv_kweight(&s->k1, &s->k2, fs);
    s->aF = exp(-s->blockSec / 0.05);
    s->aS = exp(-s->blockSec / 0.4);
    s->aM = exp(-s->blockSec / 3.0);
    s->aForget = exp(-s->blockSec / (p->forgetSec > 1.0f ? p->forgetSec : 1.0f));
    s->gDb = 0.0f;
    s->gLinPrev = 1.0f;
    s->inGap = 1;               /* treat voice start like a song start */
    int la = (int)(p->lookaheadMs * fs / 1000.0f + 0.5f);
    if (la < 1) la = 1;
    if (la > LV_MAXLA - 1) la = LV_MAXLA - 1;
    s->la = p->limiterOn ? la : 0;
    s->relCoef = (float)(1.0 - exp(-1.0 / (fs * (p->releaseMs > 1.0f ? p->releaseMs : 1.0f) / 1000.0)));
    s->ceilLin = powf(10.0f, p->ceilingDb / 20.0f);
    if (s->la) lv_reset_limiter(s);
    s->tmShort = s->tmSong = -200.0f;
    s->tmMinLimDb = 0.0f;
    s->tmPrevSong = -200.0f;
    s->songMinLim = 1.0f;
    s->wet = s->wetTarget = 1.0f;
}

/* Apply new parameters to a running instance without losing its measurements.
 * A lookahead or limiter on/off change needs a full re-init (returns 0 then). */
static inline int lv_update_params(LvState* s, const LvParams* p) {
    int la = (int)(p->lookaheadMs * s->fs / 1000.0f + 0.5f);
    if (la < 1) la = 1;
    if (la > LV_MAXLA - 1) la = LV_MAXLA - 1;
    if (!p->limiterOn) la = 0;
    if (la != s->la) return 0;
    s->p = *p;
    s->aForget = exp(-s->blockSec / (p->forgetSec > 1.0f ? p->forgetSec : 1.0f));
    s->relCoef = (float)(1.0 - exp(-1.0 / (s->fs * (p->releaseMs > 1.0f ? p->releaseMs : 1.0f) / 1000.0)));
    s->ceilLin = powf(10.0f, p->ceilingDb / 20.0f);
    return 1;
}

static inline float lv_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Start measuring a new song; the old song's estimate becomes the prior. */
static inline void lv_begin_song(LvState* s, unsigned int reason) {
    s->tmPrevSong = s->haveEst ? (float)s->estDb : -200.0f;
    s->tmPrevGain = s->gDb;
    s->tmPrevMinLim = 20.0f * log10f(s->songMinLim > 1e-6f ? s->songMinLim : 1e-6f);
    s->songMinLim = 1.0f;
    s->tmReason = reason;
    if (s->haveEst) { s->priorDb = s->estDb; s->havePrior = 1; }
    s->cumP = 0.0; s->cumN = 0.0; s->tSong = 0.0; s->jumpT = 0.0;
    s->tmSongs++;
}

/* Host hook: the sound player switched to the next song (natural end or skip). */
static inline void lv_song_changed(LvState* s) { s->pendingSong = 1; s->playerTracked = 1; }

/* Host hook: the host can see the sound player, so song changes will be reported exactly;
   turns off the silence / level-jump guessing (which can split a song at a quiet passage). */
static inline void lv_set_player_tracked(LvState* s) { s->playerTracked = 1; }

/* Host hook: start a fresh instance from the loudness of the last song heard on any radio. */
static inline void lv_seed(LvState* s, float priorDb) {
    if (!s->ready) return;
    s->priorDb = priorDb; s->havePrior = 1;
    s->estDb = priorDb; s->haveEst = 1;
    s->gDb = lv_clampf(s->p.targetLufs - priorDb, -s->p.maxCutDb, s->p.maxBoostDb);
    s->gLinPrev = powf(10.0f, s->wet * s->gDb / 20.0f);
    s->seeded = 1;
}

/* Host hook: 1 = apply leveling, 0 = A/B bypass. Switches with a ~15 ms crossfade; the
   detectors keep running so switching back lands on the right gain immediately. */
static inline void lv_set_wet(LvState* s, float target) { s->wetTarget = target > 0.5f ? 1.0f : 0.0f; }

/* Detector + gain computer for one block. Returns the new gain (linear). */
static inline float lv_analyse(LvState* s, float* const* in, int nch) {
    const LvParams* p = &s->p;
    double pb = 0.0;
    for (int c = 0; c < nch; c++) {
        const float* x = in[c];
        double* z = s->kz[c];
        double z0 = z[0], z1 = z[1], z2 = z[2], z3 = z[3];
        double acc = 0.0;
        const LvBiquad a = s->k1, b = s->k2;
        for (int i = 0; i < LV_BLOCK; i++) {
            double v = x[i];
            double y1 = a.b0 * v + z0;
            z0 = a.b1 * v - a.a1 * y1 + z1;
            z1 = a.b2 * v - a.a2 * y1;
            double y2 = b.b0 * y1 + z2;
            z2 = b.b1 * y1 - b.a1 * y2 + z3;
            z3 = b.b2 * y1 - b.a2 * y2;
            acc += y2 * y2;
        }
        if (!(fabs(z0) < 1e6) || !(fabs(z2) < 1e6) || !(acc == acc)) { z0 = z1 = z2 = z3 = 0.0; acc = 0.0; }
        if (fabs(z0) < 1e-25) z0 = 0.0;
        if (fabs(z1) < 1e-25) z1 = 0.0;
        if (fabs(z2) < 1e-25) z2 = 0.0;
        if (fabs(z3) < 1e-25) z3 = 0.0;
        z[0] = z0; z[1] = z1; z[2] = z2; z[3] = z3;
        pb += acc / LV_BLOCK;
    }

    const double T = s->blockSec;
    s->pF = s->aF * s->pF + (1.0 - s->aF) * pb;
    s->pS = s->aS * s->pS + (1.0 - s->aS) * pb;
    s->pM = s->aM * s->pM + (1.0 - s->aM) * pb;
    const double LS = lv_lufs(s->pS);

    if (s->pendingSong) {
        /* exact song change reported by the host (sound player switched sample) */
        s->pendingSong = 0;
        s->inGap = 0;
        s->silenceT = 0.0;
        lv_begin_song(s, 3);
    }

    /* gap between songs (EA tracks end with 0.4-5 s of near silence); only used while the
       host has not shown that it can report song changes itself */
    const double LF = lv_lufs(s->pF);
    if (LF < p->silenceLufs) {
        s->silenceT += T * 1000.0;
        if (s->silenceT >= p->silenceMs && !s->playerTracked) s->inGap = 1;
    } else {
        s->silenceT = 0.0;
    }

    int valid = (LS > p->gateLufs) && (!s->haveEst || LS > s->estDb - p->relGateDb);
    /* while in a gap nothing is measured; the new song starts once audio is clearly back */
    if (s->inGap && LF < p->silenceLufs + 6.0) valid = 0;

    if (valid && s->inGap) {
        /* first real audio after a gap: a new song. Keep the old estimate as a prior. */
        s->inGap = 0;
        lv_begin_song(s, 1);
    }
    if (valid) {
        s->cumP = s->cumP * s->aForget + pb;
        s->cumN = s->cumN * s->aForget + 1.0;
        s->tSong += T;
        double cumDb = lv_lufs(s->cumP / s->cumN);
        if (s->havePrior) {
            double w0 = cumDb > s->priorDb ? p->priorDownSec : p->priorUpSec;
            s->estDb = (w0 * s->priorDb + s->tSong * cumDb) / (w0 + s->tSong);
        } else {
            s->estDb = cumDb;
        }
        s->haveEst = 1;

        /* sustained jump without a gap (crossfade, medley, segue) */
        double LM = lv_lufs(s->pM);
        if (!s->playerTracked && s->tSong > 15.0 && fabs(LM - s->estDb) > p->jumpDb) {
            s->jumpT += T;
            if (s->jumpT >= p->jumpSec) {
                lv_begin_song(s, 2);
                s->cumP = s->pM; s->cumN = 1.0; s->tSong = T;
                s->estDb = s->priorDb;
            }
        } else {
            s->jumpT = 0.0;
        }
    }

    if (s->haveEst) {
        float target = lv_clampf((float)(p->targetLufs - s->estDb), -p->maxCutDb, p->maxBoostDb);
        float up = p->upDbps * (float)T;
        float dn = p->downDbps * (float)T;
        float d = target - s->gDb;
        if (d > up) d = up;
        if (d < -dn) d = -dn;
        s->gDb += d;
    }

    s->tmShort = (float)LS;
    s->tmSong = s->haveEst ? (float)s->estDb : -200.0f;
    s->tmGainDb = s->gDb;
    return powf(10.0f, s->gDb / 20.0f);
}

/* in[c] / out[c]: LV_BLOCK frames each. in and out may alias. */
static inline void lv_process(LvState* s, float* const* in, float* const* out, int nch) {
    if (!s->ready || nch != s->nch) {
        for (int c = 0; c < nch; c++) if (in[c] != out[c]) memcpy(out[c], in[c], LV_BLOCK * sizeof(float));
        return;
    }
    lv_analyse(s, in, nch);
    /* A/B crossfade: wet moves 1/3 per block (about 16 ms at 48 kHz) */
    const float w0 = s->wet;
    float w1 = w0;
    if (w1 < s->wetTarget) { w1 += 1.0f / 3.0f; if (w1 > s->wetTarget) w1 = s->wetTarget; }
    else if (w1 > s->wetTarget) { w1 -= 1.0f / 3.0f; if (w1 < s->wetTarget) w1 = s->wetTarget; }
    s->wet = w1;
    float g1 = w1 >= 1.0f ? powf(10.0f, s->gDb / 20.0f) : (w1 <= 0.0f ? 1.0f : powf(10.0f, w1 * s->gDb / 20.0f));
    float g0 = s->gLinPrev;
    float dg = (g1 - g0) / LV_BLOCK;
    s->gLinPrev = g1;
    const float dw = (w1 - w0) / LV_BLOCK;

    if (!s->la) {
        for (int c = 0; c < nch; c++) {
            const float* x = in[c]; float* y = out[c]; float g = g0;
            for (int i = 0; i < LV_BLOCK; i++) { g += dg; y[i] = x[i] * g; }
        }
        s->tmLimDb = 0.0f;
        s->tmBlocks++;
        return;
    }

    const int la = s->la;
    const float ceil = s->ceilLin;
    const float rel = s->relCoef;
    const double invLa = 1.0 / la;
    float minLim = 1.0f;
    float g = g0;
    float w = w0;
    for (int i = 0; i < LV_BLOCK; i++) {
        g += dg;
        w += dw;
        /* gain-adjusted input sample, peak across channels */
        float pk = 0.0f;
        for (int c = 0; c < nch; c++) {
            float v = in[c][i] * g;
            if (!(v == v)) v = 0.0f;              /* NaN guard */
            float a = fabsf(v);
            if (a > pk) pk = a;
            /* stash into the delay line slot we are about to read */
            float outv = s->dly[c][s->dpos];
            s->dly[c][s->dpos] = v;
            /* write later once gain is known: keep outv in out buffer temporarily */
            out[c][i] = outv;
        }
        float r = pk > ceil ? ceil / pk : 1.0f;

        /* sliding minimum over the last la+1 required gains */
        unsigned int n = s->sampleIdx++;
        while (s->dqTail != s->dqHead) {
            int last = (s->dqTail + LV_MAXLA + 1) % (LV_MAXLA + 2);
            if (s->dqVal[last] >= r) s->dqTail = last; else break;
        }
        s->dqIdx[s->dqTail] = (int)n;
        s->dqVal[s->dqTail] = r;
        s->dqTail = (s->dqTail + 1) % (LV_MAXLA + 2);
        while ((unsigned int)(n - (unsigned int)s->dqIdx[s->dqHead]) > (unsigned int)la)
            s->dqHead = (s->dqHead + 1) % (LV_MAXLA + 2);
        float m = s->dqVal[s->dqHead];

        /* box average of the sliding minimum over la samples */
        s->boxSum += (double)m - (double)s->rq[s->rqpos];
        s->rq[s->rqpos] = m;
        s->rqpos++; if (s->rqpos >= la) s->rqpos = 0;
        float att = (float)(s->boxSum * invLa);
        if (att > 1.0f) att = 1.0f;

        /* release */
        float lg = s->limG + (1.0f - s->limG) * rel;
        if (att < lg) lg = att;
        s->limG = lg;
        if (lg < minLim) minLim = lg;

        /* in bypass the limiter keeps tracking but is not applied */
        float la_g = (w >= 1.0f) ? lg : (w <= 0.0f ? 1.0f : 1.0f + w * (lg - 1.0f));
        for (int c = 0; c < nch; c++) out[c][i] *= la_g;
        s->dpos++; if (s->dpos >= la) s->dpos = 0;
    }
    /* periodic exact recompute of the running box sum (float drift) */
    if (++s->boxRecalc >= 512) {
        double sum = 0.0;
        for (int k = 0; k < la; k++) sum += s->rq[k];
        s->boxSum = sum;
        s->boxRecalc = 0;
    }
    s->tmLimDb = 20.0f * log10f(minLim > 1e-6f ? minLim : 1e-6f);
    if (s->tmLimDb < s->tmMinLimDb) s->tmMinLimDb = s->tmLimDb;
    if (minLim < s->songMinLim) s->songMinLim = minLim;
    s->tmBlocks++;
}

#endif
