# Chromium in GD

A Windows-only Chromium browser rendered **inside** Geometry Dash **2.2081** (Geode **5.8.2**).

Unlike a native child window bolted onto the game, the page is rendered by the
**Chromium Embedded Framework** in off-screen (windowless) mode: every frame CEF
paints is uploaded to an OpenGL texture and drawn as part of the GD scene. This
is what makes it work **in fullscreen** — a real child HWND gets blanked by
Windows' "fullscreen optimizations", which is the bug that motivated the switch
away from WebView2.

The frame floats above every scene, is dragged by its top bar, resized from the
lower-right handle, and remembers its size and position across launches.

## Features

- **Works in fullscreen** (off-screen rendering, not a native overlay window)
- Multiple tabs; links that open a new window become new tabs; middle-click a tab to close it
- Address/search bar with clear (✕) and copy buttons, back / forward / reload, loading spinner
- Mouse side buttons (X1/X2) navigate back / forward
- Downloads and browsing history, both persisted across launches, in a `...` menu
- In-page right-click context menus and JavaScript dialogs, rendered in-game (no native popups that minimize the game)
- Per-tab mute; DevTools
- Persistent cookies, logins, cache and site data in the mod save directory
- Microphone / camera access (for Discord voice), gated by a mod setting
- Emulated Pointer Lock so mouse-look browser games (e.g. `classic.minecraft.net`) work under off-screen rendering
- Keyboard, jump and pause input are kept in the browser while it has focus, so typing never leaks into the level
- Tabs can keep running while the frame is hidden
- Configurable home page, new-tab page, and toggle keybind (default **Ctrl+F2**)

## Build (Windows)

Requirements: Visual Studio 2022+ Build Tools with the Desktop C++ workload,
CMake, the Geode CLI, and Geode SDK 5.8.2. Build from inside the VS
`vcvars64` environment so the correct toolchain is on `PATH`:

```powershell
$env:GEODE_SDK = "C:\path\to\geode"
& "C:\Program Files (x86)\Microsoft Visual Studio\<version>\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
geode build
```

### CEF binaries

The build expects a CEF distribution in `cef/` (headers, `libcef.dll`, the
wrapper sources, and the resource/locale files). CI downloads the matching
minimal build automatically:

```
cef_binary_150.0.11+gb887805+chromium-150.0.7871.115_windows64_minimal
```

`libcef_dll_wrapper` is compiled from source by this project's `CMakeLists.txt`
rather than using CEF's own CMake, so the wrapper builds cleanly under the CI
clang toolchain (CEF's packaged flags are MSVC-only). `libcef.dll` is
delay-loaded and located at runtime with `LoadLibraryExW`.

## How it works

- A `CefClient` implements the render, life-span, display, load, download,
  context-menu, permission and JS-dialog handlers. Rendering is windowless:
  `OnPaint` hands back a BGRA buffer that the game thread uploads to a
  `CCTexture2D` via `glTexSubImage2D`.
- The page is super-sampled (2× device scale reported to CEF, downscaled on
  display) so text stays crisp instead of softly upscaled.
- A subclassed `WndProc` forwards mouse and keyboard input to CEF and, together
  with `GJBaseGameLayer`/`PauseLayer` hooks, stops that input from also reaching
  the game while the browser has focus or the cursor is over the window.
- Pointer Lock is emulated in injected JS plus a virtual-cursor warp, because
  CEF has no native pointer lock under off-screen rendering.

## Notes

This is the first release line, so it is Windows-only. The microphone/camera
setting grants those two permission kinds automatically to sites you visit —
keep it disabled when browsing untrusted pages. Some sites that actively block
embedded browsers (e.g. TikTok's captcha) will still refuse to load.
