# Building ui

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
| `cmake` | build system (≥ 3.22 required) |
| `ninja-build` | faster builds, used with `-G Ninja` |
| `pkg-config` | lets CMake locate `webkit2gtk-4.1` |
| `libgtk-3-dev` | GTK3 headers + libs |
| `libwebkit2gtk-4.1-dev` | WebKit2GTK headers + libs + `jsc` |

> **Ubuntu version note** — `webkit2gtk-4.1` is available from **22.04 (Jammy)** onward.
> On 20.04 (Focal) the package is `libwebkit2gtk-4.0-dev` and you must change
> `webkit2gtk-4.1` to `webkit2gtk-4.0` in `CMakeLists.txt`.

---

### 2. Configure

```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
```

To also build the IPC example:

```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DUI_BUILD_EXAMPLES=ON
```

---

### 3. Build

```bash
cmake --build build
```

---

### 4. Run the example

```bash
./build/examples/ipc_example
```

---

## macOS

Install Xcode command-line tools and CMake — no extra package installs are
needed because `WebKit.framework` and `Cocoa.framework` ship with the OS.

```bash
xcode-select --install
brew install cmake ninja
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUI_BUILD_EXAMPLES=ON
cmake --build build
./build/examples/ipc_example
```

---

## Windows

Install the [WebView2 SDK](https://developer.microsoft.com/microsoft-edge/webview2/)
via NuGet and place `WebView2Loader.dll` somewhere on `PATH` (or next to the
binary). Then configure with the Visual Studio generator:

```bat
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUI_BUILD_EXAMPLES=ON
cmake --build build --config Release
```