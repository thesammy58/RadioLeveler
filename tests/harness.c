// SPDX-License-Identifier: MIT
// Copyright (c) 2026 thesammy58
// Written with Claude, Anthropic's AI model; see "How this was made" in README.md.
/* Offline harness: feeds a continuous stereo stream (songs + gaps) through the
 * leveler exactly as the game would (256-frame planar blocks) and measures
 * BS.1770 loudness of each song before and after. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "../src/leveler.h"

#define FS 44100.0
#define NCH 2
#define MAXSONGS 2048

typedef struct { LvBiquad a, b; double z[NCH][4]; } Meter;
typedef struct { int id; long start, end; double *sub; int nsub, cap; double peak; double subacc; int subn; double gs, gss; int gn; float gmin, gmax; } SongAcc;

static void meter_init(Meter* m) { memset(m, 0, sizeof(*m)); lv_kweight(&m->a, &m->b, FS); }
static double meter_sample(Meter* m, const float* fr) {   /* returns sum of K-weighted squares over channels */
    double s = 0;
    for (int c = 0; c < NCH; c++) {
        double* z = m->z[c]; double v = fr[c];
        double y1 = m->a.b0 * v + z[0]; z[0] = m->a.b1 * v - m->a.a1 * y1 + z[1]; z[1] = m->a.b2 * v - m->a.a2 * y1;
        double y2 = m->b.b0 * y1 + z[2]; z[2] = m->b.b1 * y1 - m->b.a1 * y2 + z[3]; z[3] = m->b.b2 * y1 - m->b.a2 * y2;
        s += y2 * y2;
    }
    return s;
}
static SongAcc songsIn[MAXSONGS], songsOut[MAXSONGS];
static int nsongs = 0;
static Meter mIn, mOut;
static int curIn = -1, curOut = -1;   /* index of song covering current sample */

static void acc_push(SongAcc* a, double sq, float pk) {
    a->subacc += sq; a->subn++;
    if (pk > a->peak) a->peak = pk;
    if (a->subn == (int)(FS / 10)) {  /* 100 ms sub-blocks */
        if (a->nsub == a->cap) { a->cap = a->cap ? a->cap * 2 : 4096; a->sub = realloc(a->sub, a->cap * sizeof(double)); }
        a->sub[a->nsub++] = a->subacc / a->subn;
        a->subacc = 0; a->subn = 0;
    }
}
/* BS.1770-4 integrated loudness over sub-blocks [s0, s1) */
static double integrated(const double* sub, int s0, int s1) {
    int nb = (s1 - s0) - 3; if (nb <= 0) return -200;
    double* bl = malloc(nb * sizeof(double)); int n = 0;
    for (int i = 0; i < nb; i++) { double p = (sub[s0+i]+sub[s0+i+1]+sub[s0+i+2]+sub[s0+i+3]) / 4; bl[n++] = p; }
    double sum = 0; int cnt = 0;
    for (int i = 0; i < n; i++) if (lv_lufs(bl[i]) > -70) { sum += bl[i]; cnt++; }
    if (!cnt) { free(bl); return -200; }
    double rel = lv_lufs(sum / cnt) - 10; sum = 0; cnt = 0;
    for (int i = 0; i < n; i++) if (lv_lufs(bl[i]) > -70 && lv_lufs(bl[i]) > rel) { sum += bl[i]; cnt++; }
    free(bl); return cnt ? lv_lufs(sum / cnt) : -200;
}
static int dcmp(const void* a, const void* b) { double x = *(double*)a, y = *(double*)b; return x < y ? -1 : x > y; }
/* EBU Tech 3342 loudness range (3 s windows, 1 s hop on 100 ms grid) */
static double lra(const double* sub, int n) {
    int nb = n - 29; if (nb <= 0) return 0;
    double* v = malloc(nb * sizeof(double)); int k = 0;
    for (int i = 0; i < nb; i += 10) { double p = 0; for (int j = 0; j < 30; j++) p += sub[i+j]; p /= 30; if (lv_lufs(p) > -70) v[k++] = p; }
    if (k < 2) { free(v); return 0; }
    double sum = 0; for (int i = 0; i < k; i++) sum += v[i];
    double rel = lv_lufs(sum / k) - 20; int m = 0;
    for (int i = 0; i < k; i++) if (lv_lufs(v[i]) > rel) v[m++] = lv_lufs(v[i]);
    if (m < 2) { free(v); return 0; }
    qsort(v, m, sizeof(double), dcmp);
    double r = v[(int)(0.95 * (m - 1))] - v[(int)(0.10 * (m - 1))];
    free(v); return r;
}

static LvState st;
static float blk[NCH][LV_BLOCK], oblk[NCH][LV_BLOCK];
static int bfill = 0;
static long inPos = 0, outPos = 0;   /* stream sample indices */
static int la = 0;

