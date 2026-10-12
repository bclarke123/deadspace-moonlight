/*
 * Dead Space (2008) virtual display fix - proxy d3d9.dll
 *
 * At startup Dead Space reads the primary display adapter's DeviceID and
 * parses it as "PCI\VEN_xxxx&DEV_xxxx&...". Virtual display adapters used for
 * game streaming (Sunshine/Apollo's IddSampleDriver, Virtual Desktop, Parsec,
 * ...) report IDs like "root\iddsampledriver" with no "VEN_" in them, and the
 * game dereferences a NULL pointer and crashes ~5 seconds after launch.
 *
 * This DLL hooks user32!EnumDisplayDevicesA and, for adapters that lack a PCI
 * vendor ID, reports the DeviceID of a real GPU in the system instead. All
 * Direct3D 9 calls are passed through to the system d3d9.dll, and frames are
 * capped at 60 fps because the game's physics break at higher frame rates.
 * The mouse wheel is also counted independently of DirectInput (whose wheel
 * value can get stuck, scrolling menus forever) and rate-limited.
 *
 * Constraints learned the hard way:
 *  - Windows applies its DXMaximizedWindowedMode compatibility shim
 *    (AcLayers.dll) to Dead Space. While our DLL is still loading, that shim
 *    looks up d3d9 export ordinal 17 and calls it. We must not export ordinal
 *    17, and we must not load the real d3d9 at that point (LoadLibrary fails
 *    with ERROR_NOT_FOUND), so the real d3d9 is loaded lazily on first use.
 *  - Exports are real code stubs, not forwarders: AcLayers does not follow
 *    forwarded exports.
 *  - AcLayers also hooks GetProcAddress, so user32 functions are resolved by
 *    walking the export table directly.
 *
 * Logging: create an empty "d3d9shim.log" next to this DLL to enable it.
 */
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <string.h>

#define ARRAYSIZE_(a) (sizeof(a) / sizeof((a)[0]))

typedef BOOL (WINAPI *EnumDisplayDevicesW_t)(LPCWSTR, DWORD, PDISPLAY_DEVICEW, DWORD);

static HINSTANCE g_self;
static char g_logPath[MAX_PATH];
static BOOL g_logEnabled;
static char g_gpuId[128];
static EnumDisplayDevicesW_t g_enumW;
static unsigned __int64 g_loggedAdapters;

static void InstallWheelFix(void);

/* ------------------------------------------------------------------------ */
/* Logging                                                                   */

static void Log(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    DWORD n;
    HANDLE h;

    if (!g_logEnabled)
        return;
    va_start(args, fmt);
    n = wvsprintfA(buf, fmt, args);
    va_end(args);
    h = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, buf, n, &n, NULL);
    WriteFile(h, "\r\n", 2, &n, NULL);
    CloseHandle(h);
}

static void InitLog(void)
{
    char *slash;
    GetModuleFileNameA(g_self, g_logPath, MAX_PATH);
    slash = strrchr(g_logPath, '\\');
    if (!slash)
        return;
    lstrcpyA(slash + 1, "d3d9shim.log");
    g_logEnabled = GetFileAttributesA(g_logPath) != INVALID_FILE_ATTRIBUTES;
}

/* ------------------------------------------------------------------------ */
/* Display adapter ID fix                                                    */

/* Resolve an export by walking the module's export table, bypassing any
 * GetProcAddress hooks installed by the compatibility shim. */
static void *RawExport(HMODULE mod, const char *name)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_EXPORT_DIRECTORY *exp;
    DWORD *names, *funcs, i, rva;
    WORD *ords;

    if (!base)
        return NULL;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    exp = (IMAGE_EXPORT_DIRECTORY *)(base + dir->VirtualAddress);
    names = (DWORD *)(base + exp->AddressOfNames);
    ords = (WORD *)(base + exp->AddressOfNameOrdinals);
    funcs = (DWORD *)(base + exp->AddressOfFunctions);
    for (i = 0; i < exp->NumberOfNames; i++) {
        if (strcmp((char *)(base + names[i]), name) != 0)
            continue;
        rva = funcs[ords[i]];
        /* Forwarded exports point back into the export directory. */
        if (rva >= dir->VirtualAddress && rva < dir->VirtualAddress + dir->Size)
            return NULL;
        return base + rva;
    }
    return NULL;
}

