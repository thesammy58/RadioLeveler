// SPDX-License-Identifier: MIT
// Copyright (c) 2026 thesammy58
// Written with Claude, Anthropic's AI model; see "How this was made" in README.md.
/*
 * RadioLeveler.asi - automatic per-song loudness leveling for The Sims 3 radio.
 *
 * How it works
 *   RadioLeveler.package edits the game's "stereo" voice template (the DSP chain every
 *   radio / stereo / jukebox song plays through) to include RWAudioCore's Compressor1
 *   plug-in ("Co10"). Nothing in the vanilla game uses Co10, and with its default
 *   threshold (120 dB) it is a no-op, so the package alone changes nothing.
 *
 *   This ASI finds Co10's plug-in descriptor in memory and swaps three function pointers
 *   (GetSize / Init / Process) so every Co10 instance runs the leveler in leveler.h
 *   instead. No game code is patched. Only voices built from the edited template are
 *   affected, and the stereo's volume setting and distance falloff are applied after it.
 *
 * RWAudioCore ABI used (recovered from the EA App build, same RWAC library in all builds):
 *   descriptor  +0x00 name  +0x04 GetSize(spec*)  +0x08 Init(inst, param)
 *               +0x10 Process(inst, ctx)  +0x24 next  +0x28 four-cc id
 *   instance    +0x21 u8 channel count, plug-in private data after the header
 *   ctx         +0x3000C current buffer*, +0x30010 scratch buffer*, +0x30018 -> +0x0C float rate
 *   buffer      +0x04 float* data, +0x0E u16 channel stride (floats); 256 frames per call;
 *               a plug-in writes the scratch buffer and swaps the two pointers.
 *   instance    +0x08 owning voice, +0x10 descriptor
 *   voice       +0x44 u8 plug-in count, +0x4C plug-in instance pointers in chain order
 *   SndPlayer1  +0x1AC handle of the sample now playing, +0x1C9 u8 index of the current
 *               request in its queue. Either changing = the playlist moved to the next song
 *               (natural end or skip), which is how song changes are detected exactly.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include "leveler.h"

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#endif

#define RL_VERSION "1.3.0"

namespace {

constexpr uint32_t kCo10 = 0x436F3130;        /* 'Co10' as stored by RWAC */
constexpr uint32_t kSnP1 = 0x536E5031;        /* 'SnP1' */
constexpr uint32_t kMagic = 0x524C5631;       /* 'RLV1' */
constexpr int kSlots = 32;

typedef unsigned int (__cdecl* GetSizeFn)(void* spec);
typedef unsigned char (__cdecl* InitFn)(void* inst, void* param);
typedef int (__cdecl* ProcessFn)(void* inst, void* ctx);

struct RwBuffer {             /* only the fields we touch */
    uint32_t pad0;
    float* data;              /* +0x04 */
    uint8_t pad1[6];
    uint16_t stride;          /* +0x0E */
};

struct alignas(16) InstExt {
    uint32_t magic;
    uint32_t gen;
    uint32_t slot;
    uint32_t inited;
    uint32_t blocks;
    uint32_t serial;          /* voice number shown in the log */
    uint32_t lastSongs;
    char*    player;          /* SndPlayer1 instance of the same voice, or null */
    uint32_t playerChecked;
    uint32_t playerHandle;
    uint32_t playerIdx;
    LvState st;
};

/* Written by the mixer thread, read by the log thread (benign races, plain floats/ints). */
struct Slot {
    volatile DWORD tick;
    volatile uint32_t serial;
    volatile float fs, shortL, songL, gainDb, limDb, minLimDb;
    volatile float prevSong, prevGain, prevMinLim, songSec;
    volatile uint32_t nch, songs, reason, tracked, seeded, bypassed;
};

char g_dir[MAX_PATH];
char g_iniPath[MAX_PATH];
char g_logPath[MAX_PATH];
CRITICAL_SECTION g_logLock;

GetSizeFn g_origGetSize = nullptr;
InitFn g_origInit = nullptr;
ProcessFn g_origProcess = nullptr;
unsigned int g_extOffset = 0;
void** g_snp1Desc = nullptr;
volatile float g_lastSongDb = 0.0f;   /* last settled song loudness on any radio (seeds new voices) */
volatile DWORD g_lastSongTick = 0;

