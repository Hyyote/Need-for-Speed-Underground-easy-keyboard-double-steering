/*
 * NFS Underground 2 - Keyboard Double Steering ASI Plugin
 * ========================================================
 * Hooks the game's steering handler (static function pointers)
 * and forces wheel mode to disable steering smoothing.
 *
 * Why this works:
 *   - ADDR1/ADDR2 are static addresses in the exe's data section.
 *     They don't change between races — no crash risk.
 *   - RealDriver is passed as a parameter each frame — always fresh.
 *   - Setting joystick type to "wheel" disables the game's input
 *     smoothing, so partial analog values actually reach physics.
 *   - Our handler reads keyboard directly and combines partial
 *     analog steer + full digital steer = double steering.
 *
 * Build (mingw-w64, 32-bit):
 *   i686-w64-mingw32-gcc -shared -o nfs_ds.asi doublesteer.c
 *       -static-libgcc -O2 -Wl,--enable-stdcall-fixup
 */

#include <windows.h>
#include <math.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════
 * Game addresses (NFSU1 speed.exe v1.4)
 * from xan1242/NFSU-XtendedInput NFSU_Addresses.h
 * ═══════════════════════════════════════════════════════════════ */

/* Function pointer table entries for steering handlers.
 * These are STATIC addresses in the exe — safe across races. */
#define STEER_HANDLER_ADDR1      0x0041E083  /* analog handler ptr */
#define STEER_HANDLER_ADDR2      0x0041E09A  /* digital handler ptr */

/* Offset in RealDriver struct where steering float lives */
#define REALDRIVER_STEER_OFFSET  0x2C8

/* Joystick type byte: 0xFF = none, 0 = joypad, 1 = wheel */
#define JOYSTICKTYPE_P1_ADDR     0x00864788

/* ═══════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════ */

static float g_deflection      = 0.45f;
static int   g_interval        = 8;
static int   g_duration        = 2;
static int   g_enabled         = 1;

static void load_config(void)
{
    char ini[MAX_PATH];
    GetModuleFileNameA(NULL, ini, MAX_PATH);
    char *p = strrchr(ini, '\\');
    if (p) *(p + 1) = '\0';
    strcat(ini, "nfs_ds.ini");

    int pct = GetPrivateProfileIntA("DoubleSteering", "DeflectionPercent", 45, ini);
    if (pct < 5) pct = 5;
    if (pct > 95) pct = 95;
    g_deflection = pct / 100.0f;

    g_enabled = GetPrivateProfileIntA("DoubleSteering", "Enabled", 1, ini);

    g_interval = GetPrivateProfileIntA("DoubleSteering", "PulseInterval", 8, ini);
    if (g_interval < 2) g_interval = 2;
    if (g_interval > 60) g_interval = 60;

    g_duration = GetPrivateProfileIntA("DoubleSteering", "PulseDuration", 2, ini);
    if (g_duration < 1) g_duration = 1;
    if (g_duration >= g_interval) g_duration = g_interval - 1;
}

/* ═══════════════════════════════════════════════════════════════
 * Memory write helper
 * ═══════════════════════════════════════════════════════════════ */

static BOOL write_mem(DWORD addr, void *data, size_t len)
{
    DWORD oldProt;
    if (!VirtualProtect((LPVOID)addr, len, PAGE_EXECUTE_READWRITE, &oldProt))
        return FALSE;
    memcpy((void *)addr, data, len);
    VirtualProtect((LPVOID)addr, len, oldProt, &oldProt);
    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════
 * Steering handler
 *
 * Called by the game every frame. We ignore what the game passes
 * in and read keyboard directly. When an arrow key is held,
 * alternates between partial deflection and full lock spikes,
 * simulating double steering from a single key.
 *
 * With wheel mode forced, the game applies NO smoothing, so
 * the partial/full alternation reaches physics directly.
 *
 * Signature: void __cdecl handler(int port, int event, char val, void* driver)
 * ═══════════════════════════════════════════════════════════════ */

static int g_frame   = 0;
static int g_prevDir = 0;

static void __cdecl DS_SteerHandler(int JoystickPort, int JoyEvent,
                                     char Value, void *RealDriver)
{
    (void)JoystickPort; (void)JoyEvent; (void)Value;

    if (!RealDriver) return;

    float *steerVal = (float *)((DWORD)RealDriver + REALDRIVER_STEER_OFFSET);

    BOOL left  = (GetAsyncKeyState(VK_LEFT)  & 0x8000) != 0;
    BOOL right = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;

    if (!left && !right) {
        *steerVal = 0.0f;
        g_frame = 0;
        g_prevDir = 0;
        return;
    }

    /* Direction: left = positive, right = negative */
    int dir = left ? 1 : -1;
    float direction = (float)dir;

    /* Reset counter on direction change */
    if (dir != g_prevDir) {
        g_frame = 0;
        g_prevDir = dir;
    }

    /* Pulse pattern: spike then partial, repeating */
    int pos = g_frame % g_interval;

    if (pos < g_duration) {
        /* Digital spike — full lock */
        *steerVal = direction * 1.0f;
    } else {
        /* Analog hold — partial deflection */
        *steerVal = direction * g_deflection;
    }

    g_frame++;
}

static void __cdecl DS_DummyHandler(int a, int b, char c, void *d)
{
    (void)a; (void)b; (void)c; (void)d;
}

/* ═══════════════════════════════════════════════════════════════
 * Hook installation thread
 * ═══════════════════════════════════════════════════════════════ */

static DWORD WINAPI hook_thread(LPVOID param)
{
    (void)param;
    Sleep(3000);  /* Wait for game init */

    /* Force wheel mode — disables steering smoothing */
    unsigned char wheelType = 1;
    write_mem(JOYSTICKTYPE_P1_ADDR, &wheelType, 1);

    /* Install our steering handler (overwrites function pointers) */
    unsigned int handler = (unsigned int)&DS_SteerHandler;
    unsigned int dummy   = (unsigned int)&DS_DummyHandler;
    write_mem(STEER_HANDLER_ADDR1, &handler, 4);
    write_mem(STEER_HANDLER_ADDR2, &dummy, 4);

    /* Keep forcing wheel mode in case the game resets it */
    while (1) {
        unsigned char *joyType = (unsigned char *)JOYSTICKTYPE_P1_ADDR;
        if (*joyType != 1) {
            write_mem(JOYSTICKTYPE_P1_ADDR, &wheelType, 1);
        }
        Sleep(1000);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * DllMain
 * ═══════════════════════════════════════════════════════════════ */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason,
                    LPVOID lpvReserved)
{
    (void)hinstDLL; (void)lpvReserved;

    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        load_config();
        if (!g_enabled) return TRUE;
        CreateThread(NULL, 0, hook_thread, NULL, 0, NULL);
    }

    return TRUE;
}
