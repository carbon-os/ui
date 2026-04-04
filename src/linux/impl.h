#pragma once

#include <ui/webview.h>

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui {

enum class LoadMode { None, Html, File };

struct WebViewImpl {
    GtkWidget*      window  = nullptr;
    WebKitWebView*  webview = nullptr;

    WebView::ReadyCallback on_ready_cb;
    WebView::CloseCallback on_close_cb;

    std::mutex                                                channels_mutex;
    std::unordered_map<std::string, WebView::MessageCallback> channels;

    std::atomic<uint64_t>                                     next_token { 0 };
    std::mutex                                                slots_mutex;
    std::unordered_map<std::string, std::vector<uint8_t>>     slots; // "channel:token"

    std::mutex  load_mutex;
    LoadMode    load_mode  = LoadMode::None;
    std::string html_src;
    std::string file_root;
    std::string file_entry;
};

} // namespace ui