LvParams g_params[2];
volatile LONG g_paramIdx = 0;
volatile LONG g_paramGen = 1;
volatile LONG g_enabled = 1;
volatile LONG g_slotNext = 0;
volatile LONG g_initCount = 0;
Slot g_slot[kSlots];
int g_logInterval = 30;
int g_logSongs = 1;
volatile LONG g_logging = 1;          /* Logging= in the ini: 0 = never write RadioLeveler.log */
constexpr DWORD kLogMaxBytes = 256u * 1024u;   /* the log starts over past this size */
volatile LONG g_bypass = 0;           /* A/B hotkey state: 1 = leveling bypassed */
volatile LONG g_hkVk = 'L';           /* toggle hotkey virtual key, 0 = none */
volatile LONG g_hkMods = 3;           /* bit0 ctrl, bit1 shift, bit2 alt */
char g_hkText[64] = "Ctrl+Shift+L";

void Log(const char* fmt, ...) {
    if (!g_logging) return;
    char line[1024];
    SYSTEMTIME t; GetLocalTime(&t);
    int n = _snprintf(line, sizeof(line) - 3, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt);
    int m = _vsnprintf(line + n, sizeof(line) - 3 - n, fmt, ap);
    va_end(ap);
    if (m < 0) m = (int)sizeof(line) - 3 - n;
    n += m; line[n++] = '\r'; line[n++] = '\n'; line[n] = 0;
    EnterCriticalSection(&g_logLock);
    HANDLE h = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size;
    if (h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &size) && size.QuadPart > kLogMaxBytes) {
        CloseHandle(h);
        h = CreateFileA(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, line, (DWORD)n, &w, nullptr); CloseHandle(h); }
    LeaveCriticalSection(&g_logLock);
}

/* ---------------------------------------------------------------- config */

const char kDefaultIni[] =
"; RadioLeveler settings. Changes are picked up while the game runs (within ~2 s).\r\n"
"; Every value below is the shipped default; delete a line to go back to it.\r\n"
"[RadioLeveler]\r\n"
"; 1 = level radio songs, 0 = pass audio through untouched\r\n"
"Enabled=1\r\n"
"\r\n"
"; How hard songs are pulled to the same loudness:\r\n"
";   standard - tightest; also evens out loud and quiet sections within a song\r\n"
";   relaxed  - songs still close, more of each song's own loud/quiet shape kept\r\n"
";   light    - mostly fixes song-to-song jumps, leaves songs alone otherwise\r\n"
";   subtle   - lightest touch\r\n"
"Strength=standard\r\n"
"\r\n"
"; Loudness every song is steered to, before the stereo's own volume setting.\r\n"
"; Raise it (e.g. -14) for louder radio overall, lower it (e.g. -18) for quieter.\r\n"
"TargetLUFS=-16\r\n"
"\r\n"
"; Most a quiet song is turned up / a loud song is turned down, in dB\r\n"
"MaxBoostDB=10\r\n"
"MaxCutDB=12\r\n"
"\r\n"
"; Peak ceiling of the built-in limiter (dBFS, before the stereo volume). 0 disables it.\r\n"
"LimiterCeilingDB=-1\r\n"
"\r\n"
"; A/B hotkey: switches leveling off/on instantly while the game has focus, so you can compare.\r\n"
"; Ctrl/Shift/Alt + a letter, digit, F1-F24, Insert, Home, End, PageUp, PageDown, Delete,\r\n"
"; Pause or ScrollLock. Use None to disable.\r\n"
"ToggleHotkey=Ctrl+Shift+L\r\n"
"\r\n"
"; 1 = write RadioLeveler.log next to the .asi, 0 = don't write a log at all\r\n"
"Logging=1\r\n"
"; Log a line when a song starts/ends (1 = on, 0 = off)\r\n"
"LogSongs=1\r\n"
"; Also log a status line per playing radio every N seconds (0 = off)\r\n"
"LogIntervalSec=30\r\n"
"\r\n"
"; Advanced: override single values of the Strength preset (values shown are 'standard')\r\n"
";PriorUpSec=1.5\r\n"
";PriorDownSec=0.5\r\n"
";UpDbPerSec=6\r\n"
";DownDbPerSec=24\r\n"
";ForgetSec=8\r\n"
";GateLUFS=-50\r\n"
";RelGateDB=20\r\n"
";SilenceLUFS=-45\r\n"
";SilenceMs=250\r\n"
";JumpDB=6\r\n"
";JumpSec=4\r\n"
";LookaheadMs=3\r\n"
";ReleaseMs=120\r\n";

