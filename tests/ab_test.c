// SPDX-License-Identifier: MIT
// Copyright (c) 2026 thesammy58
// Written with Claude, Anthropic's AI model; see "How this was made" in README.md.
#include <stdio.h>
#include <stdlib.h>
#include "../src/leveler.h"
/* A/B bypass: wet=0 must be the input delayed by the lookahead, bit for bit; switching must not click. */
int main(void) {
    LvParams p; lv_default_params(&p);
    static LvState s; lv_init(&s, &p, 48000, 2);
    enum { NB = 48000 / 256 * 40 };
    static float inL[NB * 256], outL[NB * 256];
    unsigned int r = 7;
    for (long i = 0; i < NB * 256L; i++) { r = r * 1664525u + 1013904223u; double t = i / 48000.0;
        double amp = t < 20 ? 0.08 : 0.4;   /* quiet song then loud song -> big gain swing */
        inL[i] = (float)(amp * (0.7 * sin(2 * M_PI * 220 * t) + 0.3 * (((r >> 9) / 8388608.0) * 2 - 1))); }
    static float bl[256], br[256], ol[256], orr[256];
    float* in[2] = {bl, br}; float* out[2] = {ol, orr};
    long bypassBlocks = 0, exact = 0; int sinceRamp = 99; double maxStepNorm = 0, maxStepSwitch = 0;
    for (long b = 0; b < NB; b++) {
        double t = b * 256.0 / 48000.0;
        int bypass = ((int)(t / 1.5)) % 2;            /* toggle every 1.5 s */
        lv_set_wet(&s, bypass ? 0.0f : 1.0f);
        for (int i = 0; i < 256; i++) bl[i] = br[i] = inL[b * 256 + i];
        float wBefore = s.wet;
        lv_process(&s, in, out, 2);
        memcpy(&outL[b * 256], ol, sizeof(ol));
        int ramp = s.wet != wBefore || (s.wet > 0 && s.wet < 1);
        sinceRamp = (s.wet != wBefore) ? 0 : sinceRamp + 1;
        if (s.wet == 0.0f && wBefore == 0.0f && sinceRamp >= 2) {   /* steady bypass, crossfade tail flushed from the 3 ms delay */
            bypassBlocks++;
            int ok = 1;
            for (int i = 0; i < 256; i++) { long src = b * 256 + i - s.la; float want = src >= 0 ? inL[src] : 0.0f; if (ol[i] != want) ok = 0; }
            exact += ok;
        }
        for (int i = 1; i < 256; i++) { double d = fabs(ol[i] - ol[i - 1]); if (ramp) { if (d > maxStepSwitch) maxStepSwitch = d; } else if (d > maxStepNorm) maxStepNorm = d; }
    }
    printf("steady bypass blocks bit-exact (input delayed %d samples): %ld / %ld\n", s.la, exact, bypassBlocks);
    printf("largest sample-to-sample step: normal %.4f, during A/B crossfades %.4f\n", maxStepNorm, maxStepSwitch);
    return !(exact == bypassBlocks && maxStepSwitch <= maxStepNorm * 1.05);
}
