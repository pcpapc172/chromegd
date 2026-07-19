# Chromium in GD

A Windows-only Chromium browser overlay for Geometry Dash **2.2081** and Geode **5.8.2**.

The frame follows Relog's floating-window idea: it stays above scenes, can be dragged by its top strip, resized from the lower-right handle, and remembers its size and position. The page area is a real WebView2 Chromium surface attached to the Geometry Dash window.

## Features

- Multiple tabs, links that request a new window open as a new tab
- Address/search box, back, forward, reload, and close-tab controls
- Per-tab mute
- Chromium DevTools (`Tools`)
- Persistent cookies, logins, cache, and site data in the mod save directory
- Audio, video, downloads, clipboard, context menus, and JavaScript dialogs
- Microphone/camera access for Discord voice chat (controlled by the mod setting)
- Tabs can keep running while the frame is hidden
- Default shortcut: **Ctrl+F2**

## Build (Windows)

Requirements: Visual Studio 2022 Build Tools with Desktop C++, CMake, Ninja, Geode CLI, and Geode SDK 5.8.2.

```powershell
$env:GEODE_SDK = "C:\path\to\geode"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

CMake downloads the official Microsoft WebView2 SDK package and statically links its loader. It does not bundle a browser runtime. Windows 10/11 normally already has the Evergreen WebView2 Runtime; if startup reports that it is missing, install the Evergreen Runtime from Microsoft's official WebView2 download page.

The resulting `.geode` package is placed in the build output and can be installed with:

```powershell
geode mod install build\pcpapc172.chromium-in-gd.geode
```

## Important behavior

This is a native child Chromium window, not a Cocos texture imitation. That is what makes Discord voice, IME/text input, video acceleration, downloads, browser menus, and DevTools practical. Because of that, the first release targets Windows only and the browser surface is briefly hidden while its frame is being dragged or resized.

The microphone/camera toggle automatically grants those two permission kinds to sites you visit. Keep it disabled when browsing untrusted pages. Other permissions stay on WebView2's default behavior.

