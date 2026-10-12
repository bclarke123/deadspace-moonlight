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
 * Direct3D 9 calls are passed through to the system d3d9.dll.
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
#include <windows.h>
#include <string.h>

#define ARRAYSIZE_(a) (sizeof(a) / sizeof((a)[0]))

typedef BOOL (WINAPI *EnumDisplayDevicesW_t)(LPCWSTR, DWORD, PDISPLAY_DEVICEW, DWORD);

static HINSTANCE g_self;
static char g_logPath[MAX_PATH];
static BOOL g_logEnabled;
static char g_gpuId[128];
static EnumDisplayDevicesW_t g_enumW;
static unsigned __int64 g_loggedAdapters;

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

/* Exports and their ordinals. Ordinal 17 is deliberately unused (see top). */
#define D3D9_EXPORTS(X)                                  \
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
    X(Direct3DCreate9, 37)                               \
    X(Direct3DCreate9Ex, 38)

enum {
#define X(name, ord) IDX_##name,
    D3D9_EXPORTS(X)
#undef X
    IDX_COUNT
};

static const char *const g_exportNames[IDX_COUNT] = {
#define X(name, ord) #name,
    D3D9_EXPORTS(X)
#undef X
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
D3D9_EXPORTS(X)
#undef X

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
