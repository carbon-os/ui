#include <ui/webview.h>
#include <logger/logger.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace ui {

// ── Helpers: UTF-8 ↔ UTF-16 ──────────────────────────────────────────────────

static std::wstring to_wide(std::string_view s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

static std::wstring to_wide_path(std::string_view s)
{
    std::wstring w = to_wide(s);
    for (auto& c : w)
        if (c == L'/') c = L'\\';
    return w;
}

static std::string to_utf8(const wchar_t* w)
{
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static const wchar_t* wide_or_null(const std::wstring& s)
{
    return s.empty() ? nullptr : s.c_str();
}

// ── Helpers: IStream ──────────────────────────────────────────────────────────

static std::vector<uint8_t> read_stream(IStream* stream)
{
    if (!stream) return {};
    std::vector<uint8_t> out;
    uint8_t buf[4096];
    ULONG   read = 0;
    while (SUCCEEDED(stream->Read(buf, sizeof(buf), &read)) && read > 0)
        out.insert(out.end(), buf, buf + read);
    return out;
}

static ComPtr<IStream> make_stream(const std::vector<uint8_t>& data)
{
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!hg) return nullptr;
    void* ptr = GlobalLock(hg);
    if (ptr) { memcpy(ptr, data.data(), data.size()); GlobalUnlock(hg); }
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hg, TRUE, &stream))) { GlobalFree(hg); return nullptr; }
    return ComPtr<IStream>(stream);
}

// ── Helpers: JSON ─────────────────────────────────────────────────────────────

static std::string json_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += static_cast<char>(c); break;
        }
    }
    return out;
}

static std::string json_get_string(const std::string& json, std::string_view key)
{
    std::string needle = "\"";
    needle += key;
    needle += "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();

    std::string result;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') break;
        if (c == '\\' && pos < json.size()) {
            char e = json[pos++];
            switch (e) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:   result += e;    break;
            }
        } else {
            result += c;
        }
    }
    return result;
}

// ── Helpers: MIME ─────────────────────────────────────────────────────────────

static std::wstring mime_header(const std::string& ext)
{
    static const std::unordered_map<std::string, std::wstring> table = {
        {".html",  L"Content-Type: text/html; charset=utf-8"},
        {".htm",   L"Content-Type: text/html; charset=utf-8"},
        {".js",    L"Content-Type: text/javascript"},
        {".mjs",   L"Content-Type: text/javascript"},
        {".css",   L"Content-Type: text/css"},
        {".json",  L"Content-Type: application/json"},
        {".svg",   L"Content-Type: image/svg+xml"},
        {".png",   L"Content-Type: image/png"},
        {".jpg",   L"Content-Type: image/jpeg"},
        {".jpeg",  L"Content-Type: image/jpeg"},
        {".gif",   L"Content-Type: image/gif"},
        {".ico",   L"Content-Type: image/x-icon"},
        {".webp",  L"Content-Type: image/webp"},
        {".woff",  L"Content-Type: font/woff"},
        {".woff2", L"Content-Type: font/woff2"},
        {".ttf",   L"Content-Type: font/ttf"},
    };
    auto it = table.find(ext);
    return it != table.end()
        ? it->second
        : L"Content-Type: application/octet-stream";
}

// ── JS shim ───────────────────────────────────────────────────────────────────

static constexpr const char* k_shim = R"js(
window.__ui = (() => {
    const _listeners = {};

    window.chrome.webview.addEventListener('message', async (event) => {
        const msg = event.data;
        if (!msg || msg.type !== 'host_ipc_message') return;

        const { channel, text, token } = msg;

        if (text !== undefined) {
            const cb = _listeners[channel];
            if (cb) cb(text);
            return;
        }

        const res = await fetch(`ui-ipc://host/message/${channel}/${token}`);
        if (!res.ok) return;
        const buf = await res.arrayBuffer();
        const cb  = _listeners[channel];
        if (cb) cb(buf);
    });

    return {
        on(channel, cb)  { _listeners[channel] = cb; },
        off(channel)     { delete _listeners[channel]; },

        post(channel, data) {
            if (data instanceof ArrayBuffer || ArrayBuffer.isView(data)) {
                const body = data instanceof ArrayBuffer ? data : data.buffer;
                fetch('ui-ipc://js/binary/' + channel, { method: 'POST', body });
            } else {
                fetch('ui-ipc://js/text/' + channel, {
                    method: 'POST',
                    body: new TextEncoder().encode(String(data))
                });
            }
        }
    };
})();
)js";

// ── Impl ──────────────────────────────────────────────────────────────────────

