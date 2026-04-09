/*
 * NFS Underground 1 - Keyboard Double Steering ASI Plugin (v3 - fixed)
 *
 * Build (mingw-w64, 32-bit):
 *   i686-w64-mingw32-gcc -shared -o nfs_ds.asi doublesteer_u1.c
 *       -static-libgcc -O2 -Wl,--enable-stdcall-fixup
 */

#include <windows.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════
 * Game addresses (NFSU1 speed.exe v1.4)
 *
 * STEER_HANDLER_ADDR1/2 point to the 4-byte immediate operand
 * inside a PUSH imm32 instruction (i.e. addr-1 == 0x68 opcode).
 * We overwrite those 4 bytes with our handler's address.
 * ═══════════════════════════════════════════════════════════════ */
#define STEER_HANDLER_ADDR1      0x00461213   /* analog handler  */
#define STEER_HANDLER_ADDR2      0x00461229   /* digital handler */
#define REALDRIVER_STEER_OFFSET  0x28C
#define JOYSTICKTYPE_P1_ADDR     0x007306C4   /* 0xFF=none 0=pad 1=wheel */

/* ═══════════════════════════════════════════════════════════════
 * Configuration (loaded from nfs_ds.ini)
 * ═══════════════════════════════════════════════════════════════ */
static float         g_deflection   = 0.40f;
static float         g_spikeMag     = 1.5f;
static int           g_interval     = 6;
static int           g_duration     = 1;
static DWORD         g_keyLeft      = VK_LEFT;
static DWORD         g_keyRight     = VK_RIGHT;
static DWORD         g_keyToggle    = VK_F9;

static volatile int  g_enabled      = 1;

/* Per-frame state (only touched from the game's callback thread) */
static int           g_frame        = 0;
static int           g_prevDir      = 0;

/* Saved originals for clean unhook */
static DWORD         g_origHandler1 = 0;
static DWORD         g_origHandler2 = 0;
static volatile BOOL g_hooked       = FALSE;
static HANDLE        g_threadHandle = NULL;
static volatile BOOL g_shutdown     = FALSE;

/* ═══════════════════════════════════════════════════════════════
 * Key name -> VK code
 * ═══════════════════════════════════════════════════════════════ */
static DWORD parse_vk(const char *s)
{
    if (!s || !s[0]) return 0;

    if (!s[1]) {
        if (s[0] >= 'A' && s[0] <= 'Z') return (DWORD)s[0];
        if (s[0] >= 'a' && s[0] <= 'z') return (DWORD)(s[0] - 32);
        if (s[0] >= '0' && s[0] <= '9') return (DWORD)s[0];
    }

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

    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 24) return (DWORD)(VK_F1 + n - 1);
    }

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (DWORD)strtoul(s, NULL, 16);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * INI loader — looks for nfs_ds.ini next to speed.exe
 * ═══════════════════════════════════════════════════════════════ */
