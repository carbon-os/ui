# ui

A modern C++ UI library for building high-performance, cross-platform native desktop applications.

Write your interface in HTML/CSS/JS and communicate with native C++ over a clean, typed IPC channel —
backed by the system's native webview on every platform.

```bash
git clone https://github.com/carbon-os/ui
```

```cpp
#include <ui/webview.h>

int main()
{
    ui::WebView wv({
        .title  = "my app",
        .width  = 1280,
        .height = 800,
    });

    wv.load_html(R"html(
        <!DOCTYPE html>
        <html>
        <body>
            <button onclick="window.__ui.post('ping', 'hello')">Ping</button>
        </body>
        </html>
    )html");

    wv.on_message("ping", [&](ui::Message msg) {
        wv.post_message("pong", "hello back!");
    });

    wv.on_ready([&] {
        wv.post_message("status", "ready");
    });

    wv.run();
}
```

---

## Platforms

| Platform | Engine         | Minimum Version    | Architecture        |
|----------|----------------|--------------------|---------------------|
| Linux    | WebKit2GTK 4.1 | GTK 3, GLib 2.56   | x86_64, arm64       |
| macOS    | WKWebView      | macOS 11 Big Sur   | x86_64, arm64 (M1+) |
| Windows  | WebView2       | Windows 10 1903+   | x86_64, arm64       |

---

## Building

Requires **CMake 3.22+** and a **C++20** compiler.

```bash
cmake -B build
cmake --build build
```

To include examples:

```bash
cmake -B build -DUI_BUILD_EXAMPLES=ON
cmake --build build
```

### Dependencies

**Linux**
```bash
# Debian / Ubuntu
sudo apt install libwebkit2gtk-4.1-dev

# Fedora
sudo dnf install webkit2gtk4.1-devel

# Arch
sudo pacman -S webkit2gtk-4.1
```

**Windows**

WebView2 is bundled with Windows 10 1903 and later. Pull in the SDK via vcpkg:

```bash
vcpkg install webview2
```

**macOS**

WebKit ships with the OS — no additional dependencies required.

---

## Installation

```bash
cmake --install build --prefix /usr/local
```

Installs the following:

| Artifact | Destination |
|---|---|
| `libui.a` | `lib/` |
| `include/ui/webview.h`, `include/ui/message.h` | `include/` |
| `include/logger/logger.h` | `include/` |
| `lib/cmake/ui/ui-config.cmake` | `lib/cmake/ui/` |

Consuming from another CMake project:

```cmake
find_package(ui REQUIRED)
target_link_libraries(my_app PRIVATE ui::ui)
```

---

## API

### Configuration

```cpp
ui::WebView wv({
    .title         = "my app",  // window title
    .width         = 1280,      // initial width in pixels
    .height        = 800,       // initial height in pixels
    .debug         = false,     // enable devtools
    .logging       = false,     // enable internal logger output
    .runtime_path  = "",        // WebView2: custom runtime path
    .user_data_dir = "",        // WebView2: custom user data directory
    .browser_args  = "",        // WebView2: additional browser arguments
});
```

### Loading Content

```cpp
wv.load_html("<html>...</html>");   // inline HTML
wv.load_file("path/to/index.html"); // local file — sibling assets resolve automatically
wv.load_url("https://example.com"); // external URL
```

### IPC — C++ → JS

```cpp
wv.post_message("channel", "hello");

// Binary
std::vector<uint8_t> data = { 0x01, 0x02, 0x03 };
wv.post_message("channel", data);
```

```js
window.__ui.on("channel", (payload) => {
    if (payload instanceof ArrayBuffer) {
        // binary
    } else {
        // text
    }
});
```

### IPC — JS → C++

```js
window.__ui.post("channel", "hello");

// Binary
const buf = new Uint8Array([0x01, 0x02, 0x03]).buffer;
window.__ui.post("channel", buf);
```

```cpp
wv.on_message("channel", [](ui::Message msg) {
    if (msg.is_text())   { auto s = msg.text(); }
    if (msg.is_binary()) { auto& d = msg.data(); }
});

wv.off_message("channel"); // unregister
```

### Lifecycle

```cpp
wv.on_ready([&] {
    // fired once the webview is initialised and ready to receive messages
});

wv.on_close([&]() -> bool {
    return true;  // true → allow close, false → suppress
});

wv.run();       // blocks until the window is closed
wv.terminate(); // programmatically close from any callback
```

### Window

```cpp
wv.set_title("new title");
wv.set_size(1920, 1080);
wv.eval("document.body.style.background = 'red'");
```

---

## License

MIT