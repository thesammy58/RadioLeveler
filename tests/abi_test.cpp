// SPDX-License-Identifier: MIT
// Copyright (c) 2026 thesammy58
// Written with Claude, Anthropic's AI model; see "How this was made" in README.md.
/* abi_test.cpp - build as TS3.exe (the ASI only wakes up inside TS3.exe / TS3W.exe) and run it
 * on Windows or Wine:  TS3.exe [path\to\RadioLeveler.asi]   (default: next to TS3.exe)
 * tests\abi_test.vcxproj builds it with Visual Studio. It exits non-zero on failure.
 * Emulates the RWAudioCore side of the Compressor1 plug-in ABI: a fake Co10 descriptor in
 * .data, instance allocation through GetSize/Init, and Process calls with the ping-pong
 * buffer context. Checks that the ASI hooks the descriptor, that its output matches a
 * reference run of leveler.h bit-for-bit, and that Enabled=0 passes audio through. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <malloc.h>
#include "../src/leveler.h"

typedef unsigned int (__cdecl* GetSizeFn)(void*);
typedef unsigned char (__cdecl* InitFn)(void*, void*);
typedef int (__cdecl* ProcessFn)(void*, void*);

struct FakeDesc {
    const char* name; void* getSize; void* init; void* opt; void* process;
    void* attrs; void* events; void* u1c; void* u20; void* next; uint32_t id; uint32_t flags[2];
};
static_assert(offsetof(FakeDesc, id) == 0x28, "layout");

static const char kName[] = "Compressor1";
static unsigned int __cdecl StockGetSize(void*) { return 0xC0; }
static unsigned char __cdecl StockInit(void* inst, void*) {
    float* a = (float*)((char*)inst + 0x28);
    a[0] = 120.0f; a[2] = 1.0f; a[4] = 0.1f; a[6] = 0.5f; a[8] = 1.0f;
    return 1;
}
static volatile int g_stockCalls = 0;
static int __cdecl StockProcess(void*, void*) { g_stockCalls++; return 1; }   /* bypass: no swap */

FakeDesc g_desc = { kName, (void*)&StockGetSize, (void*)&StockInit, nullptr, (void*)&StockProcess,
                    nullptr, nullptr, nullptr, nullptr, nullptr, 0x436F3130, {0x00050004, 0x000B0000} };
static const char kSnpName[] = "SndPlayer1";
static unsigned int __cdecl SnpGetSize(void*) { return 0x200; }
static unsigned char __cdecl SnpInit(void*, void*) { return 1; }
static int __cdecl SnpProcess(void*, void*) { return 1; }
FakeDesc g_snpDesc = { kSnpName, (void*)&SnpGetSize, (void*)&SnpInit, nullptr, (void*)&SnpProcess,
                       nullptr, nullptr, nullptr, nullptr, nullptr, 0x536E5031, {0x06030100, 0x00080100} };

struct RwBuffer { uint32_t pad0; float* data; uint8_t pad1[6]; uint16_t stride; };
static_assert(offsetof(RwBuffer, stride) == 0x0E, "layout");

static float noise() { static uint32_t s = 12345; s = s * 1664525u + 1013904223u; return ((s >> 9) * (1.0f / 8388608.0f)) * 2.0f - 1.0f; }