static void feed_out(const float* fr) {
    /* output sample at outPos corresponds to input sample outPos - la */
    long src = outPos - la;
    while (curOut + 1 < nsongs && src >= songsOut[curOut + 1].start) curOut++;
    double sq = meter_sample(&mOut, fr);
    if (curOut >= 0 && src < songsOut[curOut].end) {
        float pk = fabsf(fr[0]) > fabsf(fr[1]) ? fabsf(fr[0]) : fabsf(fr[1]);
        acc_push(&songsOut[curOut], sq, pk);
    }
    outPos++;
}
static int usePlayer = 0, playerMark = 0;
static void run_block(void) {
    float* in[NCH]; float* out[NCH];
    if (playerMark) { lv_song_changed(&st); playerMark = 0; }
    for (int c = 0; c < NCH; c++) { in[c] = blk[c]; out[c] = oblk[c]; }
    lv_process(&st, in, out, NCH);
    if (curIn >= 0 && curIn < nsongs) {
        SongAcc* a = &songsOut[curIn];
        double tIn = (double)(inPos - a->start) / FS;
        float g = st.gDb;
        if (a->gn == 0 && tIn < 1.0) { a->gmin = a->gmax = g; }
        if (tIn > 15.0 && inPos <= a->end) { a->gs += g; a->gss += (double)g * g; a->gn++; if (g < a->gmin) a->gmin = g; if (g > a->gmax) a->gmax = g; }
    }
    for (int i = 0; i < LV_BLOCK; i++) { float fr[NCH]; for (int c = 0; c < NCH; c++) fr[c] = oblk[c][i]; feed_out(fr); }
    bfill = 0;
}
static void push_frame(const float* fr) {
    while (curIn + 1 < nsongs && inPos >= songsIn[curIn + 1].start) { curIn++; if (usePlayer && curIn > 0) playerMark = 1; }
    double sq = meter_sample(&mIn, fr);
    if (curIn >= 0 && inPos < songsIn[curIn].end) {
        float pk = fabsf(fr[0]) > fabsf(fr[1]) ? fabsf(fr[0]) : fabsf(fr[1]);
        acc_push(&songsIn[curIn], sq, pk);
    }
    for (int c = 0; c < NCH; c++) blk[c][bfill] = fr[c];
    inPos++;
    if (++bfill == LV_BLOCK) run_block();
}

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    LvParams p; lv_default_params(&p);
    for (int i = 1; i + 1 < argc; i += 2) {   /* --key value overrides */
        const char* k = argv[i]; float v = (float)atof(argv[i+1]);
#define P(name) if (!strcmp(k, "--" #name)) p.name = v;
        P(targetLufs) P(maxBoostDb) P(maxCutDb) P(gateLufs) P(relGateDb) P(silenceLufs) P(silenceMs)
        P(priorUpSec) P(priorDownSec) P(forgetSec) P(upDbps) P(downDbps)
        P(jumpDb) P(jumpSec) P(ceilingDb) P(lookaheadMs) P(releaseMs)
        if (!strcmp(k, "--limiterOn")) p.limiterOn = (int)v;
        if (!strcmp(k, "--player")) usePlayer = (int)v;
        if (!strcmp(k, "--bypass") && v != 0) { p.maxBoostDb = 0; p.maxCutDb = 0; p.limiterOn = 0; }
    }
    lv_init(&st, &p, (float)FS, NCH);
    la = st.la;
    meter_init(&mIn); meter_init(&mOut);
    char tag[4]; int32_t hdr[2];
    static float buf[NCH * 65536];
    while (fread(tag, 1, 4, stdin) == 4 && fread(hdr, 4, 2, stdin) == 2) {
        int id = hdr[0]; long nf = hdr[1];
        if (!memcmp(tag, "END ", 4)) break;
        int isSong = !memcmp(tag, "SONG", 4);
        if (isSong) {
            SongAcc* a = &songsIn[nsongs]; memset(a, 0, sizeof(*a)); a->id = id; a->start = inPos; a->end = inPos + nf;
            songsOut[nsongs] = *a; nsongs++;
        }
        long left = nf;
        while (left > 0) {
            long n = left > 65536 ? 65536 : left;
            if (isSong) { if (fread(buf, sizeof(float) * NCH, n, stdin) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; } }
            else memset(buf, 0, sizeof(float) * NCH * n);
            for (long i = 0; i < n; i++) push_frame(&buf[i * NCH]);
            left -= n;
        }
    }
    /* flush limiter tail */
    float z[NCH] = {0};
    for (int i = 0; i < la + 2 * LV_BLOCK; i++) push_frame(z);
    for (int s = 0; s < nsongs; s++) {
        SongAcc* a = &songsIn[s]; SongAcc* b = &songsOut[s];
        int f15a = a->nsub < 150 ? a->nsub : 150, f15b = b->nsub < 150 ? b->nsub : 150;
        double gm = b->gn ? b->gs / b->gn : 0, gsd = b->gn ? sqrt(fmax(0, b->gss / b->gn - gm * gm)) : 0;
        printf("%d %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n", a->id,
            integrated(a->sub, 0, a->nsub), integrated(b->sub, 0, b->nsub),
            integrated(a->sub, 0, f15a), integrated(b->sub, 0, f15b),
            lra(a->sub, a->nsub), lra(b->sub, b->nsub),
            20 * log10(a->peak + 1e-12), 20 * log10(b->peak + 1e-12), gsd, b->gn ? b->gmax - b->gmin : 0);
    }
    fprintf(stderr, "songs detected by leveler: %u (stream had %d)\n", st.tmSongs, nsongs);
    return 0;
}
