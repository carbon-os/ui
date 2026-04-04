#pragma once

#include <WebView2.h>

namespace ui {
    struct WebViewImpl;
}

// Called from the WebResourceRequested event handler registered in webview.cpp.
// Owns all ui-ipc:// route dispatch: app content, JS→C++ messages, C++→JS
// binary fetch. Returns S_OK in all handled cases.
HRESULT handle_resource_request(
    ui::WebViewImpl*                            impl,
    ICoreWebView2WebResourceRequestedEventArgs* args);