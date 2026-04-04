#pragma once

#include <ui/webview.h>

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui {

enum class LoadMode { None, Html, File };

struct WebViewImpl {
    NSWindow*   window  = nullptr;
    WKWebView*  webview = nullptr;

    WebView::ReadyCallback on_ready_cb;
    WebView::CloseCallback on_close_cb;

    std::mutex                                                channels_mutex;
    std::unordered_map<std::string, WebView::MessageCallback> channels;

    std::atomic<uint64_t>                                     next_token { 0 };
    std::mutex                                                slots_mutex;
    std::unordered_map<std::string, std::vector<uint8_t>>     slots;

    std::mutex  load_mutex;
    LoadMode    load_mode  = LoadMode::None;
    std::string html_src;
    std::string file_root;
    std::string file_entry;

    id __strong scheme_handler  = nil;
    id __strong window_delegate = nil;
};

} // namespace ui