/* synthetic programme: loud "song", gap, quiet "song", gap, mid "song" */
static void make_block(long blk, float* l, float* r) {
    double t0 = blk * 256.0 / 44100.0;
    for (int i = 0; i < 256; i++) {
        double t = t0 + i / 44100.0; float amp;
        if (t < 30) amp = 0.5f; else if (t < 31) amp = 0; else if (t < 61) amp = 0.06f; else if (t < 62) amp = 0; else amp = 0.18f;
        float s = (float)(amp * (0.6 * sin(2 * M_PI * 110 * t) + 0.4 * noise()));
        l[i] = s; r[i] = s * 0.9f;
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int fails = 0;
    char asi[MAX_PATH];
    if (argc > 1) GetFullPathNameA(argv[1], MAX_PATH, asi, nullptr);
    else { GetModuleFileNameA(nullptr, asi, MAX_PATH); strcpy(strrchr(asi, '\\') + 1, "RadioLeveler.asi"); }
    char ini[MAX_PATH]; strcpy(ini, asi); strcpy(strrchr(ini, '\\') + 1, "RadioLeveler.ini");   /* the ASI reads the ini next to itself */
    DeleteFileA(ini);
    if (!LoadLibraryA(asi)) { printf("FAIL load %s (%lu)\n", asi, GetLastError()); return 1; }
    for (int i = 0; i < 100 && g_desc.process == (void*)&StockProcess; i++) Sleep(100);
    if (g_desc.process == (void*)&StockProcess) { printf("FAIL: descriptor not hooked\n"); return 1; }
    printf("OK hooked: getSize %p init %p process %p\n", g_desc.getSize, g_desc.init, g_desc.process);

    unsigned int size = ((GetSizeFn)g_desc.getSize)(nullptr);
    printf("OK instance size %u (stock 192)\n", size);
    char* inst = (char*)_aligned_malloc(size, 16);
    memset(inst, 0xCD, size);
    inst[0x21] = 2;
    /* fake voice: [SndPlayer1, Compressor1, Send], as RWAC lays it out */
    static char snp[0x200] = {}; *(void**)(snp + 0x10) = &g_snpDesc; *(uint32_t*)(snp + 0x1AC) = 0x1111; snp[0x1C9] = 0;
    static char voice[0x4C + 4 * 8] = {}; voice[0x44] = 3;
    ((void**)(voice + 0x4C))[0] = snp; ((void**)(voice + 0x4C))[1] = inst; ((void**)(voice + 0x4C))[2] = nullptr;
    *(void**)(inst + 0x08) = voice;
    *(void**)(inst + 0x10) = &g_desc;
    if (!((InitFn)g_desc.init)(inst, nullptr)) { printf("FAIL init\n"); return 1; }

    char* ctx = (char*)calloc(1, 0x30020);
    static float dA[2 * 256], dB[2 * 256];
    RwBuffer A = {}, B = {}; A.data = dA; A.stride = 256; B.data = dB; B.stride = 256;
    char rate[16] = {}; *(float*)(rate + 0x0C) = 44100.0f;
    *(RwBuffer**)(ctx + 0x3000C) = &A; *(RwBuffer**)(ctx + 0x30010) = &B; *(char**)(ctx + 0x30018) = rate;

    LvParams p; lv_default_params(&p);
    static LvState ref; lv_init(&ref, &p, 44100.0f, 2); lv_set_player_tracked(&ref);   /* ASI found the fake player */
    static float rl[256], rr[256], ol[256], orr[256];
    float* rin[2] = {rl, rr}; float* rout[2] = {ol, orr};

    double maxDiff = 0; long blocks = (long)(92.0 * 44100 / 256);
    double segPow[3] = {0}, segIn[3] = {0}; long segN[3] = {0};
    for (long b = 0; b < blocks; b++) {
        RwBuffer* cur = *(RwBuffer**)(ctx + 0x3000C);
        make_block(b, cur->data, cur->data + cur->stride);
        memcpy(rl, cur->data, sizeof(rl)); memcpy(rr, cur->data + cur->stride, sizeof(rr));
        /* skips: the player switches sample mid-song at 15 s and 45 s, then advances its queue at 75 s */
        long b15 = (long)(15.0 * 44100 / 256), b45 = (long)(45.0 * 44100 / 256), b75 = (long)(75.0 * 44100 / 256);
        if (b == b15) { *(uint32_t*)(snp + 0x1AC) = 0x2222; lv_song_changed(&ref); }
        if (b == b45) { *(uint32_t*)(snp + 0x1AC) = 0x3333; lv_song_changed(&ref); }
        if (b == b75) { snp[0x1C9] = 1; lv_song_changed(&ref); }
        int ret = ((ProcessFn)g_desc.process)(inst, ctx);
        RwBuffer* now = *(RwBuffer**)(ctx + 0x3000C);
        if (ret != 1 || now == cur) { printf("FAIL: process ret %d, swapped %d\n", ret, now != cur); return 1; }
        lv_process(&ref, rin, rout, 2);
        for (int i = 0; i < 256; i++) {
            double d1 = fabs(now->data[i] - ol[i]), d2 = fabs(now->data[now->stride + i] - orr[i]);
            if (d1 > maxDiff) maxDiff = d1; if (d2 > maxDiff) maxDiff = d2;
        }
        double t = b * 256.0 / 44100.0; int seg = t > 15 && t < 30 ? 0 : t > 46 && t < 61 ? 1 : t > 77 ? 2 : -1;
        if (seg >= 0) for (int i = 0; i < 256; i++) { segPow[seg] += now->data[i] * now->data[i]; segIn[seg] += rl[i] * rl[i]; segN[seg]++; }
    }
    if (maxDiff >= 1e-6) fails++;
    printf("%s output vs reference (with 3 player song changes): max |diff| = %g, reference songs %u\n",
           maxDiff == 0 ? "OK" : (maxDiff < 1e-6 ? "OK~" : "FAIL"), maxDiff, ref.tmSongs);
    for (int s = 0; s < 3; s++)
        printf("   segment %d: in %.1f dBFS rms -> out %.1f dBFS rms\n", s, 10 * log10(segIn[s] / segN[s]), 10 * log10(segPow[s] / segN[s]));

    /* Enabled=0 must pass through untouched (stock Co10 path, no swap) */
    WritePrivateProfileStringA("RadioLeveler", "Enabled", "0", ini);
    Sleep(4500);
    int before = g_stockCalls; double pd = 0;
    for (long b = 0; b < 200; b++) {
        RwBuffer* cur = *(RwBuffer**)(ctx + 0x3000C);
        make_block(b, cur->data, cur->data + cur->stride);
        memcpy(rl, cur->data, sizeof(rl));
        ((ProcessFn)g_desc.process)(inst, ctx);
        RwBuffer* now = *(RwBuffer**)(ctx + 0x3000C);
        for (int i = 0; i < 256; i++) { double d = fabs(now->data[i] - rl[i]); if (d > pd) pd = d; }
    }
    bool passOk = g_stockCalls - before == 200 && pd == 0;
    if (!passOk) fails++;
    printf("%s Enabled=0 passthrough: stock calls %d, max |out-in| = %g\n", passOk ? "OK" : "FAIL", g_stockCalls - before, pd);
    WritePrivateProfileStringA("RadioLeveler", "Enabled", "1", ini);
    Sleep(2500);
    printf(fails ? "FAILED (%d)\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
