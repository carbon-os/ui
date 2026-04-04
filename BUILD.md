# Building ui

## First-time setup

Nothing to do manually for vcpkg — CMake clones and bootstraps it automatically
on the first configure. Just call cmake and it handles everything.

The only things that are never automated are:
- **Linux** system packages, because they require sudo (see below).
- **Windows** WebView2 runtime binaries — run the one-time download command
  below before building.

---

## Linux (Ubuntu / Debian)

### 1. System dependencies
```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    libgtk-3-dev \
    libwebkit2gtk-4.1-dev
```

| Package | Provides |
|---|---|
| `build-essential` | `g++`, `make`, `libc-dev` |
| `cmake` | build system (>= 3.22 required) |
| `ninja-build` | faster builds |
| `pkg-config` | lets CMake locate `webkit2gtk-4.1` |
| `libgtk-3-dev` | GTK3 headers + libs |
| `libwebkit2gtk-4.1-dev` | WebKit2GTK headers + libs |

> **Ubuntu version note** — `webkit2gtk-4.1` is available from **22.04 (Jammy)**
> onward. On 20.04 (Focal) the package is `libwebkit2gtk-4.0-dev` and you must
> change `webkit2gtk-4.1` to `webkit2gtk-4.0` in `CMakeLists.txt`.

### 2. Configure and build
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To also build the examples:
```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DUI_BUILD_EXAMPLES=ON
cmake --build build
```

### 3. Run the example
```bash
./build/examples/ipc_example
```

---

## macOS

Xcode command-line tools and CMake are all that is needed. WebKit and Cocoa
ship with the OS.
```bash
xcode-select --install
brew install cmake ninja
```
```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DUI_BUILD_EXAMPLES=ON
cmake --build build
```
```bash
./build/examples/ipc_example
```

---

## Windows

### 1. Download the WebView2 runtime (one-time)

The WebView2 SDK (headers + import lib) comes from vcpkg automatically.
The runtime binaries must be downloaded separately before your first build.
Run this once from the repo root:
```bat
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\setup.ps1
```

This downloads the runtime into `webview2_runtime\146.0.3856.97\`.
To fetch a different version, pass `-Version`:
```bat
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\setup.ps1 -Version 146.0.3856.97
```

### 2. Configure and build
```bat
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DUI_BUILD_EXAMPLES=ON
cmake --build build
```

### 3. Run the example
```bat
build\examples\Debug\ipc_example.exe ^
    --webview-runtime C:\Users\cloud\Desktop\ui\webview2_runtime\146.0.3856.97 ^
    --load-file C:\Users\cloud\Desktop\ui\examples\index.html
```

For a Release build, swap `Debug` for `Release` in both the cmake configure
step and the exe path:
```bat
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUI_BUILD_EXAMPLES=ON
cmake --build build --config Release
build\examples\Release\ipc_example.exe ^
    --webview-runtime webview2_runtime\146.0.3856.97 ^
    --load-file examples\index.html
```