/*
 * NFS Underground 2 - Keyboard Double Steering ASI Plugin (v2)
 *
 * Build (mingw-w64, 32-bit):
 *   i686-w64-mingw32-gcc -shared -o nfs_ds.asi doublesteer.c
 *       -static-libgcc -O2 -Wl,--enable-stdcall-fixup
 */

#include <windows.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════
 * Game addresses (NFSU1 speed2.exe v1.4)
 * These are immediate operands inside PUSH instructions in .text
 * that supply handler function pointers to the input dispatcher.
 * ═══════════════════════════════════════════════════════════════ */
#define STEER_HANDLER_ADDR1      0x0041E083   /* analog handler  */
#define STEER_HANDLER_ADDR2      0x0041E09A   /* digital handler */
#define REALDRIVER_STEER_OFFSET  0x2C8
#define JOYSTICKTYPE_P1_ADDR     0x00864788   /* 0xFF=none 0=pad 1=wheel */

/* ═══════════════════════════════════════════════════════════════
 * Configuration (loaded from nfs_ds.ini)
 * ═══════════════════════════════════════════════════════════════ */
static float         g_deflection   = 0.45f;  /* partial hold magnitude  */
static int           g_interval     = 8;       /* frames per cycle        */
static int           g_duration     = 2;       /* spike frames per cycle  */
static DWORD         g_keyLeft      = VK_LEFT;
static DWORD         g_keyRight     = VK_RIGHT;
static DWORD         g_keyToggle    = VK_F9;

/* Cross-thread state — marked volatile so the compiler always
 * reloads from memory rather than caching in registers.         */
static volatile int  g_enabled      = 1;

/* Per-frame state (only touched from the game's callback thread) */
static int           g_frame        = 0;
static int           g_prevDir      = 0;

/* Saved originals for clean unhook                               */
static DWORD         g_origHandler1 = 0;
static DWORD         g_origHandler2 = 0;
static volatile BOOL g_hooked       = FALSE;
static HANDLE        g_threadHandle = NULL;
static volatile BOOL g_shutdown     = FALSE;

/* ═══════════════════════════════════════════════════════════════
 * Key name → VK code
 * ═══════════════════════════════════════════════════════════════ */
static DWORD parse_vk(const char *s)
{
    if (!s || !s[0]) return 0;

    /* Single printable character */
    if (!s[1]) {
        if (s[0] >= 'A' && s[0] <= 'Z') return (DWORD)s[0];
        if (s[0] >= 'a' && s[0] <= 'z') return (DWORD)(s[0] - 32);
        if (s[0] >= '0' && s[0] <= '9') return (DWORD)s[0];
    }

    /* Named keys */
    if (!_stricmp(s, "Left"))    return VK_LEFT;
    if (!_stricmp(s, "Right"))   return VK_RIGHT;
    if (!_stricmp(s, "Up"))      return VK_UP;
    if (!_stricmp(s, "Down"))    return VK_DOWN;
    if (!_stricmp(s, "Space"))   return VK_SPACE;
    if (!_stricmp(s, "LShift"))  return VK_LSHIFT;
    if (!_stricmp(s, "RShift"))  return VK_RSHIFT;
    if (!_stricmp(s, "LCtrl"))   return VK_LCONTROL;
    if (!_stricmp(s, "RCtrl"))   return VK_RCONTROL;
    if (!_stricmp(s, "Tab"))     return VK_TAB;

    /* F-keys */
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 24) return (DWORD)(VK_F1 + n - 1);
    }

    /* Hex literal: 0x1B etc. */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (DWORD)strtoul(s, NULL, 16);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * INI loader
 * ═══════════════════════════════════════════════════════════════ */