static void load_config(void)
{
    char ini[MAX_PATH], buf[64];
    DWORD vk;

    GetModuleFileNameA(NULL, ini, MAX_PATH);
    char *p = strrchr(ini, '\\');
    if (p) *(p + 1) = '\0';
    strcat(ini, "nfs_ds.ini");

    int pct = GetPrivateProfileIntA("DoubleSteering",
                                    "DeflectionPercent", 40, ini);
    if (pct < 0)  pct = 0;
    if (pct > 95) pct = 95;
    g_deflection = pct / 100.0f;

    int spk = GetPrivateProfileIntA("DoubleSteering",
                                    "SpikeMagnitude", 150, ini);
    if (spk < 100) spk = 100;
    if (spk > 300) spk = 300;
    g_spikeMag = spk / 100.0f;

    g_enabled = GetPrivateProfileIntA("DoubleSteering", "Enabled", 1, ini);

    g_interval = GetPrivateProfileIntA("DoubleSteering",
                                       "PulseInterval", 6, ini);
    if (g_interval < 2)  g_interval = 2;
    if (g_interval > 60) g_interval = 60;

    g_duration = GetPrivateProfileIntA("DoubleSteering",
                                       "PulseDuration", 1, ini);
    if (g_duration < 1)              g_duration = 1;
    if (g_duration >= g_interval)    g_duration = g_interval - 1;

    GetPrivateProfileStringA("DoubleSteering", "KeyLeft",
                             "Left", buf, sizeof(buf), ini);
    vk = parse_vk(buf);
    if (vk) g_keyLeft = vk;

    GetPrivateProfileStringA("DoubleSteering", "KeyRight",
                             "Right", buf, sizeof(buf), ini);
    vk = parse_vk(buf);
    if (vk) g_keyRight = vk;

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
 * Sign convention: left = +1, right = -1
 * Must ALWAYS write steerVal (including 0.0f on idle) so the
 * game never sees a stale value from the previous frame.
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

    if (!g_enabled) {
        *steerVal = 0.0f;
        return;
    }

    BOOL left  = (GetAsyncKeyState(g_keyLeft)  & 0x8000) != 0;
    BOOL right = (GetAsyncKeyState(g_keyRight) & 0x8000) != 0;

    if ((left && right) || (!left && !right)) {
        *steerVal = 0.0f;
        g_frame   = 0;
        g_prevDir = 0;
        return;
    }

    int   dir       = left ? 1 : -1;
    float direction = (float)dir;

    if (dir != g_prevDir) {
        g_frame   = 0;
        g_prevDir = dir;
    }

    int pos = g_frame % g_interval;

    if (pos < g_duration)
        *steerVal = direction * g_spikeMag;
    else
        *steerVal = direction * g_deflection;

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

    read_mem(STEER_HANDLER_ADDR1, &g_origHandler1, 4);
    read_mem(STEER_HANDLER_ADDR2, &g_origHandler2, 4);

    /* Force wheel mode — disables the game's steering smoothing */
    unsigned char wt = 1;
    write_mem(JOYSTICKTYPE_P1_ADDR, &wt, 1);

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

    unsigned char pad = 0;
    write_mem(JOYSTICKTYPE_P1_ADDR, &pad, 1);

    g_hooked = FALSE;
}

/* ═══════════════════════════════════════════════════════════════
 * Worker thread
 *  - Waits for game init
 *  - Maintains wheel mode byte (game can reset it)
 *  - Handles F9 toggle hotkey
 * ═══════════════════════════════════════════════════════════════ */
static DWORD WINAPI hook_thread(LPVOID param)
{
    (void)param;

    /* Wait for game init — joystick byte starts as 0x00 in .bss
     * or 0xCC in debug builds; game writes 0xFF (none) or 0 (pad).
     * We accept ANY value after a reasonable delay.              */
    Sleep(3000);
    if (g_shutdown) return 0;

    install_hooks();

    BOOL toggleHeld = FALSE;

    while (!g_shutdown) {
        /* Toggle hotkey (edge-triggered) */
        BOOL nowHeld = (GetAsyncKeyState(g_keyToggle) & 0x8000) != 0;
        if (nowHeld && !toggleHeld) {
            g_enabled = !g_enabled;
        }
        toggleHeld = nowHeld;

        /* Keep wheel mode forced while enabled */
        if (g_enabled && g_hooked) {
            unsigned char jt;
            read_mem(JOYSTICKTYPE_P1_ADDR, &jt, 1);
            if (jt != 1) {
                unsigned char wt = 1;
                write_mem(JOYSTICKTYPE_P1_ADDR, &wt, 1);
            }
        }

        Sleep(100);
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
        g_shutdown     = FALSE;
        g_threadHandle = CreateThread(NULL, 0,
                                      hook_thread, NULL, 0, NULL);
        break;

    case DLL_PROCESS_DETACH:
        g_shutdown = TRUE;
        if (g_threadHandle) {
            WaitForSingleObject(g_threadHandle, 4000);
            CloseHandle(g_threadHandle);
            g_threadHandle = NULL;
        }
        remove_hooks();
        break;
    }

    return TRUE;
}