bool IniStr(const char* key, char* out, DWORD n) {
    out[0] = 0;
    GetPrivateProfileStringA("RadioLeveler", key, "", out, n, g_iniPath);
    char* c = strchr(out, ';'); if (c) *c = 0;
    size_t len = strlen(out);
    while (len && (out[len - 1] == ' ' || out[len - 1] == '\t')) out[--len] = 0;
    return out[0] != 0;
}
void IniFloat(const char* key, float* v) {
    char b[64];
    if (IniStr(key, b, sizeof(b))) { char* e; double d = strtod(b, &e); if (e != b) *v = (float)d; }
}

/* "Ctrl+Shift+L" -> vk + modifier mask. Returns false if it does not parse. */
bool ParseHotkey(const char* text, LONG* vk, LONG* mods) {
    char b[64]; strncpy(b, text, sizeof(b) - 1); b[sizeof(b) - 1] = 0;
    LONG v = 0, m = 0;
    if (!_stricmp(b, "none") || !b[0]) { *vk = 0; *mods = 0; return true; }
    for (char* tok = strtok(b, "+ "); tok; tok = strtok(nullptr, "+ ")) {
        if (!_stricmp(tok, "ctrl") || !_stricmp(tok, "control")) m |= 1;
        else if (!_stricmp(tok, "shift")) m |= 2;
        else if (!_stricmp(tok, "alt")) m |= 4;
        else if (strlen(tok) == 1 && isalnum((unsigned char)tok[0])) v = toupper((unsigned char)tok[0]);
        else if ((tok[0] == 'F' || tok[0] == 'f') && atoi(tok + 1) >= 1 && atoi(tok + 1) <= 24) v = VK_F1 + atoi(tok + 1) - 1;
        else if (!_stricmp(tok, "insert")) v = VK_INSERT;
        else if (!_stricmp(tok, "delete")) v = VK_DELETE;
        else if (!_stricmp(tok, "home")) v = VK_HOME;
        else if (!_stricmp(tok, "end")) v = VK_END;
        else if (!_stricmp(tok, "pageup")) v = VK_PRIOR;
        else if (!_stricmp(tok, "pagedown")) v = VK_NEXT;
        else if (!_stricmp(tok, "pause")) v = VK_PAUSE;
        else if (!_stricmp(tok, "scrolllock")) v = VK_SCROLL;
        else return false;
    }
    if (!v) return false;
    *vk = v; *mods = m;
    return true;
}