/* Pick the DeviceID of a real (PCI) GPU, preferring a discrete card over
 * Intel integrated graphics. */
static void FindRealGpu(void)
{
    DISPLAY_DEVICEW dd;
    DWORD i;

    g_gpuId[0] = 0;
    for (i = 0; ; i++) {
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!g_enumW(NULL, i, &dd, 0))
            break;
        if (!wcsstr(dd.DeviceID, L"VEN_"))
            continue;
        if (!g_gpuId[0] || strstr(g_gpuId, "VEN_8086"))
            WideCharToMultiByte(CP_ACP, 0, dd.DeviceID, -1, g_gpuId, sizeof(g_gpuId), NULL, NULL);
        if (!wcsstr(dd.DeviceID, L"VEN_8086"))
            break;
    }
    /* No PCI adapter found at all: any well-formed ID keeps the game's parser happy. */
    if (!g_gpuId[0])
        lstrcpyA(g_gpuId, "PCI\\VEN_0000&DEV_0000&SUBSYS_00000000&REV_00");
}

static BOOL WINAPI Hook_EnumDisplayDevicesA(LPCSTR device, DWORD index, PDISPLAY_DEVICEA out, DWORD flags)
{
    WCHAR wdevice[64];
    DISPLAY_DEVICEW w;
    DISPLAY_DEVICEA a;
    DWORD cb;

    if (!out || out->cb <= sizeof(DWORD))
        return FALSE;
    cb = out->cb < sizeof(a) ? out->cb : sizeof(a);
    if (device)
        MultiByteToWideChar(CP_ACP, 0, device, -1, wdevice, ARRAYSIZE_(wdevice));

    ZeroMemory(&w, sizeof(w));
    w.cb = sizeof(w);
    if (!g_enumW(device ? wdevice : NULL, index, &w, flags))
        return FALSE;

    ZeroMemory(&a, sizeof(a));
    a.StateFlags = w.StateFlags;
    WideCharToMultiByte(CP_ACP, 0, w.DeviceName, -1, a.DeviceName, sizeof(a.DeviceName), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, w.DeviceString, -1, a.DeviceString, sizeof(a.DeviceString), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, w.DeviceID, -1, a.DeviceID, sizeof(a.DeviceID), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, w.DeviceKey, -1, a.DeviceKey, sizeof(a.DeviceKey), NULL, NULL);

    /* Adapter-level query on a virtual adapter: report the real GPU instead. */
    if (!device && !strstr(a.DeviceID, "VEN_")) {
        /* The game enumerates repeatedly; log each adapter once. */
        if (index < 64 && !(g_loggedAdapters & (1ull << index))) {
            g_loggedAdapters |= 1ull << index;
            Log("adapter #%lu \"%s\" (%s) -> %s", index, a.DeviceString, a.DeviceID, g_gpuId);
        }
        lstrcpynA(a.DeviceID, g_gpuId, sizeof(a.DeviceID));
    }

    /* Honour the caller's struct size; cb itself is left untouched. */
    memcpy((BYTE *)out + sizeof(DWORD), (BYTE *)&a + sizeof(DWORD), cb - sizeof(DWORD));
    return TRUE;
}

static void InstallDisplayHook(void)
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    BYTE *target;
    DWORD old;

    if (!user32)
        user32 = LoadLibraryA("user32.dll");
    g_enumW = (EnumDisplayDevicesW_t)RawExport(user32, "EnumDisplayDevicesW");
    target = (BYTE *)RawExport(user32, "EnumDisplayDevicesA");
    if (!g_enumW || !target) {
        Log("could not find EnumDisplayDevices in user32");
        return;
    }
    FindRealGpu();

    /* The hook never calls the original, so overwriting the first 5 bytes with
     * a jmp is enough; no trampoline needed. */
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old))
        return;
    target[0] = 0xE9;
    *(DWORD *)(target + 1) = (DWORD)((UINT_PTR)Hook_EnumDisplayDevicesA - (UINT_PTR)(target + 5));
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    Log("hook installed, real GPU: %s", g_gpuId);
}

