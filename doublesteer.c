/*
 * NFS Underground 1 - Keyboard Double Steering ASI Plugin
 * ========================================================
 * Self-calibrating — finds the steering float automatically.
 * Works on any game version.
 *
 * Build (mingw-w64, 32-bit):
 *   i686-w64-mingw32-gcc -shared -o nfs_ds.asi doublesteer.c
 *       -static-libgcc -O2 -lwinmm
 */

#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════ */

static float g_partial       = 0.45f;
static int   g_interval      = 8;
static int   g_duration      = 2;
static int   g_enabled       = 1;

/* Double steering state */
static int   g_frame         = 0;
static int   g_prevDir       = 0;

/* Calibration */
#define MAX_CAND 16384

static DWORD *g_cand         = NULL;
static int    g_numCand      = 0;
static float *g_steerPtr     = NULL;
static int    g_calibState   = 0;
static int    g_calibDir     = 0;
static BOOL   g_prevLeft     = FALSE;
static BOOL   g_prevRight    = FALSE;
static int    g_validFrames  = 0;
static int    g_invalidCount = 0;

/* ═══════════════════════════════════════════════════════════════
 * Config
 * ═══════════════════════════════════════════════════════════════ */

static void load_config(void)
{
    char ini[MAX_PATH];
    GetModuleFileNameA(NULL, ini, MAX_PATH);
    char *p = strrchr(ini, '\\');
    if (p) *(p + 1) = '\0';
    strcat(ini, "nfs_ds.ini");

    int pct = GetPrivateProfileIntA("DoubleSteering", "DeflectionPercent", 45, ini);
    if (pct < 5)  pct = 5;
    if (pct > 95) pct = 95;
    g_partial = pct / 100.0f;

    g_enabled = GetPrivateProfileIntA("DoubleSteering", "Enabled", 1, ini);

    g_interval = GetPrivateProfileIntA("DoubleSteering", "PulseInterval", 8, ini);
    if (g_interval < 2)  g_interval = 2;
    if (g_interval > 60) g_interval = 60;

    g_duration = GetPrivateProfileIntA("DoubleSteering", "PulseDuration", 2, ini);
    if (g_duration < 1) g_duration = 1;
    if (g_duration >= g_interval) g_duration = g_interval - 1;
}

/* ═══════════════════════════════════════════════════════════════
 * Memory scanning
 * ═══════════════════════════════════════════════════════════════ */

static BOOL page_readable(DWORD addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((PVOID)addr, &mbi, sizeof(mbi)))
        return FALSE;
    if (mbi.State != MEM_COMMIT)
        return FALSE;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return FALSE;
    DWORD ok = PAGE_READONLY | PAGE_READWRITE |
               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
               PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & ok) != 0;
}

static void scan_for_float(float target)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD addr = 0x10000;

    g_numCand = 0;

    while (addr < 0x7FFE0000 && g_numCand < MAX_CAND) {
        if (VirtualQuery((PVOID)addr, &mbi, sizeof(mbi)) == 0) {
            addr += 0x1000;
            continue;
        }

        DWORD regionEnd = (DWORD)mbi.BaseAddress + (DWORD)mbi.RegionSize;

        if (mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE |
                            PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
        {
            DWORD base = (DWORD)mbi.BaseAddress;
            DWORD end  = regionEnd - 3;

            if (base & 3) base = (base + 3) & ~3;

            for (DWORD a = base; a <= end && g_numCand < MAX_CAND; a += 4) {
                float val = *(float *)a;
                if (fabsf(val - target) < 0.001f) {
                    g_cand[g_numCand++] = a;
                }
            }
        }

        addr = regionEnd;
    }
}

static void filter_candidates(float target)
{
    int kept = 0;
    for (int i = 0; i < g_numCand; i++) {
        if (page_readable(g_cand[i])) {
            float val = *(float *)g_cand[i];
            if (fabsf(val - target) < 0.001f) {
                g_cand[kept++] = g_cand[i];
            }
        }
    }
    g_numCand = kept;
}

static BOOL validate_steering_value(float val)
{
    return (fabsf(val) < 0.001f ||
            fabsf(val - 1.0f) < 0.001f ||
            fabsf(val + 1.0f) < 0.001f);
}

/* ═══════════════════════════════════════════════════════════════
 * Calibration state machine
 * ═══════════════════════════════════════════════════════════════ */

