# Building ui

## For developers of this repo

### Linux (Ubuntu / Debian)

#### 1. System dependencies
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

#### 2. Configure and build
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

#### 3. Run the example
```bash
./build/examples/ipc_example
```

---

### macOS

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

### Windows

#### 1. Install vcpkg and pull webview2

vcpkg is not bootstrapped automatically — set it up once and point CMake at it.

```bat
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

#### 2. Download the WebView2 runtime (one-time)

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

#### 3. Configure and build
```bat
cmake -S . -B build ^
    -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DUI_BUILD_EXAMPLES=ON
cmake --build build
```

#### 4. Run the example
```bat
build\examples\Debug\ipc_example.exe ^
    --webview-runtime webview2_runtime\146.0.3856.97 ^
    --load-file examples\index.html
```

For a Release build:
```bat
cmake -S . -B build ^
    -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DUI_BUILD_EXAMPLES=ON
cmake --build build --config Release
build\examples\Release\ipc_example.exe ^
    --webview-runtime webview2_runtime\146.0.3856.97 ^
    --load-file examples\index.html
```

---

## For consumers — using ui as a vcpkg dependency

`ui` is distributed as a vcpkg port via a git registry. Add the following to
your app's `vcpkg-configuration.json`:

```json
{
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "<current-vcpkg-baseline-sha>"
  },
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/YOUR_ORG/ui",
      "baseline": "<sha-from-versions-baseline.json>",
      "packages": ["ui"]
    }
  ]
}
```

Then declare the dependency in your app's `vcpkg.json`:
```json
{
  "name": "my-app",
  "version": "1.0.0",
  "dependencies": [
    "ui"
  ]
}
```

And link in CMake:
```cmake
find_package(ui CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ui::ui)
```

vcpkg will pull in webview2 automatically on Windows as a transitive
dependency — no extra steps needed in the consuming app.


## Updating port version hashes

After any change to `ports/ui/`, run:
```bash
git rev-parse HEAD:ports/ui   # → goes into versions/u-/ui.json  "git-tree"
git rev-parse HEAD            # → goes into portfile.cmake "REF"
                              #   and terminal app vcpkg-configuration.json "baseline"
```

Example:
```
git rev-parse HEAD:ports/ui
a2ea2edce8d77f26b463f9e949c852982ffbfd56   ← versions/u-/ui.json "git-tree"

git rev-parse HEAD
441599f0b5c17c3dcd412a395019ffc851506d4a   ← portfile.cmake "REF"
                                            ← terminal vcpkg-configuration.json "baseline"
```