/* ------------------------------------------------------------------------ */
/* d3d9 pass-through                                                         */

/* Exports passed straight through, with their ordinals. Direct3DCreate9 (37)
 * is wrapped below. Ordinal 17 is deliberately unused (see top). */
#define D3D9_PASSTHROUGH(X)                              \
    X(Direct3DCreate9On12, 20)                           \
    X(Direct3DCreate9On12Ex, 21)                         \
    X(Direct3DShaderValidatorCreate9, 24)                \
    X(PSGPError, 25)                                     \
    X(PSGPSampleTexture, 26)                             \
    X(D3DPERF_BeginEvent, 27)                            \
    X(D3DPERF_EndEvent, 28)                              \
    X(D3DPERF_GetStatus, 29)                             \
    X(D3DPERF_QueryRepeatFrame, 30)                      \
    X(D3DPERF_SetMarker, 31)                             \
    X(D3DPERF_SetOptions, 32)                            \
    X(D3DPERF_SetRegion, 33)                             \
    X(DebugSetLevel, 34)                                 \
    X(DebugSetMute, 35)                                  \
    X(Direct3D9EnableMaximizedWindowedModeShim, 36)      \
    X(Direct3DCreate9Ex, 38)

enum {
#define X(name, ord) IDX_##name,
    D3D9_PASSTHROUGH(X)
#undef X
    IDX_Direct3DCreate9,
    IDX_COUNT
};

static const char *const g_exportNames[IDX_COUNT] = {
#define X(name, ord) #name,
    D3D9_PASSTHROUGH(X)
#undef X
    "Direct3DCreate9",
};

static void *g_real[IDX_COUNT];
static volatile LONG g_realLoaded;

/* Load the system d3d9.dll. Called on the first d3d9 call, never while the
 * loader is busy. */
static void __cdecl LoadRealD3D9(void)
{
    char path[MAX_PATH];
    HMODULE real;
    int i;

    if (InterlockedExchange(&g_realLoaded, 1))
        return;

    GetSystemDirectoryA(path, MAX_PATH); /* SysWOW64 for this 32-bit process */
    lstrcatA(path, "\\d3d9.dll");
    real = LoadLibraryA(path);
    if (!real) {
        Log("failed to load real d3d9 (error %lu)", GetLastError());
        return;
    }
    Log("using %s", path);
    for (i = 0; i < IDX_COUNT; i++)
        g_real[i] = (void *)(UINT_PTR)GetProcAddress(real, g_exportNames[i]);
}

/* Each export: load the real d3d9 on first use, then tail-jump into it with
 * the caller's stack and registers untouched. */
