# Dead Space (2008) virtual display crash fix

A drop-in `d3d9.dll` that stops the original **Dead Space (2008)** from crashing at startup on PCs whose display is a **virtual display adapter**, which is the usual setup when streaming games from a headless PC. It also fixes the two problems you hit next: physics breaking because VSync doesn't cap the frame rate, and menus skipping endlessly after you use the mouse wheel.

## Symptoms

- The game crashes to desktop about 5 seconds after launch, before any menu appears.
- Windows Event Viewer → Application log shows:
  ```
  Faulting application name: Dead Space.exe, version: 1.0.0.222
  Exception code: 0xc0000005
  Fault offset: 0x00025439
  ```
- Verifying game files, reinstalling DirectX, compatibility mode, changing resolution and installing DXVK don't help.
- Your active display comes from software such as **Sunshine / Apollo** (IddSampleDriver / Virtual Display Driver), **Virtual Desktop**, **Parsec**, or **Meta Quest Link**, or no physical monitor is attached.

## Cause

At startup Dead Space reads the display adapter's hardware ID through `EnumDisplayDevicesA` and parses it as `PCI\VEN_xxxx&DEV_xxxx&...` to detect the GPU vendor. Virtual adapters report IDs like `root\iddsampledriver` or `Root\Parsec\VDA` with no `VEN_` in them. The game's lookup returns NULL, it reads from address `0x4`, and it crashes.

### Broken physics once it runs

Dead Space's physics and scripting break at high frame rates. Kendra's hair stretches across the room, and doors stay solid after they open, so you can't walk through. The game relies on VSync to hold it at 60 fps, but VSync often has no effect on a virtual display. On the test machine the game would have run at 1,300–2,400 fps.

### Menus skip endlessly after using the scroll wheel

One wheel click can make the menu selection race and keep moving, and other menu input can feel "sticky". The game asks DirectInput for wheel movement since the last read, but on the test machine (with input from Sunshine/Moonlight) DirectInput returned a **running total** instead: one click up and two down read as `120, 240, 120, 0, -120 ...` rather than `+120, -120, -120`. The game treats every non-zero total as more scrolling. Free-spinning wheels (Logitech MX) make it worse, sending 40+ clicks in half a second.

## The fix

This `d3d9.dll` sits in the game folder and:

1. Hooks `EnumDisplayDevicesA` so that any adapter without a PCI vendor ID reports the ID of a real GPU in your system instead. It prefers a discrete NVIDIA/AMD card over Intel integrated graphics, and falls back to a dummy ID if there's no real GPU at all.
2. Caps the frame rate at 60 fps, which keeps physics and scripting working whether or not VSync works on your display.
3. Counts mouse wheel clicks itself with a low-level mouse hook instead of trusting DirectInput's value. Only clicks made while Dead Space is the active window count. It passes them to the game one at a time, at most one every 100 ms, and drops the excess so scrolling stops when the wheel stops.
4. Passes every Direct3D 9 call through to Windows' own `d3d9.dll`, unchanged.

It doesn't modify any game files, and you can remove it at any time.

## Install

1. Download `d3d9.dll` from the [Releases](../../releases) page.
2. Copy it into the game folder, next to `Dead Space.exe`. For Steam that's usually:
   ```
   C:\Program Files (x86)\Steam\steamapps\common\Dead Space
   ```
3. Launch the game.

To uninstall, delete `d3d9.dll` from the game folder.

This replaces any other `d3d9.dll` in the game folder, such as DXVK's. DXVK isn't needed for this fix, and it doesn't fix the crash on its own.

### Troubleshooting

Create an empty file named `d3d9shim.log` in the game folder and launch the game. The fix will write which GPU it picked, which adapters it rewrote, the frame rate every 10 seconds for the first two minutes (alongside the frame rate the game would reach without the cap), and each wheel click it counted next to the value DirectInput reported. Delete the file to turn logging off again.

## Building

Requires Visual Studio 2019 or later with the C++ build tools (x86).

```bat
build.bat
```

The output is `bin\d3d9.dll`, a 32-bit DLL that statically links the C runtime, so it needs no redistributables.

Pushing a tag that starts with `v` (for example `v1.0.0`) builds the DLL on GitHub Actions and publishes it as a GitHub Release.

## Technical notes

Windows applies its own compatibility shim to `Dead Space.exe` (`DXMaximizedWindowedMode` in `AcLayers.dll`, the "fullscreen optimizations" machinery). That shim imposes some constraints on any proxy `d3d9.dll`:

- While the DLL is still loading, the shim calls d3d9 **export ordinal 17**. This DLL leaves ordinal 17 unused. A proxy that puts a different function at ordinal 17 crashes with a jump to address 0.
- At that point loading another DLL fails (`ERROR_NOT_FOUND`), so the real d3d9 is loaded lazily on the game's first Direct3D call.
- The shim doesn't follow forwarded exports, so each export is a small stub that jumps into the real d3d9.
- The shim hooks `GetProcAddress`, so the `user32` functions involved are resolved by reading `user32`'s export table directly.

## Tested on

- Steam version (build 252137), Windows 11, RTX 4070 Ti SUPER, streaming via Sunshine with Parsec and Meta virtual displays also installed.

Other versions of the game (EA App, GOG-style retail) may have the same bug. Reports are welcome.

## License

MIT. See [LICENSE](LICENSE).
