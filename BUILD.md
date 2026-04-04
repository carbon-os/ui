# Building ui

## First-time setup — clone vcpkg

Run this once from the repo root:

```bash
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh    # Linux / macOS
./vcpkg/vcpkg install
```

```bat
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat   # Windows
.\vcpkg\vcpkg install
```

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
| `cmake` | build system (≥ 3.22 required) |
| `ninja-build` | faster builds, used with `-G Ninja` |
| `pkg-config` | lets CMake locate `webkit2gtk-4.1` |
| `libgtk-3-dev` | GTK3 headers + libs |
| `libwebkit2gtk-4.1-dev` | WebKit2GTK headers + libs + `jsc` |

> **Ubuntu version note** — `webkit2gtk-4.1` is available from **22.04 (Jammy)** onward.
> On 20.04 (Focal) the package is `libwebkit2gtk-4.0-dev` and you must change
> `webkit2gtk-4.1` to `webkit2gtk-4.0` in `CMakeLists.txt`.

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

### 3. Build

```bash
cmake --build build
```

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
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DUI_BUILD_EXAMPLES=ON
cmake --build build
./build/examples/ipc_example
```

---

## Windows

WebView2 is installed via vcpkg (see first-time setup above). The runtime
itself ships with Microsoft Edge and is already present on all modern
Windows 10/11 machines — nothing extra to install.

> **Air-gapped / kiosk machines** — if the target has no Edge, download and
> run the [WebView2 Evergreen Standalone Installer](https://developer.microsoft.com/en-us/microsoft-edge/webview2/)
> once on that machine.

Fixed Version
Select and package a specific version of the WebView2 Runtime with your application.


```bash
sudo apt install cabextract
wget https://msedge.sf.dl.delivery.mp.microsoft.com/filestreamingservice/files/24e2b740-e13d-4418-a307-89050e3921d1/Microsoft.WebView2.FixedVersionRuntime.146.0.3856.97.x64.cab -o webview2_runtime.cab


mkdir webview2_runtime 
cabextract -d webview2_runtime webview2_runtime.cab
```



```bat
cmake -S . -B build ^
    -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DUI_BUILD_EXAMPLES=ON
cmake --build build --config Release
.\build\examples\ipc_example.exe
```