#define X(name, ord)                                                            \
    __declspec(naked) void Stub_##name(void)                                    \
    {                                                                           \
        __asm pushad                                                            \
        __asm call LoadRealD3D9                                                 \
        __asm popad                                                             \
        __asm jmp dword ptr [g_real + IDX_##name * 4]                           \
    }                                                                           \
    __pragma(comment(linker, "/export:" #name "=_Stub_" #name ",@" #ord))
D3D9_PASSTHROUGH(X)
#undef X

/* ------------------------------------------------------------------------ */
/* 60 fps frame limiter                                                      */
/*                                                                           */
/* Dead Space's physics and scripting break well above 60 fps: doors stay    */
/* solid after opening, and hair and cloth stretch across rooms. The game    */
/* relies on vsync to hold it at 60, but vsync often has no effect on        */
/* virtual displays, so we pace IDirect3DDevice9::Present ourselves.         */

#define TARGET_FPS 60

/* vtable slots, from d3d9.h */
#define VT_D3D9_CREATEDEVICE 16
#define VT_DEVICE_PRESENT    17

typedef HRESULT (WINAPI *CreateDevice_t)(void *, UINT, DWORD, HWND, DWORD, void *, void **);
typedef HRESULT (WINAPI *Present_t)(void *, const RECT *, const RECT *, HWND, const void *);

static CreateDevice_t g_realCreateDevice;
static Present_t g_realPresent;
static HANDLE g_timer;
static LARGE_INTEGER g_qpcFreq, g_nextFrame, g_fpsWindowStart;
static DWORD g_fpsFrames;
static LONGLONG g_fpsWaited;
static int g_fpsReports;

static void PatchVtable(void **vtable, int slot, void *hook, void **original)
{
    DWORD old;
    if (vtable[slot] == hook)
        return;
    if (!VirtualProtect(&vtable[slot], sizeof(void *), PAGE_READWRITE, &old))
        return;
    *original = vtable[slot];
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void *), old, &old);
}

/* Wait until the given QPC time: sleep on a high-resolution timer for most of
 * it, then spin for the last ~1.5 ms for accuracy. */
static void WaitUntil(LONGLONG target)
{
    LARGE_INTEGER now, due;
    LONGLONG spinTicks = g_qpcFreq.QuadPart * 3 / 2000;
    LONGLONG remaining;

    QueryPerformanceCounter(&now);
    remaining = target - now.QuadPart - spinTicks;
    if (remaining > 0) {
        /* Negative due time = relative, in 100 ns units. */
        due.QuadPart = -(remaining * 10000000 / g_qpcFreq.QuadPart);
        if (g_timer && SetWaitableTimer(g_timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(g_timer, INFINITE);
        else
            Sleep((DWORD)(remaining * 1000 / g_qpcFreq.QuadPart));
    }
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < target);
}

static void LogFps(void)
{
    LARGE_INTEGER now;
    LONGLONG elapsed;

    QueryPerformanceCounter(&now);
    g_fpsFrames++;
    elapsed = now.QuadPart - g_fpsWindowStart.QuadPart;
    if (elapsed < g_qpcFreq.QuadPart * 10)
        return;
    /* A dozen reports (two minutes) is enough to confirm the cap. */
    if (g_fpsReports < 12) {
        g_fpsReports++;
        /* Time not spent waiting shows how fast the game would run uncapped. */
        Log("fps: %lu (uncapped would be ~%lu)", (DWORD)(g_fpsFrames * g_qpcFreq.QuadPart / elapsed),
            elapsed > g_fpsWaited ? (DWORD)(g_fpsFrames * g_qpcFreq.QuadPart / (elapsed - g_fpsWaited)) : 0);
    }
    g_fpsWindowStart = now;
    g_fpsFrames = 0;
    g_fpsWaited = 0;
}

static HRESULT WINAPI Hook_Present(void *device, const RECT *src, const RECT *dst, HWND wnd, const void *dirty)
{
    LONGLONG period = g_qpcFreq.QuadPart / TARGET_FPS;
    LARGE_INTEGER now;

    QueryPerformanceCounter(&now);
    if (now.QuadPart < g_nextFrame.QuadPart) {
        WaitUntil(g_nextFrame.QuadPart);
        g_fpsWaited += g_nextFrame.QuadPart - now.QuadPart;
        g_nextFrame.QuadPart += period;
    } else if (now.QuadPart - g_nextFrame.QuadPart > period) {
        /* Fell behind (loading screen, alt-tab): restart pacing from now
         * instead of rushing frames out to catch up. */
        g_nextFrame.QuadPart = now.QuadPart + period;
    } else {
        g_nextFrame.QuadPart += period;
    }
    if (g_logEnabled)
        LogFps();
    return g_realPresent(device, src, dst, wnd, dirty);
}

static HRESULT WINAPI Hook_CreateDevice(void *d3d, UINT adapter, DWORD type, HWND wnd, DWORD flags,
                                        void *params, void **device)
{
    HRESULT hr = g_realCreateDevice(d3d, adapter, type, wnd, flags, params, device);
    if (SUCCEEDED(hr) && device && *device) {
        PatchVtable(*(void ***)*device, VT_DEVICE_PRESENT, (void *)Hook_Present, (void **)&g_realPresent);
        Log("device created, frame limiter at %lu fps", (DWORD)TARGET_FPS);
    }
    return hr;
}

void *WINAPI My_Direct3DCreate9(UINT sdkVersion)
{
    typedef void *(WINAPI *Direct3DCreate9_t)(UINT);
    void *d3d;

    LoadRealD3D9();
    if (!g_real[IDX_Direct3DCreate9])
        return NULL;
    d3d = ((Direct3DCreate9_t)g_real[IDX_Direct3DCreate9])(sdkVersion);
    if (d3d) {
        if (!g_qpcFreq.QuadPart) {
            QueryPerformanceFrequency(&g_qpcFreq);
            g_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!g_timer) /* High-resolution timers need Windows 10 1803+. */
                g_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
            QueryPerformanceCounter(&g_fpsWindowStart);
            InstallWheelFix();
        }
        PatchVtable(*(void ***)d3d, VT_D3D9_CREATEDEVICE, (void *)Hook_CreateDevice, (void **)&g_realCreateDevice);
    }
    return d3d;
}
#pragma comment(linker, "/export:Direct3DCreate9=_My_Direct3DCreate9@4,@37")

/* ------------------------------------------------------------------------ */
/* Mouse wheel fix                                                           */
/*                                                                           */
/* The game reads the wheel through DirectInput as movement since the last   */
/* read. On some setups (seen with input injected by Sunshine/Moonlight)     */
/* DirectInput's wheel value never resets to zero, so the game sees endless  */
/* scrolling and menus skip forever. Fast wheels (free-spinning Logitech MX) */
/* and high-resolution scrolling also flood the menus.                       */
/*                                                                           */
/* So we ignore DirectInput's wheel value and count wheel events ourselves   */
/* with a low-level mouse hook, then hand the game whole clicks, at most one */
/* per WHEEL_MIN_INTERVAL_MS, dropping the excess so movement stops when the */
/* wheel stops.                                                              */

/* vtable slots, from dinput.h */
#define VT_DIDEV_GETCAPABILITIES 3
#define VT_DIDEV_GETPROPERTY     5
#define VT_DIDEV_GETDEVICESTATE  9
#define VT_DIDEV_GETDEVICEDATA   10

/* At most one wheel click per this many ms reaches the game. */
#define WHEEL_MIN_INTERVAL_MS 100
/* A partial click older than this is dropped instead of carried over. */
#define WHEEL_IDLE_RESET_MS 300

typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, void **, void *);
typedef HRESULT (WINAPI *GetCapabilities_t)(void *, DIDEVCAPS *);
typedef HRESULT (WINAPI *GetProperty_t)(void *, REFGUID, DIPROPHEADER *);
typedef HRESULT (WINAPI *GetDeviceState_t)(void *, DWORD, void *);
typedef HRESULT (WINAPI *GetDeviceData_t)(void *, DWORD, DIDEVICEOBJECTDATA *, DWORD *, DWORD);