static void calibration_tick(BOOL leftHeld, BOOL rightHeld)
{
    BOOL steering    = leftHeld || rightHeld;
    BOOL wasSteering = g_prevLeft || g_prevRight;
    BOOL justPressed  = steering && !wasSteering;
    BOOL justReleased = !steering && wasSteering;

    switch (g_calibState) {

    case 0:
        if (justPressed) {
            g_calibDir = leftHeld ? -1 : 1;
            Sleep(80);
            float target = (g_calibDir < 0) ? -1.0f : 1.0f;
            scan_for_float(target);
            if (g_numCand > 0 && g_numCand < MAX_CAND)
                g_calibState = 1;
        }
        break;

    case 1:
        if (justReleased) {
            Sleep(80);
            filter_candidates(0.0f);
            if (g_numCand == 0) {
                g_calibState = 0;
            } else if (g_numCand <= 3) {
                g_steerPtr = (float *)g_cand[0];
                g_calibState = 3;
                g_validFrames = 0;
            } else {
                g_calibState = 2;
            }
        }
        break;

    case 2:
        if (justPressed) {
            g_calibDir = leftHeld ? -1 : 1;
            Sleep(80);
            float target = (g_calibDir < 0) ? -1.0f : 1.0f;
            filter_candidates(target);
            if (g_numCand == 0) {
                g_calibState = 0;
            } else if (g_numCand <= 5) {
                g_steerPtr = (float *)g_cand[0];
                g_calibState = 3;
                g_validFrames = 0;
            } else {
                g_calibState = 1;
            }
        }
        break;

    case 3:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Double steering application
 * ═══════════════════════════════════════════════════════════════ */

static void apply_double_steering(void)
{
    if (!g_steerPtr) return;

    if (!page_readable((DWORD)g_steerPtr)) {
        g_invalidCount++;
        if (g_invalidCount > 100) {
            g_steerPtr = NULL;
            g_calibState = 0;
            g_invalidCount = 0;
        }
        return;
    }
    g_invalidCount = 0;

    float raw = *g_steerPtr;

    if (!validate_steering_value(raw)) {
        g_validFrames = 0;
        return;
    }

    if (fabsf(raw) < 0.001f) {
        g_frame = 0;
        g_prevDir = 0;
        g_validFrames++;
        return;
    }

    g_validFrames++;

    if (g_validFrames < 30) return;

    int dir = (raw < 0.0f) ? -1 : 1;

    if (dir != g_prevDir) {
        g_frame = 0;
        g_prevDir = dir;
    }

    int pos = g_frame % g_interval;
    float direction = (dir < 0) ? -1.0f : 1.0f;

    if (pos < g_duration) {
        *g_steerPtr = direction * 1.0f;
    } else {
        *g_steerPtr = direction * g_partial;
    }

    g_frame++;
}

/* ═══════════════════════════════════════════════════════════════
 * Main polling thread
 * ═══════════════════════════════════════════════════════════════ */

static volatile BOOL g_running = TRUE;

static DWORD WINAPI ds_thread(LPVOID param)
{
    (void)param;

    timeBeginPeriod(1);
    Sleep(5000);

    g_cand = (DWORD *)VirtualAlloc(NULL, MAX_CAND * sizeof(DWORD),
                                    MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
    if (!g_cand) {
        timeEndPeriod(1);
        return 1;
    }

    while (g_running) {
        BOOL leftHeld  = (GetAsyncKeyState(VK_LEFT)  & 0x8000) != 0;
        BOOL rightHeld = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;

        if (g_calibState < 3)
            calibration_tick(leftHeld, rightHeld);
        else
            apply_double_steering();

        g_prevLeft  = leftHeld;
        g_prevRight = rightHeld;

        Sleep(1);
    }

    timeEndPeriod(1);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * DllMain
 * ═══════════════════════════════════════════════════════════════ */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason,
                    LPVOID lpvReserved)
{
    (void)hinstDLL;
    (void)lpvReserved;

    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        load_config();
        if (!g_enabled) return TRUE;
        CreateThread(NULL, 0, ds_thread, NULL, 0, NULL);
    }

    if (fdwReason == DLL_PROCESS_DETACH) {
        g_running = FALSE;
        Sleep(50);
        if (g_cand) VirtualFree(g_cand, 0, MEM_RELEASE);
    }

    return TRUE;
}
