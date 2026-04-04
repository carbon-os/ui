#include "impl.h"
#include "scheme.h"
#include "wstring.h"
#include "shim.h"

#include <ui/webview.h>
#include <logger/logger.h>

#include <WebView2EnvironmentOptions.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include <cstdio>
#include <stdexcept>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

// Defined in window.cpp — same translation unit group, same impl.h
namespace ui { LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM); }

namespace ui {

WebView::WebView(WebViewConfig config) : impl_(new WebViewImpl())
{
    if (config.logging)
        logger::SetEnabled(true);

    logger::Info("WebView constructing — title='{}' size={}x{} debug={} logging={}",
                 config.title, config.width, config.height,
                 config.debug, config.logging);

    // ── COM ───────────────────────────────────────────────────────────────────
    HRESULT hr_com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr_com == S_OK) {
        logger::Debug("CoInitializeEx succeeded (hr=0x{:08X})", static_cast<uint32_t>(hr_com));
    } else if (hr_com == S_FALSE || hr_com == RPC_E_CHANGED_MODE) {
        logger::Warn("CoInitializeEx: COM already initialised (hr=0x{:08X}) — continuing",
                     static_cast<uint32_t>(hr_com));
    } else {
        logger::Error("CoInitializeEx failed (hr=0x{:08X})", static_cast<uint32_t>(hr_com));
        throw std::runtime_error("ui::WebView: CoInitializeEx failed");
    }

    // ── Window class ──────────────────────────────────────────────────────────
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = wnd_proc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ui_webview";
        wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        registered = true;
        logger::Debug("WNDCLASSEX 'ui_webview' registered");
    }

    // ── Native window ─────────────────────────────────────────────────────────
    impl_->hwnd = CreateWindowExW(
        0, L"ui_webview",
        win::to_wide(config.title).c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        config.width, config.height,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!impl_->hwnd) {
        logger::Error("CreateWindowEx failed");
        throw std::runtime_error("ui::WebView: CreateWindowEx failed");
    }
    logger::Info("native HWND created (hwnd=0x{:X})",
                 reinterpret_cast<uintptr_t>(impl_->hwnd));

    SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(impl_));
    ShowWindow(impl_->hwnd, SW_SHOW);
    UpdateWindow(impl_->hwnd);

    // ── Paths ─────────────────────────────────────────────────────────────────
    std::wstring w_runtime_path  = win::to_wide_path(config.runtime_path);
    std::wstring w_user_data_dir = win::to_wide_path(config.user_data_dir);
    std::wstring w_browser_args  = win::to_wide(config.browser_args);

    logger::Debug("runtime_path  = '{}'", config.runtime_path.empty() ? "(evergreen)" : config.runtime_path);
    logger::Debug("user_data_dir = '{}'", config.user_data_dir.empty() ? "(default)"   : config.user_data_dir);
    logger::Debug("browser_args  = '{}'", config.browser_args.empty()  ? "(none)"      : config.browser_args);

    // ── Environment options ───────────────────────────────────────────────────
    auto opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (!w_browser_args.empty())
        opts->put_AdditionalBrowserArguments(w_browser_args.c_str());

    ComPtr<ICoreWebView2EnvironmentOptions4> opts4;
    if (SUCCEEDED(opts->QueryInterface(IID_PPV_ARGS(&opts4)))) {
        auto reg = Microsoft::WRL::Make<CoreWebView2CustomSchemeRegistration>(L"ui-ipc");
        reg->put_TreatAsSecure(TRUE);
        reg->put_HasAuthorityComponent(TRUE);

        LPCWSTR origins[] = { L"ui-ipc://app" };
        reg->SetAllowedOrigins(1, origins);

        ICoreWebView2CustomSchemeRegistration* regs[] = { reg.Get() };
        opts4->SetCustomSchemeRegistrations(1, regs);
        logger::Debug("custom scheme 'ui-ipc' registered (allowed origin: ui-ipc://app)");
    } else {
        logger::Warn("ICoreWebView2EnvironmentOptions4 unavailable — custom scheme not registered");
    }

    // ── Create environment ────────────────────────────────────────────────────
    logger::Info("calling CreateCoreWebView2EnvironmentWithOptions...");

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        win::wide_or_null(w_runtime_path),
        win::wide_or_null(w_user_data_dir),
        opts.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [this, debug = config.debug](HRESULT res, ICoreWebView2Environment* env) -> HRESULT
        {
            if (FAILED(res) || !env) {
                logger::Error("environment creation failed (hr=0x{:08X})", static_cast<uint32_t>(res));
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                    "ui::WebView: environment creation failed (HRESULT 0x%08lX)."
                    " Check runtime_path / user_data_dir in WebViewConfig.", res);
                throw std::runtime_error(msg);
            }

            logger::Info("WebView2 environment created");
            impl_->env = env;

            return env->CreateCoreWebView2Controller(impl_->hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [this, debug](HRESULT res2, ICoreWebView2Controller* ctrl) -> HRESULT
                {
                    if (FAILED(res2) || !ctrl) {
                        logger::Error("controller creation failed (hr=0x{:08X})", static_cast<uint32_t>(res2));
                        return res2;
                    }

                    logger::Info("WebView2 controller created");
                    impl_->controller = ctrl;
                    ctrl->get_CoreWebView2(&impl_->webview);

                    RECT rc;
                    GetClientRect(impl_->hwnd, &rc);
                    ctrl->put_Bounds(rc);
                    logger::Debug("initial bounds set to {}x{}", rc.right, rc.bottom);

                    if (debug) {
                        ComPtr<ICoreWebView2Settings> settings;
                        impl_->webview->get_Settings(&settings);
                        settings->put_AreDevToolsEnabled(TRUE);
                        logger::Debug("DevTools enabled");
                    }

                    impl_->webview->AddScriptToExecuteOnDocumentCreated(
                        win::to_wide(win::k_shim).c_str(), nullptr);
                    logger::Debug("IPC shim injected via AddScriptToExecuteOnDocumentCreated");

                    impl_->webview->AddWebResourceRequestedFilter(
                        L"ui-ipc://*/*",
                        COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

                    impl_->webview->add_WebResourceRequested(
                        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                        [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                            return handle_resource_request(impl_, args);
                        }).Get(),
                        &impl_->resource_token);

                    logger::Info("WebView2 fully initialised");

                    if (impl_->on_ready_cb) {
                        logger::Debug("firing on_ready callback");
                        impl_->on_ready_cb();
                    }

                    return S_OK;
                }).Get());
        }).Get());

    if (FAILED(hr)) {
        logger::Error("CreateCoreWebView2EnvironmentWithOptions failed (hr=0x{:08X})",
                      static_cast<uint32_t>(hr));
        throw std::runtime_error("ui::WebView: CreateCoreWebView2EnvironmentWithOptions failed");
    }
}

WebView::~WebView()
{
    logger::Info("WebView destructor");
    delete impl_;
}

void WebView::eval(std::string_view js)
{
    logger::Debug("eval: {} chars of JS", js.size());
    impl_->webview->ExecuteScript(win::to_wide(js).c_str(), nullptr);
}

} // namespace ui