static GetDeviceState_t g_realGetDeviceState;
static GetDeviceData_t g_realGetDeviceData;

/* Wheel movement counted by the mouse hook and not yet read by the game. */
static volatile LONG g_wheelCounted;
static BOOL g_wheelHooked;

/* The device vtable is shared by mice and keyboards, so remember which device
 * objects are mice, and how each one reports its wheel. */
typedef struct {
    void *device;
    LONG click;      /* wheel units per click, 0 = not a mouse */
    BOOL absolute;   /* game asked for the wheel as a position, not a delta */
    LONG gamePos;    /* absolute mode: position we report to the game */
    LONG pending;    /* movement not yet passed on */
    DWORD lastMove;  /* GetTickCount of the last wheel movement */
    DWORD lastEmit;  /* GetTickCount of the last click passed on */
    LONG lastDiRaw;  /* DirectInput's own wheel value, for the log */
} WheelState;

static WheelState g_wheels[8];
static int g_wheelLogLines;

static BOOL GetDwordProperty(void *device, REFGUID prop, DWORD obj, DWORD how, DWORD *value)
{
    DIPROPDWORD p;
    ZeroMemory(&p, sizeof(p));
    p.diph.dwSize = sizeof(p);
    p.diph.dwHeaderSize = sizeof(p.diph);
    p.diph.dwObj = obj;
    p.diph.dwHow = how;
    if (FAILED(((GetProperty_t)(*(void ***)device)[VT_DIDEV_GETPROPERTY])(device, prop, &p.diph)))
        return FALSE;
    *value = p.dwData;
    return TRUE;
}