void LoadConfig(bool first) {
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileA(g_iniPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, kDefaultIni, sizeof(kDefaultIni) - 1, &w, nullptr); CloseHandle(h); }
        Log("Created default %s", g_iniPath);
    }
    LvParams p; lv_default_params(&p);
    char b[64];
    /* lv_default_params() is the "standard" preset. Pre-1.3 names still work:
       max = standard, strong = relaxed, balanced = light, gentle = subtle. */
    const char* strength = "standard";
    if (IniStr("Strength", b, sizeof(b))) {
        if (!_stricmp(b, "relaxed") || !_stricmp(b, "strong")) {
            strength = "relaxed"; p.priorUpSec = 3; p.priorDownSec = 1; p.upDbps = 4; p.downDbps = 20; p.forgetSec = 20;
        } else if (!_stricmp(b, "light") || !_stricmp(b, "balanced")) {
            strength = "light"; p.priorUpSec = 10; p.priorDownSec = 2; p.upDbps = 1.5f; p.downDbps = 6; p.forgetSec = 60;
        } else if (!_stricmp(b, "subtle") || !_stricmp(b, "gentle")) {
            strength = "subtle"; p.priorUpSec = 15; p.priorDownSec = 2; p.upDbps = 1.0f; p.downDbps = 6; p.forgetSec = 120;
        } else if (_stricmp(b, "standard") != 0 && _stricmp(b, "max") != 0) {
            Log("Strength=%s not understood, using standard", b);
        }
    }
    IniFloat("TargetLUFS", &p.targetLufs);
    IniFloat("MaxBoostDB", &p.maxBoostDb);
    IniFloat("MaxCutDB", &p.maxCutDb);
    float ceil = p.ceilingDb; IniFloat("LimiterCeilingDB", &ceil);
    if (ceil >= 0.0f) p.limiterOn = 0; else p.ceilingDb = ceil;
    IniFloat("PriorUpSec", &p.priorUpSec);
    IniFloat("PriorDownSec", &p.priorDownSec);
    IniFloat("UpDbPerSec", &p.upDbps);
    IniFloat("DownDbPerSec", &p.downDbps);
    IniFloat("ForgetSec", &p.forgetSec);
    IniFloat("GateLUFS", &p.gateLufs);
    IniFloat("RelGateDB", &p.relGateDb);
    IniFloat("SilenceLUFS", &p.silenceLufs);
    IniFloat("SilenceMs", &p.silenceMs);
    IniFloat("JumpDB", &p.jumpDb);
    IniFloat("JumpSec", &p.jumpSec);
    IniFloat("LookaheadMs", &p.lookaheadMs);
    IniFloat("ReleaseMs", &p.releaseMs);
    /* sanity */
    p.maxBoostDb = lv_clampf(p.maxBoostDb, 0.0f, 24.0f);
    p.maxCutDb = lv_clampf(p.maxCutDb, 0.0f, 40.0f);
    p.targetLufs = lv_clampf(p.targetLufs, -40.0f, -3.0f);
    p.upDbps = lv_clampf(p.upDbps, 0.01f, 100.0f);
    p.downDbps = lv_clampf(p.downDbps, 0.01f, 100.0f);
    p.lookaheadMs = lv_clampf(p.lookaheadMs, 0.1f, 5.0f);

    float en = 1; IniFloat("Enabled", &en);
    float li = 30; IniFloat("LogIntervalSec", &li);
    g_logInterval = (int)li;
    float ls = 1; IniFloat("LogSongs", &ls);
    g_logSongs = ls != 0;
    float lg = 1; IniFloat("Logging", &lg);
    InterlockedExchange(&g_logging, lg != 0 ? 1 : 0);
    char hk[64];
    if (!IniStr("ToggleHotkey", hk, sizeof(hk))) strcpy(hk, "Ctrl+Shift+L");
    LONG vk, mods;
    if (ParseHotkey(hk, &vk, &mods)) {
        InterlockedExchange(&g_hkVk, vk); InterlockedExchange(&g_hkMods, mods);
        const char* t = vk ? hk : "none";
        size_t n = strlen(t); if (n > sizeof(g_hkText) - 1) n = sizeof(g_hkText) - 1;
        memcpy(g_hkText, t, n); g_hkText[n] = 0;
    } else {
        Log("ToggleHotkey=%s not understood, keeping %s", hk, g_hkText);
    }

    LONG next = 1 - g_paramIdx;
    g_params[next] = p;
    InterlockedExchange(&g_paramIdx, next);
    InterlockedIncrement(&g_paramGen);
    InterlockedExchange(&g_enabled, en != 0 ? 1 : 0);
    Log("%s config: Enabled=%d Strength=%s Target=%.1f LUFS Boost<=%.1f dB Cut<=%.1f dB Limiter=%s%.1f dBFS A/B hotkey=%s",
        first ? "Loaded" : "Reloaded", en != 0, strength, p.targetLufs, p.maxBoostDb, p.maxCutDb,
        p.limiterOn ? "" : "off/", p.ceilingDb, g_hkText);
}

/* ------------------------------------------------------ plug-in overrides */

inline InstExt* Ext(void* inst) { return (InstExt*)((char*)inst + g_extOffset); }

bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    const char* a = (const char*)p;
    while (n) {
        if (!VirtualQuery(a, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return false;
        if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        size_t avail = (size_t)((const char*)mi.BaseAddress + mi.RegionSize - a);
        if (avail >= n) return true;
        a += avail; n -= avail;
    }
    return true;
}

/* The SndPlayer1 that feeds this Compressor1: first plug-in of the same voice. Checked once per
   voice, every assumption verified; any mismatch means "no player tracking" (silence fallback). */
char* FindPlayer(void* inst) {
    if (!g_snp1Desc) return nullptr;
    char* voice = *(char**)((char*)inst + 0x08);
    if (!voice || !Readable(voice, 0x4C + 4 * 32)) return nullptr;
    unsigned cnt = *(uint8_t*)(voice + 0x44);
    if (cnt < 2 || cnt > 32) return nullptr;
    void** list = (void**)(voice + 0x4C);
    bool self = false;
    for (unsigned i = 0; i < cnt; i++) if (list[i] == inst) { self = true; break; }
    if (!self) return nullptr;
    char* p0 = (char*)list[0];
    if (!p0 || !Readable(p0, 0x1D0)) return nullptr;
    if (*(void***)(p0 + 0x10) != g_snp1Desc) return nullptr;
    return p0;
}

unsigned int __cdecl RL_GetSize(void* /*spec*/) {
    return g_extOffset + (unsigned int)sizeof(InstExt);
}

unsigned char __cdecl RL_Init(void* inst, void* param) {
    unsigned char ok = g_origInit(inst, param);
    InstExt* e = Ext(inst);
    e->magic = kMagic;
    e->inited = 0;
    e->blocks = 0;
    e->gen = 0;
    e->serial = (uint32_t)InterlockedIncrement(&g_slotNext);
    e->slot = (e->serial - 1) % kSlots;
    e->lastSongs = 0;
    e->player = nullptr;
    e->playerChecked = 0;
    InterlockedIncrement(&g_initCount);
    return ok;
}

int __cdecl RL_Process(void* inst, void* ctx) {
    InstExt* e = Ext(inst);
    char* c = (char*)ctx;
    RwBuffer** curp = (RwBuffer**)(c + 0x3000C);
    RwBuffer** scrp = (RwBuffer**)(c + 0x30010);
    RwBuffer* cur = *curp;
    RwBuffer* scr = *scrp;
    char* rateObj = *(char**)(c + 0x30018);
    int nch = *((uint8_t*)inst + 0x21);

    if (!g_enabled || e->magic != kMagic || !cur || !scr || !cur->data || !scr->data || !rateObj ||
        nch < 1 || nch > LV_MAXCH || cur->stride < LV_BLOCK || scr->stride < LV_BLOCK) {
        return g_origProcess(inst, ctx);      /* stock Co10 at its defaults = untouched audio */
    }
    float fs = *(float*)(rateObj + 0x0C);

    LONG gen = g_paramGen;
    if (!e->inited || e->st.fs != fs || e->st.nch != nch) {
        lv_init(&e->st, &g_params[g_paramIdx], fs, nch);
        if (!e->inited && g_lastSongTick && GetTickCount() - g_lastSongTick < 10u * 60u * 1000u)
            lv_seed(&e->st, g_lastSongDb);
        e->inited = 1;
        e->gen = (uint32_t)gen;
    } else if (e->gen != (uint32_t)gen) {
        if (!lv_update_params(&e->st, &g_params[g_paramIdx])) lv_init(&e->st, &g_params[g_paramIdx], fs, nch);
        e->gen = (uint32_t)gen;
    }

    /* exact song changes from the voice's SndPlayer1 */
    if (!e->playerChecked) {
        e->playerChecked = 1;
        e->player = FindPlayer(inst);
        if (e->player) {
            e->playerHandle = *(uint32_t*)(e->player + 0x1AC);
            e->playerIdx = *(uint8_t*)(e->player + 0x1C9);
            lv_set_player_tracked(&e->st);
        }
    } else if (e->player) {
        uint32_t h = *(uint32_t*)(e->player + 0x1AC);
        uint32_t ix = *(uint8_t*)(e->player + 0x1C9);
        if (h != e->playerHandle || ix != e->playerIdx) {
            bool first = e->playerHandle == 0;      /* player just started its first request */
            e->playerHandle = h; e->playerIdx = ix;
            if (!first && h != 0) lv_song_changed(&e->st);
        }
    }

    lv_set_wet(&e->st, g_bypass ? 0.0f : 1.0f);

    float* in[LV_MAXCH];
    float* out[LV_MAXCH];
    for (int ch = 0; ch < nch; ch++) {
        in[ch] = cur->data + (size_t)ch * cur->stride;
        out[ch] = scr->data + (size_t)ch * scr->stride;
    }
    lv_process(&e->st, in, out, nch);
    *curp = scr;
    *scrp = cur;

    if ((++e->blocks & 127) == 0 || e->st.tmSongs != e->lastSongs) {
        Slot& s = g_slot[e->slot];
        e->lastSongs = e->st.tmSongs;
        s.serial = e->serial;
        s.fs = fs; s.nch = (uint32_t)nch;
        s.shortL = e->st.tmShort; s.songL = e->st.tmSong; s.gainDb = e->st.tmGainDb;
        s.limDb = e->st.tmLimDb; s.minLimDb = e->st.tmMinLimDb;
        s.prevSong = e->st.tmPrevSong; s.prevGain = e->st.tmPrevGain; s.prevMinLim = e->st.tmPrevMinLim;
        s.reason = e->st.tmReason;
        s.tracked = e->player ? 1u : 0u;
        s.seeded = (uint32_t)e->st.seeded;
        s.bypassed = g_bypass ? 1u : 0u;
        s.songSec = (float)e->st.tSong;
        s.songs = e->st.tmSongs;
        DWORD now = GetTickCount();
        s.tick = now;
        if (e->st.haveEst && e->st.tSong > 10.0) { g_lastSongDb = (float)e->st.estDb; g_lastSongTick = now; }
    }
    return 1;
}

/* ------------------------------------------------------- descriptor hunt */

struct Range { uintptr_t lo, hi; };

bool InRanges(const Range* r, int n, uintptr_t p) {
    for (int i = 0; i < n; i++) if (p >= r[i].lo && p < r[i].hi) return true;
    return false;
}

void** FindDescriptor(uint32_t id, const char* wantName, uintptr_t base, Range* code, int ncode, Range* data, int ndata, uintptr_t imgEnd) {
    size_t nlen = strlen(wantName) + 1;
    for (int d = 0; d < ndata; d++) {
        for (uintptr_t p = (data[d].lo + 0x28 + 3) & ~(uintptr_t)3; p + 4 <= data[d].hi; p += 4) {
            if (*(uint32_t*)p != id) continue;
            void** desc = (void**)(p - 0x28);
            uintptr_t name = (uintptr_t)desc[0];
            if (name < base || name + nlen > imgEnd) continue;
            if (memcmp((const void*)name, wantName, nlen) != 0) continue;
            if (!InRanges(code, ncode, (uintptr_t)desc[1]) || !InRanges(code, ncode, (uintptr_t)desc[2]) ||
                !InRanges(code, ncode, (uintptr_t)desc[4])) continue;
            return desc;
        }
    }
    return nullptr;
}

bool InstallHooks() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    uintptr_t imgEnd = base + nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    Range code[8], data[8]; int ncode = 0, ndata = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uintptr_t lo = base + sec[i].VirtualAddress;
        uintptr_t hi = lo + (sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData);
        if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) && ncode < 8) code[ncode++] = {lo, hi};
        else if ((sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) && ndata < 8) data[ndata++] = {lo, hi};
    }

    /* the EA build decrypts itself at startup: keep looking until the descriptor is readable */
    void** desc = nullptr;
    for (int tries = 0; tries < 1800 && !desc; tries++) {
        desc = FindDescriptor(kCo10, "Compressor1", base, code, ncode, data, ndata, imgEnd);
        if (!desc) Sleep(100);
    }
    if (!desc) { Log("ERROR: Compressor1 (Co10) plug-in descriptor not found - unsupported game build?"); return false; }

    g_origGetSize = (GetSizeFn)desc[1];
    g_origInit = (InitFn)desc[2];
    g_origProcess = (ProcessFn)desc[4];
    unsigned int origSize = g_origGetSize(nullptr);
    if (origSize < 0x28 || origSize > 0x4000) { Log("ERROR: unexpected Co10 instance size 0x%X - not patching", origSize); return false; }
    g_extOffset = (origSize + 15u) & ~15u;

    DWORD old;
    if (!VirtualProtect(&desc[1], sizeof(void*) * 4, PAGE_READWRITE, &old)) { Log("ERROR: VirtualProtect failed (%lu)", GetLastError()); return false; }
    InterlockedExchange((volatile LONG*)&desc[1], (LONG)(uintptr_t)&RL_GetSize);
    InterlockedExchange((volatile LONG*)&desc[2], (LONG)(uintptr_t)&RL_Init);
    InterlockedExchange((volatile LONG*)&desc[4], (LONG)(uintptr_t)&RL_Process);
    VirtualProtect(&desc[1], sizeof(void*) * 4, old, &old);
    Log("Hooked Compressor1 descriptor at 0x%08X (orig size 0x%X, leveler state %u bytes per voice)",
        (unsigned)(uintptr_t)desc, origSize, (unsigned)sizeof(InstExt));
    g_snp1Desc = FindDescriptor(kSnP1, "SndPlayer1", base, code, ncode, data, ndata, imgEnd);
    if (g_snp1Desc) Log("Found SndPlayer1 descriptor at 0x%08X: song changes and skips are tracked exactly", (unsigned)(uintptr_t)g_snp1Desc);
    else Log("SndPlayer1 descriptor not found: song changes are detected from the silence between songs");
    return true;
}