static void load_config(void)
{
    char ini[MAX_PATH], buf[64];
    DWORD vk;

    GetModuleFileNameA(NULL, ini, MAX_PATH);
    char *p = strrchr(ini, '\\');
    if (p) *(p + 1) = '\0';
    strcat(ini, "nfs_ds.ini");

    /* Deflection */
    int pct = GetPrivateProfileIntA("DoubleSteering",
                                    "DeflectionPercent", 45, ini);
    if (pct < 0)  pct = 0;      /* 0 = release fully (classic DS) */
    if (pct > 95) pct = 95;
    g_deflection = pct / 100.0f;

    /* Enabled */
    g_enabled = GetPrivateProfileIntA("DoubleSteering", "Enabled", 1, ini);

    /* Pulse timing */
    g_interval = GetPrivateProfileIntA("DoubleSteering",
                                       "PulseInterval", 8, ini);
    if (g_interval < 2)  g_interval = 2;
    if (g_interval > 60) g_interval = 60;

    g_duration = GetPrivateProfileIntA("DoubleSteering",
                                       "PulseDuration", 2, ini);
    if (g_duration < 1)              g_duration = 1;
    if (g_duration >= g_interval)    g_duration = g_interval - 1;

    /* Steering keys */
    GetPrivateProfileStringA("DoubleSteering", "KeyLeft",
                             "Left", buf, sizeof(buf), ini);
    vk = parse_vk(buf);
    if (vk) g_keyLeft = vk;

    GetPrivateProfileStringA("DoubleSteering", "KeyRight",
                             "Right", buf, sizeof(buf), ini);
    vk = parse_vk(buf);
    if (vk) g_keyRight = vk;

    /* Toggle hotkey */
    GetPrivateProfileStringA("DoubleSteering", "KeyToggle",
                             "F9", buf, sizeof(buf), ini);
    vk = parse_vk(buf);
    if (vk) g_keyToggle = vk;
}

/* ═══════════════════════════════════════════════════════════════
 * Memory helpers
 * ═══════════════════════════════════════════════════════════════ */
static BOOL write_mem(DWORD addr, const void *data, size_t len)
{
    DWORD old;
    if (!VirtualProtect((LPVOID)addr, len,
                        PAGE_EXECUTE_READWRITE, &old))
        return FALSE;
    memcpy((void *)addr, data, len);
    VirtualProtect((LPVOID)addr, len, old, &old);
    return TRUE;
}