static WheelState *WheelFor(void *device)
{
    DIDEVCAPS caps;
    DWORD value;
    WheelState *w;
    int i;

    for (i = 0; i < (int)ARRAYSIZE_(g_wheels); i++) {
        if (g_wheels[i].device == device)
            return g_wheels[i].click ? &g_wheels[i] : NULL;
    }
    for (i = 0; i < (int)ARRAYSIZE_(g_wheels) && g_wheels[i].device; i++)
        ;
    if (i == (int)ARRAYSIZE_(g_wheels))
        return NULL;
    w = &g_wheels[i];
    w->device = device;

    ZeroMemory(&caps, sizeof(caps));
    caps.dwSize = sizeof(caps);
    if (FAILED(((GetCapabilities_t)(*(void ***)device)[VT_DIDEV_GETCAPABILITIES])(device, &caps)) ||
        GET_DIDEVICE_TYPE(caps.dwDevType) != DI8DEVTYPE_MOUSE)
        return NULL;

    /* Wheel units per click: usually WHEEL_DELTA (120). */
    if (GetDwordProperty(device, DIPROP_GRANULARITY, DIMOFS_Z, DIPH_BYOFFSET, &value) && value > 1 && value <= 10000)
        w->click = (LONG)value;
    else
        w->click = WHEEL_DELTA;
    w->absolute = GetDwordProperty(device, DIPROP_AXISMODE, 0, DIPH_DEVICE, &value) && value == DIPROPAXISMODE_ABS;
    Log("mouse device found, wheel click = %ld, %s axes", w->click, w->absolute ? "absolute" : "relative");
    return w;
}

/* Take the wheel movement counted since the last read and return the movement
 * to pass on: zero or one whole click. */
static LONG FilterWheel(WheelState *w, LONG diRaw, const char *source)
{
    LONG delta = InterlockedExchange(&g_wheelCounted, 0);
    DWORD now = GetTickCount();
    LONG out = 0;

    if (delta != 0) {
        /* Drop a stale partial click, or movement left over from the other direction. */
        if (now - w->lastMove > WHEEL_IDLE_RESET_MS || (w->pending > 0) != (delta > 0))
            w->pending = 0;
        w->lastMove = now;
        w->pending += delta;
    }
    if ((w->pending >= w->click || w->pending <= -w->click) && now - w->lastEmit >= WHEEL_MIN_INTERVAL_MS) {
        out = w->pending > 0 ? w->click : -w->click;
        w->lastEmit = now;
        w->pending = 0; /* drop the excess so a fast spin doesn't keep scrolling */
    }
    /* Never bank more than one click while rate-limited. */
    if (w->pending > w->click)
        w->pending = w->click;
    else if (w->pending < -w->click)
        w->pending = -w->click;

    if (g_logEnabled && (delta || out || diRaw != w->lastDiRaw) && g_wheelLogLines < 400) {
        g_wheelLogLines++;
        Log("wheel %s t=%lu counted=%ld out=%ld (directinput said %ld)", source, now, delta, out, diRaw);
    }
    w->lastDiRaw = diRaw;
    return out;
}

static HRESULT WINAPI Hook_GetDeviceState(void *device, DWORD size, void *data)
{
    HRESULT hr = g_realGetDeviceState(device, size, data);
    WheelState *w;

    /* DIMOUSESTATE and DIMOUSESTATE2 both start lX, lY, lZ. */
    if (g_wheelHooked && SUCCEEDED(hr) && data && size >= 3 * sizeof(LONG) && (w = WheelFor(device)) != NULL) {
        LONG *z = &((LONG *)data)[2];
        LONG out = FilterWheel(w, *z, "state");
        if (w->absolute) {
            w->gamePos += out;
            *z = w->gamePos;
        } else {
            *z = out;
        }
    }
    return hr;
}

