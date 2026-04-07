#pragma once

#include <ui/webview.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl/client.h>
#include <WebView2.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ui {

enum class LoadMode { None, Html, File };

// Queued outbound message — built on whichever thread calls post_message(),
// drained on the UI thread when WM_APP+1 is received.
struct OutboundFrame {
    std::string          channel;
    bool                 binary = false;
    std::string          text;   // binary=false
    std::vector<uint8_t> data;   // binary=true
};

struct WebViewImpl {
    HWND                             hwnd       = nullptr;
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller>  controller;
    ComPtr<ICoreWebView2>            webview;

    WebView::ReadyCallback           on_ready_cb;
    WebView::CloseCallback           on_close_cb;

    std::mutex                                                channels_mutex;
    std::unordered_map<std::string, WebView::MessageCallback> channels;

    std::atomic<uint64_t>                                     next_token { 0 };
    std::mutex                                                slots_mutex;
    std::unordered_map<std::string, std::vector<uint8_t>>     slots;

    std::mutex                load_mutex;
    LoadMode                  load_mode  = LoadMode::None;
    std::string               html_src;
    std::string               file_root;
    std::string               file_entry;

    // Thread-safe outbound queue — any thread pushes, UI thread drains
    std::mutex                    post_mutex;
    std::queue<OutboundFrame>     post_queue;

    EventRegistrationToken msg_token      {};
    EventRegistrationToken resource_token {};
};

} // namespace ui