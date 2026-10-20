// SPDX-License-Identifier: MIT
// Copyright (c) 2026 thesammy58
// Written with Claude, Anthropic's AI model; see "How this was made" in README.md.
/* bench.c - CPU cost of the leveler: 10 minutes of 48 kHz stereo noise through lv_process(). */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../src/leveler.h"

int main(void) {
    LvParams p; lv_default_params(&p);
    static LvState s; lv_init(&s, &p, 48000, 2);
    static float a[2][LV_BLOCK], b[2][LV_BLOCK];
    float* in[2] = {a[0], a[1]};
    float* out[2] = {b[0], b[1]};
    const double seconds = 600.0;
    const long blocks = (long)(48000 / LV_BLOCK * seconds);
    srand(1);
    clock_t t0 = clock();
    for (long k = 0; k < blocks; k++) {
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < LV_BLOCK; i++) a[c][i] = ((rand() % 2000) - 1000) * 0.0003f;
        lv_process(&s, in, out, 2);
    }
    double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("10 min of 48 kHz stereo in %.3f s (including noise generation) = %.4f%% of one core\n",
           sec, sec / seconds * 100.0);
    return 0;
}