static HRESULT WINAPI Hook_GetDeviceData(void *device, DWORD objSize, DIDEVICEOBJECTDATA *events,
                                         DWORD *count, DWORD flags)
{
    HRESULT hr = g_realGetDeviceData(device, objSize, events, count, flags);
    WheelState *w;
    DWORD i, kept;
    BYTE *base = (BYTE *)events;
    BOOL delivered = FALSE;

    /* Leave peeks and count-only calls alone. */
    if (!g_wheelHooked || FAILED(hr) || !events || !count || (flags & DIGDD_PEEK) ||
        objSize < sizeof(DIDEVICEOBJECTDATA_DX3) || (w = WheelFor(device)) == NULL)
        return hr;

    /* Replace DirectInput's wheel events: the first one carries our filtered
     * click (if any), the rest are dropped. */
    for (i = 0, kept = 0; i < *count; i++) {
        DIDEVICEOBJECTDATA *e = (DIDEVICEOBJECTDATA *)(base + i * objSize);
        if (e->dwOfs == (DWORD)DIMOFS_Z) {
            LONG out = delivered ? 0 : FilterWheel(w, (LONG)e->dwData, "data");
            delivered = TRUE;
            if (out == 0)
                continue;
            w->gamePos += out;
            e->dwData = (DWORD)(w->absolute ? w->gamePos : out);
        }
        if (kept != i)
            memmove(base + kept * objSize, e, objSize);
        kept++;
    }
    *count = kept;
    return hr;
}

/* Low-level mouse hook: count wheel movement while the game is the active
 * window. Runs on its own thread; must return quickly. */
static LRESULT CALLBACK WheelHookProc(int code, WPARAM msg, LPARAM lparam)
{
    if (code == HC_ACTION && msg == WM_MOUSEWHEEL) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId())
            InterlockedExchangeAdd(&g_wheelCounted, (SHORT)HIWORD(((MSLLHOOKSTRUCT *)lparam)->mouseData));
    }
    return CallNextHookEx(NULL, code, msg, lparam);
}

static DWORD WINAPI WheelHookThread(void *param)
{
    HANDLE ready = (HANDLE)param;
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, WheelHookProc, g_self, 0);
    MSG m;

    g_wheelHooked = hook != NULL;
    SetEvent(ready);
    if (!hook)
        return 0;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
        DispatchMessageW(&m);
    UnhookWindowsHookEx(hook);
    return 0;
}

/* Patch the shared DirectInput device vtable via a throwaway mouse device and
 * start the mouse hook. Called from Direct3DCreate9, once the game's own
 * DirectInput is loaded. */
static void InstallWheelFix(void)
{
    HMODULE dinput = GetModuleHandleA("dinput8.dll");
    DirectInput8Create_t create;
    IDirectInput8A *di = NULL;
    IDirectInputDevice8A *mouse = NULL;
    HANDLE ready, thread;

    if (!dinput)
        return;

    ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    thread = ready ? CreateThread(NULL, 0, WheelHookThread, ready, 0, NULL) : NULL;
    if (thread) {
        WaitForSingleObject(ready, 5000);
        CloseHandle(thread);
    }
    if (ready)
        CloseHandle(ready);
    if (!g_wheelHooked) {
        Log("mouse hook failed (error %lu), wheel left alone", GetLastError());
        return;
    }

    create = (DirectInput8Create_t)RawExport(dinput, "DirectInput8Create");
    if (!create || FAILED(create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&di, NULL)))
        return;
    if (SUCCEEDED(di->lpVtbl->CreateDevice(di, &GUID_SysMouse, &mouse, NULL))) {
        void **vtable = *(void ***)mouse;
        PatchVtable(vtable, VT_DIDEV_GETDEVICESTATE, (void *)Hook_GetDeviceState, (void **)&g_realGetDeviceState);
        PatchVtable(vtable, VT_DIDEV_GETDEVICEDATA, (void *)Hook_GetDeviceData, (void **)&g_realGetDeviceData);
        Log("wheel fix installed");
        mouse->lpVtbl->Release(mouse);
    }
    di->lpVtbl->Release(di);
}

/* ------------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        InitLog();
        InstallDisplayHook();
    }
    return TRUE;
}
