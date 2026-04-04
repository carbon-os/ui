#include <ui/webview.h>
#include <logger/logger.h>

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ── C++ helpers ───────────────────────────────────────────────────────────────

namespace ui {

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

static std::string mime_for_ext(const std::string& ext)
{
    static const std::unordered_map<std::string, std::string> table = {
        {".html",  "text/html; charset=utf-8"},
        {".htm",   "text/html; charset=utf-8"},
        {".js",    "text/javascript"},
        {".mjs",   "text/javascript"},
        {".css",   "text/css"},
        {".json",  "application/json"},
        {".svg",   "image/svg+xml"},
        {".png",   "image/png"},
        {".jpg",   "image/jpeg"},
        {".jpeg",  "image/jpeg"},
        {".gif",   "image/gif"},
        {".ico",   "image/x-icon"},
        {".webp",  "image/webp"},
        {".woff",  "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf",   "font/ttf"},
    };
    auto it = table.find(ext);
    return it != table.end() ? it->second : "application/octet-stream";
}

static std::vector<uint8_t> read_request_body(NSURLRequest* req)
{
    if (NSData* body = req.HTTPBody; body && body.length > 0) {
        const auto* bytes = static_cast<const uint8_t*>(body.bytes);
        return {bytes, bytes + body.length};
    }
    if (NSInputStream* stream = req.HTTPBodyStream) {
        [stream open];
        std::vector<uint8_t> out;
        uint8_t buf[4096];
        NSInteger n;
        while ((n = [stream read:buf maxLength:sizeof(buf)]) > 0)
            out.insert(out.end(), buf, buf + n);
        [stream close];
        return out;
    }
    return {};
}

// ── JS shim ───────────────────────────────────────────────────────────────────
//
// Mirrors the Linux shim exactly: single authority ui-ipc://app/ with /-/
// prefixed paths for IPC routes. WKWebView has no chrome.webview so we use
// the same fetch-based approach as the GTK backend.

static constexpr const char* k_shim = R"js(
window.__ui = (() => {
    const _listeners = {};

    return {
        on(channel, cb)  { _listeners[channel] = cb; },
        off(channel)     { delete _listeners[channel]; },

        // C++ → JS binary: fetch payload, deliver as ArrayBuffer
        async _dispatch(channel, token) {
            const res = await fetch(
                'ui-ipc://app/-/host/message/' + channel + '/' + token);
            if (!res.ok) return;
            const buf = await res.arrayBuffer();
            const cb  = _listeners[channel];
            if (cb) cb(buf);
        },

        // C++ → JS text: direct delivery via eval, no fetch needed
        _dispatchText(channel, text) {
            const cb = _listeners[channel];
            if (cb) cb(text);
        },

        // JS → C++
        post(channel, data) {
            if (data instanceof ArrayBuffer || ArrayBuffer.isView(data)) {
                const body = data instanceof ArrayBuffer ? data : data.buffer;
                fetch('ui-ipc://app/-/js/binary/' + channel,
                      { method: 'POST', body });
            } else {
                fetch('ui-ipc://app/-/js/text/' + channel, {
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

    // Strong references to Objective-C delegates (ARC keeps them alive)
    id __strong scheme_handler  = nil;
    id __strong window_delegate = nil;
};

} // namespace ui

// ── Objective-C: scheme handler ───────────────────────────────────────────────

@interface UISchemeHandler : NSObject <WKURLSchemeHandler> {
    ui::WebViewImpl* _impl;
    NSMutableSet*    _active; // tasks currently in-flight; main thread only
}
- (instancetype)initWithImpl:(ui::WebViewImpl*)impl;
@end

@implementation UISchemeHandler

- (instancetype)initWithImpl:(ui::WebViewImpl*)impl
{
    self   = [super init];
    _impl  = impl;
    _active = [NSMutableSet new];
    return self;
}

// ── Route dispatch ────────────────────────────────────────────────────────────

- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task
{
    [_active addObject:task];

    NSURLRequest* req = task.request;
    std::string uri   = req.URL.absoluteString.UTF8String ? req.URL.absoluteString.UTF8String : "";

    logger::Debug("WebResourceRequested: {}", uri);

    // Strip "ui-ipc://" prefix (9 chars)
    if (uri.size() < 9) { [self error:task code:400]; return; }
    std::string path = uri.substr(9);

    auto first_slash = path.find('/');
    if (first_slash == std::string::npos) { [self error:task code:400]; return; }

    const std::string authority = path.substr(0, first_slash);
    const std::string rest      = path.substr(first_slash + 1);

    if (authority != "app") {
        logger::Warn("unhandled ui-ipc authority: '{}'", authority);
        [self error:task code:400];
        return;
    }

    // ── /-/js/{verb}/{channel}  — JS → C++ ───────────────────────────────────
    if (rest.rfind("-/js/", 0) == 0) {
        const std::string ipc    = rest.substr(5);
        const auto        vslash = ipc.find('/');
        if (vslash == std::string::npos) { [self noContent:task]; return; }

        const std::string verb    = ipc.substr(0, vslash);
        const std::string channel = ipc.substr(vslash + 1);

        std::vector<uint8_t> bytes = ui::read_request_body(req);

        logger::Info("JS → C++ [{}] via '{}' ({} bytes)", verb, channel, bytes.size());

        ui::WebView::MessageCallback cb;
        {
            std::lock_guard lock(_impl->channels_mutex);
            auto it = _impl->channels.find(channel);
            if (it != _impl->channels.end()) cb = it->second;
        }

        if (cb) {
            logger::Debug("dispatching to registered handler for '{}'", channel);
            if (verb == "text")
                cb(ui::Message(std::string(bytes.begin(), bytes.end())));
            else
                cb(ui::Message(std::move(bytes)));
        } else {
            logger::Warn("no handler registered for channel '{}'", channel);
        }

        [self noContent:task];
        return;
    }

    // ── /-/host/message/{channel}/{token}  — C++ → JS binary fetch ───────────
    if (rest.rfind("-/host/message/", 0) == 0) {
        const std::string tail      = rest.substr(15);
        const auto        tok_slash = tail.find('/');
        if (tok_slash == std::string::npos) { [self noContent:task]; return; }

        const std::string channel   = tail.substr(0, tok_slash);
        const std::string token_str = tail.substr(tok_slash + 1);
        const std::string slot_key  = channel + ":" + token_str;

        std::vector<uint8_t> data;
        {
            std::lock_guard lock(_impl->slots_mutex);
            auto it = _impl->slots.find(slot_key);
            if (it != _impl->slots.end()) {
                data = std::move(it->second);
                _impl->slots.erase(it);
                logger::Info("C++ → JS binary fetch: channel='{}' token={} ({} bytes)",
                             channel, token_str, data.size());
            } else {
                logger::Warn("binary slot not found for key '{}'", slot_key);
            }
        }

        NSData* ns_data = [NSData dataWithBytes:data.data() length:data.size()];
        [self respond:task data:ns_data mime:@"application/octet-stream" status:200];
        return;
    }

    // ── App content (Html / File) ─────────────────────────────────────────────
    std::lock_guard lock(_impl->load_mutex);

    switch (_impl->load_mode) {

    case ui::LoadMode::Html: {
        if (rest.empty() || rest == "/" || rest.back() == '/') {
            NSData* data = [NSData dataWithBytes:_impl->html_src.data()
                                          length:_impl->html_src.size()];
            logger::Info("app/html: served inline HTML ({} bytes)", _impl->html_src.size());
            [self respond:task data:data mime:@"text/html; charset=utf-8" status:200];
        } else {
            [self error:task code:404];
        }
        return;
    }

    case ui::LoadMode::File: {
        namespace fs = std::filesystem;

        fs::path requested = (fs::path(_impl->file_root) / rest).lexically_normal();
        auto     root_abs  = fs::absolute(_impl->file_root);
        auto     req_abs   = fs::absolute(requested);

        if (req_abs.string().rfind(root_abs.string(), 0) != 0) {
            logger::Warn("app/file: path traversal blocked: {}", req_abs.string());
            [self error:task code:403];
            return;
        }

        std::ifstream f(requested, std::ios::binary);
        if (!f) {
            logger::Warn("app/file: not found: {}", requested.string());
            [self error:task code:404];
            return;
        }

        std::vector<uint8_t> file_data(
            (std::istreambuf_iterator<char>(f)),
             std::istreambuf_iterator<char>());

        NSData*   ns_data = [NSData dataWithBytes:file_data.data() length:file_data.size()];
        NSString* ns_mime = [NSString stringWithUTF8String:
                                ui::mime_for_ext(requested.extension().string()).c_str()];

        logger::Info("app/file: served '{}' ({} bytes)", requested.string(), file_data.size());
        [self respond:task data:ns_data mime:ns_mime status:200];
        return;
    }

    default:
        logger::Warn("app: request received but load_mode is None");
        [self error:task code:404];
        return;
    }
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task
{
    // Task was cancelled — remove it so the finish helpers become no-ops.
    [_active removeObject:task];
}

// ── Private response helpers ──────────────────────────────────────────────────

- (void)respond:(id<WKURLSchemeTask>)task
           data:(NSData*)data
           mime:(NSString*)mime
         status:(NSInteger)status
{
    if (![_active containsObject:task]) return;

    NSDictionary* headers = @{
        @"Content-Type":                mime,
        @"Access-Control-Allow-Origin": @"*",
    };
    NSHTTPURLResponse* resp =
        [[NSHTTPURLResponse alloc] initWithURL:task.request.URL
                                    statusCode:status
                                   HTTPVersion:@"HTTP/1.1"
                                  headerFields:headers];
    [task didReceiveResponse:resp];
    [task didReceiveData:data ?: [NSData data]];
    [task didFinish];
    [_active removeObject:task];
}

- (void)noContent:(id<WKURLSchemeTask>)task
{
    if (![_active containsObject:task]) return;

    NSHTTPURLResponse* resp =
        [[NSHTTPURLResponse alloc] initWithURL:task.request.URL
                                    statusCode:204
                                   HTTPVersion:@"HTTP/1.1"
                                  headerFields:@{}];
    [task didReceiveResponse:resp];
    [task didReceiveData:[NSData data]];
    [task didFinish];
    [_active removeObject:task];
}

- (void)error:(id<WKURLSchemeTask>)task code:(NSInteger)code
{
    if (![_active containsObject:task]) return;

    [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain
                                               code:code
                                           userInfo:nil]];
    [_active removeObject:task];
}

@end

// ── Objective-C: window delegate ──────────────────────────────────────────────

@interface UIWindowDelegate : NSObject <NSWindowDelegate> {
    ui::WebViewImpl* _impl;
}
- (instancetype)initWithImpl:(ui::WebViewImpl*)impl;
@end

@implementation UIWindowDelegate

- (instancetype)initWithImpl:(ui::WebViewImpl*)impl
{
    self  = [super init];
    _impl = impl;
    return self;
}

- (BOOL)windowShouldClose:(NSWindow*)sender
{
    logger::Info("windowShouldClose received");
    if (_impl->on_close_cb) {
        if (!_impl->on_close_cb()) {
            logger::Info("on_close callback suppressed close");
            return NO;
        }
    }
    return YES;
}

- (void)windowWillClose:(NSNotification*)notification
{
    logger::Info("windowWillClose — stopping run loop");
    [NSApp stop:nil];
    // Unblock the event loop which may be waiting for the next event.
    NSEvent* dummy = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSMakePoint(0, 0)
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:dummy atStart:YES];
}

@end

// ── WebView ───────────────────────────────────────────────────────────────────

namespace ui {

WebView::WebView(WebViewConfig config) : impl_(new WebViewImpl())
{
    if (config.logging)
        logger::SetEnabled(true);

    logger::Info("WebView constructing — title='{}' size={}x{} debug={} logging={}",
                 config.title, config.width, config.height,
                 config.debug, config.logging);

    // Ensure a shared NSApplication exists and is configured as a regular app
    // (shows in the Dock, can become key). Safe to call multiple times.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // ── Window ────────────────────────────────────────────────────────────────
    NSRect frame = NSMakeRect(0, 0,
                              static_cast<CGFloat>(config.width),
                              static_cast<CGFloat>(config.height));

    NSWindowStyleMask style = NSWindowStyleMaskTitled
                            | NSWindowStyleMaskClosable
                            | NSWindowStyleMaskMiniaturizable
                            | NSWindowStyleMaskResizable;

    impl_->window = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    [impl_->window setTitle:[NSString stringWithUTF8String:config.title.c_str()]];
    [impl_->window center];
    logger::Debug("NSWindow created");

    auto* win_delegate = [[UIWindowDelegate alloc] initWithImpl:impl_];
    impl_->window_delegate = win_delegate;
    [impl_->window setDelegate:win_delegate];

    // ── WKWebView configuration ───────────────────────────────────────────────
    WKWebViewConfiguration* wk_config = [[WKWebViewConfiguration alloc] init];

    // Custom scheme handler — receives every ui-ipc:// request
    auto* handler = [[UISchemeHandler alloc] initWithImpl:impl_];
    impl_->scheme_handler = handler;
    [wk_config setURLSchemeHandler:handler forURLScheme:@"ui-ipc"];
    logger::Debug("custom scheme 'ui-ipc' registered");

    // Inject the IPC shim at document start into all frames
    WKUserScript* shim =
        [[WKUserScript alloc] initWithSource:[NSString stringWithUTF8String:k_shim]
                               injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                            forMainFrameOnly:NO];
    [wk_config.userContentController addUserScript:shim];
    logger::Debug("IPC shim injected via WKUserScript");

    if (config.debug) {
        // developerExtrasEnabled is a well-known KVC key for WKPreferences that
        // enables the right-click "Inspect Element" menu and the Web Inspector.
        [wk_config.preferences setValue:@YES forKey:@"developerExtrasEnabled"];
        logger::Debug("DevTools enabled");
    }

    // ── WKWebView ─────────────────────────────────────────────────────────────
    impl_->webview = [[WKWebView alloc] initWithFrame:frame
                                        configuration:wk_config];
    [impl_->window setContentView:impl_->webview];
    [impl_->window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    logger::Info("WebView fully initialised");
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
        impl_->load_mode = LoadMode::Html;
        impl_->html_src  = std::string(html);
        impl_->file_root.clear();
        impl_->file_entry.clear();
    }
    [impl_->webview loadRequest:
        [NSURLRequest requestWithURL:[NSURL URLWithString:@"ui-ipc://app/"]]];
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
    [impl_->webview loadRequest:
        [NSURLRequest requestWithURL:[NSURL URLWithString:
            [NSString stringWithUTF8String:url.c_str()]]]];
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
    [impl_->webview loadRequest:
        [NSURLRequest requestWithURL:[NSURL URLWithString:
            [NSString stringWithUTF8String:std::string(url).c_str()]]]];
}

// ── Eval ──────────────────────────────────────────────────────────────────────

void WebView::eval(std::string_view js)
{
    logger::Debug("eval: {} chars of JS", js.size());
    NSString* src = [NSString stringWithUTF8String:std::string(js).c_str()];
    [impl_->webview evaluateJavaScript:src completionHandler:^(id, NSError* err) {
        if (err)
            logger::Warn("eval error: {}", err.localizedDescription.UTF8String);
    }];
}

// ── IPC: C++ → JS ────────────────────────────────────────────────────────────

void WebView::post_message(std::string_view channel, std::string_view text)
{
    logger::Info("post_message (text) → channel='{}' ({} chars)", channel, text.size());

    std::string js =
        "window.__ui._dispatchText(\""
        + json_escape(channel) + "\",\""
        + json_escape(text)    + "\")";
    eval(js);
}

void WebView::post_message(std::string_view channel,
                            const std::vector<uint8_t>& data)
{
    uint64_t    token    = impl_->next_token.fetch_add(1, std::memory_order_relaxed);
    std::string slot_key = std::string(channel) + ":" + std::to_string(token);

    {
        std::lock_guard lock(impl_->slots_mutex);
        impl_->slots[slot_key] = data;
    }

    logger::Info("post_message (binary) → channel='{}' token={} ({} bytes)",
                 channel, token, data.size());

    // void discards the Promise so WebKit's eval callback doesn't warn about
    // an unsupported result type when _dispatch (an async function) is evaluated.
    std::string js =
        "void window.__ui._dispatch(\""
        + json_escape(channel)  + "\",\""
        + std::to_string(token) + "\")";
    eval(js);
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
    // Fire on_ready from the next run-loop iteration so it always executes
    // after the caller has registered all callbacks — mirrors the ordering
    // guarantee that g_idle_add provides on Linux.
    if (impl_->on_ready_cb) {
        auto cb = impl_->on_ready_cb;
        dispatch_async(dispatch_get_main_queue(), ^{ cb(); });
    }

    logger::Info("entering NSApp run loop");
    [NSApp run];
    logger::Info("NSApp run loop exited");
}

void WebView::terminate()
{
    logger::Info("terminate called — stopping NSApp run loop");
    [NSApp stop:nil];
    // Post a dummy event so the run loop unblocks immediately rather than
    // waiting for the next real user event to notice the stop request.
    NSEvent* dummy = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSMakePoint(0, 0)
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:dummy atStart:YES];
}

// ── Window ────────────────────────────────────────────────────────────────────

void WebView::set_title(std::string_view title)
{
    logger::Debug("set_title: '{}'", title);
    [impl_->window setTitle:[NSString stringWithUTF8String:std::string(title).c_str()]];
}

void WebView::set_size(int width, int height)
{
    logger::Debug("set_size: {}x{}", width, height);
    NSRect frame = impl_->window.frame;
    frame.size   = NSMakeSize(static_cast<CGFloat>(width),
                              static_cast<CGFloat>(height));
    [impl_->window setFrame:frame display:YES];
}

} // namespace ui