/* ------------------------------------------------------------ threads */

FILETIME IniTime() {
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILETIME z = {0, 0};
    return GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &a) ? a.ftLastWriteTime : z;
}

bool GameHasFocus() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI HotkeyThread(LPVOID) {
    bool wasDown = false;
    for (;;) {
        Sleep(25);
        LONG vk = g_hkVk, mods = g_hkMods;
        if (!vk) { wasDown = false; continue; }
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool combo = down && ctrl == ((mods & 1) != 0) && shift == ((mods & 2) != 0) && alt == ((mods & 4) != 0);
        if (combo && !wasDown && GameHasFocus()) {
            LONG now = !g_bypass;
            InterlockedExchange(&g_bypass, now);
            char what[256]; int n = 0; what[0] = 0;
            DWORD t = GetTickCount();
            for (int i = 0; i < kSlots && n < 200; i++) {
                Slot& s = g_slot[i];
                if (!s.tick || t - s.tick > 3000) continue;
                n += _snprintf(what + n, sizeof(what) - n, "%sradio #%u %+.1f dB", n ? ", " : "", s.serial, s.gainDb);
            }
            what[sizeof(what) - 1] = 0;
            if (!n) strcpy(what, "no radio playing");
            Log(now ? "A/B: leveling OFF - original game levels (difference you should hear: %s)"
                    : "A/B: leveling ON (applying: %s)", what);
        }
        wasDown = down;
    }
}