static BOOL read_mem(DWORD addr, void *out, size_t len)
{
    DWORD old;
    if (!VirtualProtect((LPVOID)addr, len,
                        PAGE_EXECUTE_READWRITE, &old))
        return FALSE;
    memcpy(out, (const void *)addr, len);
    VirtualProtect((LPVOID)addr, len, old, &old);
    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════
 * Steering handler — called by the game each frame
 *
 * Reads keyboard directly, alternates partial↔full lock.
 * With wheel mode forced the game applies NO smoothing.
 *
 * void __cdecl handler(int port, int event, char value, void *drv)
 * ═══════════════════════════════════════════════════════════════ */
static void __cdecl DS_SteerHandler(int port, int event,
                                     char value, void *RealDriver)
{
    (void)port; (void)event; (void)value;
    if (!RealDriver) return;

    float *steerVal =
        (float *)((DWORD)RealDriver + REALDRIVER_STEER_OFFSET);

    /* If double-steering is toggled off, let the game coast
     * at zero — the original handler is restored separately. */
    if (!g_enabled) {
        *steerVal = 0.0f;
        return;
    }

    BOOL left  = (GetAsyncKeyState(g_keyLeft)  & 0x8000) != 0;
    BOOL right = (GetAsyncKeyState(g_keyRight) & 0x8000) != 0;

    /* Both held → cancel (no accidental bias) */
    if (left && right) {
        *steerVal = 0.0f;
        g_frame   = 0;
        g_prevDir = 0;
        return;
    }

    if (!left && !right) {
        *steerVal = 0.0f;
        g_frame   = 0;
        g_prevDir = 0;
        return;
    }

    int   dir       = left ? 1 : -1;
    float direction = (float)dir;

    /* Reset cycle on direction change */
    if (dir != g_prevDir) {
        g_frame   = 0;
        g_prevDir = dir;
    }

    /* Pulse pattern: [spike, spike, ..., partial, partial, ...] */
    int pos = g_frame % g_interval;

    if (pos < g_duration)
        *steerVal = direction * 1.0f;          /* full lock spike  */
    else
        *steerVal = direction * g_deflection;  /* partial / zero   */

    /* Wrap instead of unbounded increment — avoids
     * signed overflow (UB) after ~414 days at 60 fps. */
    g_frame++;
    if (g_frame >= g_interval)
        g_frame = 0;
}

static void __cdecl DS_DummyHandler(int a, int b, char c, void *d)
{
    (void)a; (void)b; (void)c; (void)d;
}

/* ═══════════════════════════════════════════════════════════════
 * Hook / unhook
 * ═══════════════════════════════════════════════════════════════ */
static void install_hooks(void)
{
    if (g_hooked) return;

    /* Save originals */
    read_mem(STEER_HANDLER_ADDR1, &g_origHandler1, 4);
    read_mem(STEER_HANDLER_ADDR2, &g_origHandler2, 4);

    /* Force wheel mode */
    unsigned char wt = 1;
    write_mem(JOYSTICKTYPE_P1_ADDR, &wt, 1);

    /* Patch in our handlers */
    DWORD h1 = (DWORD)&DS_SteerHandler;
    DWORD h2 = (DWORD)&DS_DummyHandler;
    write_mem(STEER_HANDLER_ADDR1, &h1, 4);
    write_mem(STEER_HANDLER_ADDR2, &h2, 4);

    g_hooked = TRUE;
}

static void remove_hooks(void)
{
    if (!g_hooked) return;
    if (g_origHandler1)
        write_mem(STEER_HANDLER_ADDR1, &g_origHandler1, 4);
    if (g_origHandler2)
        write_mem(STEER_HANDLER_ADDR2, &g_origHandler2, 4);

    /* Restore gamepad mode */
    unsigned char pad = 0;
    write_mem(JOYSTICKTYPE_P1_ADDR, &pad, 1);

    g_hooked = FALSE;
}

/* ═══════════════════════════════════════════════════════════════
 * Worker thread
 *  - Waits for game init (polls for valid data at game address)
 *  - Maintains wheel mode
 *  - Handles toggle hotkey
 * ═══════════════════════════════════════════════════════════════ */
static DWORD WINAPI hook_thread(LPVOID param)
{
    (void)param;

    /* Wait for the game to finish initializing.
     * Poll until the joystick type byte is something other
     * than uninitialized memory, or fall back to a timeout. */
    for (int i = 0; i < 60 && !g_shutdown; i++) {   /* up to 6 s */
        unsigned char jt = 0;
        read_mem(JOYSTICKTYPE_P1_ADDR, &jt, 1);
        if (jt != 0xCC && jt != 0x00) break;  /* game wrote it */
        Sleep(100);
    }
    if (g_shutdown) return 0;

    Sleep(500);  /* small extra margin */

    install_hooks();

    BOOL toggleHeld = FALSE;

    while (!g_shutdown) {
        /* ── Toggle hotkey (edge-triggered) ── */
        BOOL nowHeld = (GetAsyncKeyState(g_keyToggle) & 0x8000) != 0;
        if (nowHeld && !toggleHeld) {
            g_enabled = !g_enabled;
            /* Optionally flash something — for now just toggle */
        }
        toggleHeld = nowHeld;

        /* ── Keep wheel mode forced while enabled ── */
        if (g_enabled && g_hooked) {
            unsigned char jt;
            read_mem(JOYSTICKTYPE_P1_ADDR, &jt, 1);
            if (jt != 1) {
                unsigned char wt = 1;
                write_mem(JOYSTICKTYPE_P1_ADDR, &wt, 1);
            }
        }

        Sleep(100);  /* 10 Hz poll is plenty for hotkey + guard */
    }

    remove_hooks();
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * DllMain
 * ═══════════════════════════════════════════════════════════════ */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason,
                    LPVOID lpvReserved)
{
    (void)lpvReserved;

    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        load_config();
        if (!g_enabled) return TRUE;
        g_shutdown    = FALSE;
        g_threadHandle = CreateThread(NULL, 0,
                                      hook_thread, NULL, 0, NULL);
        break;

    case DLL_PROCESS_DETACH:
        g_shutdown = TRUE;
        if (g_threadHandle) {
            WaitForSingleObject(g_threadHandle, 2000);
            CloseHandle(g_threadHandle);
            g_threadHandle = NULL;
        }
        /* Safety: remove hooks even if thread timed out */
        remove_hooks();
        break;
    }

    return TRUE;
}