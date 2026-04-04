#include <ui/webview.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

int main()
{
    ui::WebView wv(ui::WebViewConfig{
        .title  = "IPC Example",
        .width  = 900,
        .height = 640,
        .debug  = true,
    });

    // ── JS → C++: plain text on "ping" ───────────────────────────────────────
    wv.on_message("ping", [&wv](ui::Message msg) {
        if (!msg.is_text()) return;

        std::string received(msg.text());
        std::printf("[C++] ping: %s\n", received.c_str());

        // echo a text reply back on "pong"
        wv.post_message("pong", "C++ received: " + received);
    });

    // ── JS → C++: binary on "binary-in" ──────────────────────────────────────
    wv.on_message("binary-in", [&wv](ui::Message msg) {
        if (!msg.is_binary()) return;

        const auto& bytes = msg.data();
        std::printf("[C++] binary-in: %zu bytes\n", bytes.size());

        // XOR every byte with 0xFF and send it back on "binary-out"
        std::vector<uint8_t> reply(bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i)
            reply[i] = bytes[i] ^ 0xFF;

        wv.post_message("binary-out", reply);
    });

    // ── Close handler ─────────────────────────────────────────────────────────
    wv.on_close([&wv]() -> bool {
        std::puts("[C++] window closed – shutting down");
        wv.terminate();
        return true; // returning true lets the close proceed
    });

    // ── Load the local HTML page ──────────────────────────────────────────────
    std::filesystem::path html =
        std::filesystem::path(__FILE__).parent_path() / "index.html";

    wv.load_url("file://" + html.lexically_normal().string());
    wv.run();
}