enum class LoadMode { None, Html, File };

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

    // ── App content state ─────────────────────────────────────────────────────
    std::mutex  load_mutex;
    LoadMode    load_mode  = LoadMode::None;
    std::string html_src;      // load_html: the raw HTML string
    std::string file_root;     // load_file: absolute directory path
    std::string file_entry;    // load_file: entry filename (e.g. "index.html")

    EventRegistrationToken msg_token      {};
    EventRegistrationToken resource_token {};
};

// ── HWND WndProc ──────────────────────────────────────────────────────────────

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* impl = reinterpret_cast<WebViewImpl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_SIZE && impl && impl->controller) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        impl->controller->put_Bounds(rc);
        logger::Debug("WM_SIZE → bounds {}x{}", rc.right, rc.bottom);
    }

    if (msg == WM_CLOSE) {
        logger::Info("WM_CLOSE received");
        if (impl && impl->on_close_cb) {
            if (!impl->on_close_cb()) {
                logger::Info("on_close callback suppressed close");
                return 0;
            }
        }
        DestroyWindow(hwnd);
        return 0;
    }

    if (msg == WM_DESTROY) {
        logger::Info("WM_DESTROY — posting quit");
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── WebView ───────────────────────────────────────────────────────────────────

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
        to_wide(config.title).c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        config.width, config.height,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
    );
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
    std::wstring w_runtime_path  = to_wide_path(config.runtime_path);
    std::wstring w_user_data_dir = to_wide_path(config.user_data_dir);
    std::wstring w_browser_args  = to_wide(config.browser_args);

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

        // Only pages served from our own app authority can reach the IPC endpoints.
        // External URLs loaded via load_url() get no access.
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
        wide_or_null(w_runtime_path),
        wide_or_null(w_user_data_dir),
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

            logger::Info("WebView2 environment created (hr=0x{:08X})", static_cast<uint32_t>(res));
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
                        to_wide(k_shim).c_str(), nullptr);
                    logger::Debug("IPC shim injected via AddScriptToExecuteOnDocumentCreated");

                    impl_->webview->AddWebResourceRequestedFilter(
                        L"ui-ipc://*/*",
                        COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    logger::Debug("WebResourceRequestedFilter registered for ui-ipc://*/*");

                    impl_->webview->add_WebResourceRequested(
                        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                        [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT
                        {
                            ComPtr<ICoreWebView2WebResourceRequest> req;
                            args->get_Request(&req);

                            LPWSTR raw_uri = nullptr;
                            req->get_Uri(&raw_uri);
                            std::string uri = to_utf8(raw_uri);
                            CoTaskMemFree(raw_uri);

                            logger::Debug("WebResourceRequested: {}", uri);

                            // Strip "ui-ipc://" (9 chars)
                            std::string path = uri.substr(9);

                            auto first_slash = path.find('/');
                            if (first_slash == std::string::npos) return S_OK;

                            std::string authority = path.substr(0, first_slash);
                            std::string rest      = path.substr(first_slash + 1); // everything after authority/

                            auto second_slash = rest.find('/');

                            // ── ui-ipc://app — serves user content ────────────
                            if (authority == "app") {
                                std::lock_guard lock(impl_->load_mutex);

                                switch (impl_->load_mode) {

                                case LoadMode::Html: {
                                    // Serve the inline HTML for the root request only.
                                    // Relative sub-resource fetches from inline HTML that
                                    // aren't handled here will naturally 404 — that's fine,
                                    // since load_html is for self-contained documents.
                                    if (rest.empty() || rest == "/" || rest.back() == '/') {
                                        auto stream = make_stream({
                                            impl_->html_src.begin(),
                                            impl_->html_src.end()});
                                        ComPtr<ICoreWebView2WebResourceResponse> response;
                                        impl_->env->CreateWebResourceResponse(
                                            stream.Get(), 200, L"OK",
                                            L"Content-Type: text/html; charset=utf-8",
                                            &response);
                                        args->put_Response(response.Get());
                                        logger::Info("app/html: served inline HTML ({} bytes)",
                                                     impl_->html_src.size());
                                    }
                                    return S_OK;
                                }

                                case LoadMode::File: {
                                    namespace fs = std::filesystem;

                                    // `rest` is the relative path the browser requested,
                                    // e.g. "index.html", "style.css", "assets/logo.png"
                                    fs::path requested =
                                        fs::path(impl_->file_root) / rest;
                                    requested = requested.lexically_normal();

                                    // Path traversal guard: must stay inside file_root
                                    auto root_abs = fs::absolute(impl_->file_root);
                                    auto req_abs  = fs::absolute(requested);
                                    if (req_abs.string().rfind(root_abs.string(), 0) != 0) {
                                        logger::Warn("app/file: path traversal blocked: {}",
                                                     req_abs.string());
                                        return S_OK;
                                    }

                                    std::ifstream f(requested, std::ios::binary);
                                    if (!f) {
                                        logger::Warn("app/file: not found: {}",
                                                     requested.string());
                                        return S_OK;
                                    }

                                    std::vector<uint8_t> data(
                                        (std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());

                                    auto stream = make_stream(data);
                                    ComPtr<ICoreWebView2WebResourceResponse> response;
                                    impl_->env->CreateWebResourceResponse(
                                        stream.Get(), 200, L"OK",
                                        mime_header(requested.extension().string()).c_str(),
                                        &response);
                                    args->put_Response(response.Get());

                                    logger::Info("app/file: served '{}' ({} bytes)",
                                                 requested.string(), data.size());
                                    return S_OK;
                                }

                                default:
                                    logger::Warn("app: request received but load_mode is None");
                                    return S_OK;
                                }
                            }

                            // ── ui-ipc://js — JS → C++ messages ──────────────
                            if (authority == "js") {
                                if (second_slash == std::string::npos) return S_OK;

                                std::string verb    = rest.substr(0, second_slash);
                                std::string channel = rest.substr(second_slash + 1);

                                ComPtr<IStream> body_stream;
                                req->get_Content(&body_stream);
                                std::vector<uint8_t> bytes = read_stream(body_stream.Get());

                                logger::Info("JS → C++ [{}] via '{}' ({} bytes)",
                                             verb, channel, bytes.size());

                                WebView::MessageCallback cb;
                                {
                                    std::lock_guard lock(impl_->channels_mutex);
                                    auto it = impl_->channels.find(channel);
                                    if (it != impl_->channels.end())
                                        cb = it->second;
                                }

                                if (cb) {
                                    logger::Debug("dispatching to registered handler for '{}'", channel);
                                    if (verb == "text")
                                        cb(Message(std::string(bytes.begin(), bytes.end())));
                                    else
                                        cb(Message(std::move(bytes)));
                                } else {
                                    logger::Warn("no handler registered for channel '{}'", channel);
                                }

                                ComPtr<ICoreWebView2WebResourceResponse> response;
                                impl_->env->CreateWebResourceResponse(
                                    nullptr, 204, L"No Content",
                                    L"Access-Control-Allow-Origin: *",
                                    &response);
                                args->put_Response(response.Get());
                                return S_OK;
                            }

                            // ── ui-ipc://host — C++ → JS binary fetch ─────────
                            if (authority == "host") {
                                if (second_slash == std::string::npos) return S_OK;

                                std::string verb = rest.substr(0, second_slash);
                                std::string tail = rest.substr(second_slash + 1);

                                if (verb == "message") {
                                    auto token_slash = tail.find('/');
                                    if (token_slash == std::string::npos) return S_OK;

                                    std::string channel   = tail.substr(0, token_slash);
                                    std::string token_str = tail.substr(token_slash + 1);
                                    std::string slot_key  = channel + ":" + token_str;

                                    std::vector<uint8_t> data;
                                    {
                                        std::lock_guard lock(impl_->slots_mutex);
                                        auto it = impl_->slots.find(slot_key);
                                        if (it != impl_->slots.end()) {
                                            data = std::move(it->second);
                                            impl_->slots.erase(it);
                                            logger::Info("C++ → JS binary fetch: channel='{}' token={} ({} bytes)",
                                                         channel, token_str, data.size());
                                        } else {
                                            logger::Warn("binary slot not found for key '{}'", slot_key);
                                        }
                                    }

                                    ComPtr<IStream> stream = make_stream(data);
                                    ComPtr<ICoreWebView2WebResourceResponse> response;
                                    impl_->env->CreateWebResourceResponse(
                                        stream.Get(), 200, L"OK",
                                        L"Content-Type: application/octet-stream\r\n"
                                        L"Access-Control-Allow-Origin: *",
                                        &response);
                                    args->put_Response(response.Get());
                                    return S_OK;
                                }
                            }

                            logger::Warn("unhandled ui-ipc route: authority='{}'", authority);
                            return S_OK;

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
        logger::Error("CreateCoreWebView2EnvironmentWithOptions failed immediately (hr=0x{:08X})",
                      static_cast<uint32_t>(hr));
        throw std::runtime_error("ui::WebView: CreateCoreWebView2EnvironmentWithOptions failed");
    }
}

WebView::~WebView()
{
    logger::Info("WebView destructor");
    delete impl_;
}

// ── Navigation ────────────────────────────────────────────────────────────────

void WebView::load_html(std::string_view html)
{
    logger::Info("load_html: {} bytes of inline HTML", html.size());
    {
        std::lock_guard lock(impl_->load_mutex);
        impl_->load_mode  = LoadMode::Html;
        impl_->html_src   = std::string(html);
        impl_->file_root.clear();
        impl_->file_entry.clear();
    }
    // Navigate to the app root — the resource handler serves the HTML from memory
    impl_->webview->Navigate(L"ui-ipc://app/");
}

void WebView::load_file(std::string_view path)
{
    namespace fs = std::filesystem;
    fs::path p = fs::absolute(fs::path(path).lexically_normal());

    logger::Info("load_file: {}", p.string());
    {
        std::lock_guard lock(impl_->load_mutex);
        impl_->load_mode  = LoadMode::File;
        impl_->html_src.clear();
        impl_->file_root  = p.parent_path().string();
        impl_->file_entry = p.filename().string();
    }

    std::string url = "ui-ipc://app/" + p.filename().string();
    impl_->webview->Navigate(to_wide(url).c_str());
}

void WebView::load_url(std::string_view url)
{
    if (url.starts_with("ui-ipc://")) {
        logger::Error("load_url: ui-ipc:// is reserved — use load_html or load_file");
        throw std::invalid_argument("ui::WebView: load_url does not accept ui-ipc:// URLs");
    }
    logger::Info("load_url: {}", url);
    {
        std::lock_guard lock(impl_->load_mutex);
        impl_->load_mode = LoadMode::None;
        impl_->html_src.clear();
        impl_->file_root.clear();
        impl_->file_entry.clear();
    }
    impl_->webview->Navigate(to_wide(url).c_str());
}

// ── Eval ──────────────────────────────────────────────────────────────────────

void WebView::eval(std::string_view js)
{
    logger::Debug("eval: {} chars of JS", js.size());
    impl_->webview->ExecuteScript(to_wide(js).c_str(), nullptr);
}

// ── IPC: C++ → JS ────────────────────────────────────────────────────────────

void WebView::post_message(std::string_view channel, std::string_view text)
{
    logger::Info("post_message (text) → channel='{}' ({} chars)", channel, text.size());

    std::string json =
        "{\"type\":\"host_ipc_message\","
        "\"channel\":\"" + json_escape(channel) + "\","
        "\"text\":\""    + json_escape(text)    + "\"}";
    impl_->webview->PostWebMessageAsJson(to_wide(json).c_str());
}

void WebView::post_message(std::string_view channel,
                            const std::vector<uint8_t>& data)
{
    uint64_t token = impl_->next_token.fetch_add(1, std::memory_order_relaxed);
    std::string slot_key = std::string(channel) + ":" + std::to_string(token);

    {
        std::lock_guard lock(impl_->slots_mutex);
        impl_->slots[slot_key] = data;
    }

    logger::Info("post_message (binary) → channel='{}' token={} ({} bytes)",
                 channel, token, data.size());

    std::string json =
        "{\"type\":\"host_ipc_message\","
        "\"channel\":\"" + json_escape(channel)  + "\","
        "\"token\":\""   + std::to_string(token) + "\"}";
    impl_->webview->PostWebMessageAsJson(to_wide(json).c_str());
}

// ── IPC: JS → C++ ────────────────────────────────────────────────────────────

void WebView::on_message(std::string_view channel, MessageCallback cb)
{
    logger::Info("on_message: registering handler for channel '{}'", channel);
    std::lock_guard lock(impl_->channels_mutex);
    impl_->channels[std::string(channel)] = std::move(cb);
}

void WebView::off_message(std::string_view channel)
{
    logger::Info("off_message: removing handler for channel '{}'", channel);
    std::lock_guard lock(impl_->channels_mutex);
    impl_->channels.erase(std::string(channel));
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void WebView::on_ready(ReadyCallback cb)
{
    logger::Debug("on_ready callback registered");
    impl_->on_ready_cb = std::move(cb);
}

void WebView::on_close(CloseCallback cb)
{
    logger::Debug("on_close callback registered");
    impl_->on_close_cb = std::move(cb);
}

void WebView::run()
{
    logger::Info("entering message loop");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    logger::Info("message loop exited");
}

void WebView::terminate()
{
    logger::Info("terminate called — posting WM_QUIT");
    PostQuitMessage(0);
}

// ── Window ────────────────────────────────────────────────────────────────────

void WebView::set_title(std::string_view title)
{
    logger::Debug("set_title: '{}'", title);
    SetWindowTextW(impl_->hwnd, to_wide(title).c_str());
}

void WebView::set_size(int width, int height)
{
    logger::Debug("set_size: {}x{}", width, height);
    SetWindowPos(impl_->hwnd, nullptr, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOZORDER);
}

} // namespace ui