#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "message.h"

namespace ui {

// Forward-declared at namespace scope so the pimpl struct is reachable
// from the free C callbacks in the .cpp without a private-access violation.
struct WebViewImpl;

struct WebViewConfig {
    std::string title  = "ui";
    int         width  = 1280;
    int         height = 800;
    bool        debug  = false;
};

class WebView {
public:
    using MessageCallback = std::function<void(Message)>;
    using CloseCallback   = std::function<bool()>;

    explicit WebView(WebViewConfig config = {});
    ~WebView();

    WebView(const WebView&)            = delete;
    WebView& operator=(const WebView&) = delete;
    WebView(WebView&&)                 = default;
    WebView& operator=(WebView&&)      = default;

    // ── Navigation ────────────────────────────────────────────────────────────
    void load_url(std::string_view url);
    void load_html(std::string_view html);

    // ── Eval ──────────────────────────────────────────────────────────────────
    void eval(std::string_view js);

    // ── IPC: C++ → JS (channel based) ────────────────────────────────────────
    void post_message(std::string_view channel, std::string_view text);
    void post_message(std::string_view channel, const std::vector<uint8_t>& data);

    // ── IPC: JS → C++ (channel based) ────────────────────────────────────────
    void on_message(std::string_view channel, MessageCallback cb);
    void off_message(std::string_view channel);

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    void on_close(CloseCallback cb);
    void run();
    void terminate();

    // ── Window ────────────────────────────────────────────────────────────────
    void set_title(std::string_view title);
    void set_size(int width, int height);

private:
    WebViewImpl* impl_;
};

} // namespace ui