DWORD WINAPI MainThread(LPVOID) {
    g_logging = GetPrivateProfileIntA("RadioLeveler", "Logging", 1, g_iniPath) != 0;
    Log("RadioLeveler %s starting", RL_VERSION);
    LoadConfig(true);
    if (!InstallHooks()) return 0;
    HANDLE hk = CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
    if (hk) CloseHandle(hk);

    FILETIME ft = IniTime();
    DWORD lastStatus = GetTickCount();
    bool announcedNone = false;
    uint32_t seenSerial[kSlots] = {}, seenSongs[kSlots] = {};
    bool seenAlive[kSlots] = {};
    float lastSong[kSlots] = {}, lastGain[kSlots] = {}, lastSec[kSlots] = {};
    bool lastByp[kSlots] = {};
    for (;;) {
        Sleep(1000);
        FILETIME nt = IniTime();
        if (CompareFileTime(&nt, &ft) != 0) { ft = nt; Sleep(200); LoadConfig(false); }

        DWORD now = GetTickCount();
        int active = 0;
        for (int i = 0; i < kSlots; i++) {
            Slot& s = g_slot[i];
            if (!s.tick) continue;
            bool alive = now - s.tick < 3000;
            uint32_t serial = s.serial;
            if (seenAlive[i] && (!alive || serial != seenSerial[i])) {
                if (g_logSongs) {
                    if (lastSec[i] < 8.0f)
                        Log("radio #%u off | song played %.0f s, too short to measure", seenSerial[i], lastSec[i]);
                    else if (lastByp[i])
                        Log("radio #%u off | song measured %.1f LUFS, played unleveled (A/B off)", seenSerial[i], lastSong[i]);
                    else
                        Log("radio #%u off | song measured %.1f LUFS -> played at %.1f LUFS (gain %+.1f dB)",
                            seenSerial[i], lastSong[i], lastSong[i] + lastGain[i], lastGain[i]);
                }
                seenAlive[i] = false;
            }
            if (!alive) continue;
            active++;
            if (!seenAlive[i]) {
                seenAlive[i] = true; seenSerial[i] = serial; seenSongs[i] = s.songs;
                if (g_logSongs) {
                    char how[96];
                    if (s.seeded) _snprintf(how, sizeof(how), "starting at %+.1f dB, carried over from the last song", s.gainDb);
                    else _snprintf(how, sizeof(how), "starting at %+.1f dB (no earlier song to carry over)", s.gainDb);
                    how[sizeof(how) - 1] = 0;
                    Log("radio #%u on (%uch %.0f Hz) | %s%s", serial, s.nch, s.fs, how,
                        s.tracked ? "" : " | song changes from silence detection (player not found)");
                }
            }
            uint32_t songs = s.songs;
            if (songs != seenSongs[i]) {
                if (g_logSongs && songs > 1 && s.prevSong > -100.0f) {
                    char lim[48] = "";
                    if (s.prevMinLim < -1.0f) _snprintf(lim, sizeof(lim), ", limiter peaked %.1f dB", s.prevMinLim);
                    Log("radio #%u next song (%s) | last song %.1f LUFS -> played at %.1f LUFS (gain %+.1f dB%s)",
                        serial, s.reason == 3 ? "player" : s.reason == 2 ? "level jump" : "after gap",
                        s.prevSong, s.prevSong + s.prevGain, s.prevGain, lim);
                }
                seenSongs[i] = songs;
            }
            lastSec[i] = s.songSec; lastByp[i] = s.bypassed != 0;
            lastSong[i] = s.songL; lastGain[i] = s.gainDb;
        }

        if (g_logInterval > 0 && now - lastStatus >= (DWORD)g_logInterval * 1000u) {
            lastStatus = now;
            for (int i = 0; i < kSlots; i++) {
                Slot& s = g_slot[i];
                if (!s.tick || now - s.tick > 3000) continue;
                Log("radio #%u status: now %.1f LUFS | song %.1f LUFS | gain %+.1f dB%s | limiter %.1f dB (worst %.1f)",
                    s.serial, s.shortL, s.songL, s.gainDb, s.bypassed ? " (A/B off)" : "", s.limDb, s.minLimDb);
            }
            if (!active && !announcedNone) {
                Log(g_initCount ? "No radio playing right now." :
                    "No leveled radio voice has been created yet. If a stereo is playing, check that RadioLeveler.package is in Mods\\Packages.");
                announcedNone = true;
            }
        }
        if (active) announcedNone = false;
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        const char* name = strrchr(exe, '\\'); name = name ? name + 1 : exe;
        if (_stricmp(name, "TS3.exe") != 0 && _stricmp(name, "TS3W.exe") != 0) return TRUE;
        InitializeCriticalSection(&g_logLock);
        GetModuleFileNameA(mod, g_dir, MAX_PATH);
        char* slash = strrchr(g_dir, '\\'); if (slash) slash[1] = 0;
        _snprintf(g_iniPath, MAX_PATH, "%sRadioLeveler.ini", g_dir);
        _snprintf(g_logPath, MAX_PATH, "%sRadioLeveler.log", g_dir);
        DeleteFileA(g_logPath);
        lv_default_params(&g_params[0]);
        g_params[1] = g_params[0];
        HANDLE t = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
