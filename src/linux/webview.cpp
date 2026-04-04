#include <ui/webview.h>
#include <logger/logger.h>

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui {

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

// ── Helpers: MIME ─────────────────────────────────────────────────────────────

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

// ── Helpers: streams ──────────────────────────────────────────────────────────

static std::vector<uint8_t> read_gstream(GInputStream* stream)
{
    if (!stream) return {};
    std::vector<uint8_t> out;
    uint8_t buf[4096];
    while (true) {
        gsize    n   = 0;
        GError*  err = nullptr;
        gboolean ok  = g_input_stream_read_all(stream, buf, sizeof(buf),
                                               &n, nullptr, &err);
        if (err) { g_error_free(err); break; }
        if (n > 0) out.insert(out.end(), buf, buf + n);
        if (!ok || n < sizeof(buf)) break;
    }
    return out;
}

static void finish_stream(WebKitURISchemeRequest* req,
                           const void* data, gsize size, const char* mime)
{
    GBytes*       bytes  = g_bytes_new(data, size);
    GInputStream* stream = g_memory_input_stream_new_from_bytes(bytes);
    g_bytes_unref(bytes);
    webkit_uri_scheme_request_finish(req, stream,
                                     static_cast<gint64>(size), mime);
    g_object_unref(stream);
}

static void finish_no_content(WebKitURISchemeRequest* req)
{
    GInputStream* empty = g_memory_input_stream_new();
    webkit_uri_scheme_request_finish(req, empty, 0, "text/plain");
    g_object_unref(empty);
}

static void finish_error(WebKitURISchemeRequest* req,
                          GIOErrorEnum code, const char* msg)
{
    webkit_uri_scheme_request_finish_error(req,
        g_error_new(G_IO_ERROR, code, "%s", msg));
}

// ── JS shim ───────────────────────────────────────────────────────────────────
//
// All fetches target ui-ipc://app/ so they are same-origin — WebKit's CORS
// check never fires.  IPC paths are reserved under the /-/ prefix which the
// app-content handler skips before falling through to file / html serving.
//
// Public surface (window.__ui) is identical to the Windows shim.

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

        // C++ → JS text: direct delivery, no fetch needed
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

// ── URI scheme handler ────────────────────────────────────────────────────────
//
// Single authority: ui-ipc://app/
//
//   /-/js/{text|binary}/{channel}       POST  — JS → C++
//   /-/host/message/{channel}/{token}   GET   — C++ → JS binary fetch
//   everything else                           — app content (html / file)

static void on_uri_scheme_request(WebKitURISchemeRequest* request,
                                   gpointer               user_data)
{
    auto* impl = static_cast<WebViewImpl*>(user_data);

    std::string uri  = webkit_uri_scheme_request_get_uri(request);
    std::string path = uri.substr(9); // strip "ui-ipc://"

    logger::Debug("WebResourceRequested: {}", uri);

    // path is now "app/..." — split authority from rest
    auto first_slash = path.find('/');
    if (first_slash == std::string::npos) {
        finish_error(request, G_IO_ERROR_INVALID_ARGUMENT, "bad uri");
        return;
    }

    const std::string authority = path.substr(0, first_slash);
    const std::string rest      = path.substr(first_slash + 1);

    if (authority != "app") {
        logger::Warn("unhandled ui-ipc authority: '{}'", authority);
        finish_error(request, G_IO_ERROR_INVALID_ARGUMENT, "unknown authority");
        return;
    }

    // ── /-/js/{verb}/{channel}  — JS → C++ ───────────────────────────────────
    if (rest.rfind("-/js/", 0) == 0) {
        const std::string ipc    = rest.substr(5); // strip "-/js/"
        const auto        vslash = ipc.find('/');
        if (vslash == std::string::npos) { finish_no_content(request); return; }

        const std::string verb    = ipc.substr(0, vslash);
        const std::string channel = ipc.substr(vslash + 1);

        std::vector<uint8_t> bytes =
            read_gstream(webkit_uri_scheme_request_get_http_body(request));

        logger::Info("JS → C++ [{}] via '{}' ({} bytes)", verb, channel, bytes.size());

        WebView::MessageCallback cb;
        {
            std::lock_guard lock(impl->channels_mutex);
            auto it = impl->channels.find(channel);
            if (it != impl->channels.end()) cb = it->second;
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

        finish_no_content(request);
        return;
    }

    // ── /-/host/message/{channel}/{token}  — C++ → JS binary fetch ───────────
    if (rest.rfind("-/host/message/", 0) == 0) {
        const std::string tail        = rest.substr(15); // strip "-/host/message/"
        const auto        token_slash = tail.find('/');
        if (token_slash == std::string::npos) { finish_no_content(request); return; }

        const std::string channel   = tail.substr(0, token_slash);
        const std::string token_str = tail.substr(token_slash + 1);
        const std::string slot_key  = channel + ":" + token_str;

        std::vector<uint8_t> data;
        {
            std::lock_guard lock(impl->slots_mutex);
            auto it = impl->slots.find(slot_key);
            if (it != impl->slots.end()) {
                data = std::move(it->second);
                impl->slots.erase(it);
                logger::Info("C++ → JS binary fetch: channel='{}' token={} ({} bytes)",
                             channel, token_str, data.size());
            } else {
                logger::Warn("binary slot not found for key '{}'", slot_key);
            }
        }

        finish_stream(request, data.data(), data.size(), "application/octet-stream");
        return;
    }

    // ── App content (Html / File) ─────────────────────────────────────────────
    std::lock_guard lock(impl->load_mutex);

    switch (impl->load_mode) {

    case LoadMode::Html: {
        if (rest.empty() || rest == "/" || rest.back() == '/') {
            finish_stream(request,
                impl->html_src.data(), impl->html_src.size(),
                "text/html; charset=utf-8");
            logger::Info("app/html: served inline HTML ({} bytes)",
                         impl->html_src.size());
        } else {
            finish_error(request, G_IO_ERROR_NOT_FOUND, "not found");
        }
        return;
    }

    case LoadMode::File: {
        namespace fs = std::filesystem;

        fs::path requested = (fs::path(impl->file_root) / rest).lexically_normal();

        // Path traversal guard: must stay inside file_root
        auto root_abs = fs::absolute(impl->file_root);
        auto req_abs  = fs::absolute(requested);
        if (req_abs.string().rfind(root_abs.string(), 0) != 0) {
            logger::Warn("app/file: path traversal blocked: {}", req_abs.string());
            finish_error(request, G_IO_ERROR_PERMISSION_DENIED, "forbidden");
            return;
        }

        std::ifstream f(requested, std::ios::binary);
        if (!f) {
            logger::Warn("app/file: not found: {}", requested.string());
            finish_error(request, G_IO_ERROR_NOT_FOUND, "not found");
            return;
        }

        std::vector<uint8_t> data(
            (std::istreambuf_iterator<char>(f)),
             std::istreambuf_iterator<char>());

        std::string mime = mime_for_ext(requested.extension().string());
        finish_stream(request, data.data(), data.size(), mime.c_str());
        logger::Info("app/file: served '{}' ({} bytes)",
                     requested.string(), data.size());
        return;
    }

    default:
        logger::Warn("app: request received but load_mode is None");
        finish_error(request, G_IO_ERROR_NOT_FOUND, "no content loaded");
        return;
    }
}

// ── WebView ───────────────────────────────────────────────────────────────────

WebView::WebView(WebViewConfig config) : impl_(new WebViewImpl())
{
    if (config.logging)
        logger::SetEnabled(true);

    logger::Info("WebView constructing — title='{}' size={}x{} debug={} logging={}",
                 config.title, config.width, config.height,
                 config.debug, config.logging);

    gtk_init(nullptr, nullptr);

    impl_->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(impl_->window), config.title.c_str());
    gtk_window_set_default_size(GTK_WINDOW(impl_->window), config.width, config.height);
    logger::Debug("GTK window created");

    // Register scheme before the WebView is created so it takes effect immediately.
    WebKitWebContext* ctx = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(ctx, "ui-ipc",
        on_uri_scheme_request, impl_, nullptr);
    logger::Debug("custom scheme 'ui-ipc' registered");

    auto* manager = webkit_user_content_manager_new();

    auto* script = webkit_user_script_new(
        k_shim,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        nullptr, nullptr);
    webkit_user_content_manager_add_script(manager, script);
    webkit_user_script_unref(script);
    logger::Debug("IPC shim injected via WebKitUserScript");

    impl_->webview = WEBKIT_WEB_VIEW(
        webkit_web_view_new_with_user_content_manager(manager));

    if (config.debug) {
        auto* settings = webkit_web_view_get_settings(impl_->webview);
        webkit_settings_set_enable_developer_extras(settings, TRUE);
        logger::Debug("DevTools enabled");
    }

    gtk_container_add(GTK_CONTAINER(impl_->window), GTK_WIDGET(impl_->webview));

    g_signal_connect(impl_->window, "delete-event",
        G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer user_data) -> gboolean {
            auto* impl = static_cast<WebViewImpl*>(user_data);
            if (impl->on_close_cb)
                return impl->on_close_cb() ? FALSE : TRUE;
            gtk_main_quit();
            return FALSE;
        }),
        impl_);

    gtk_widget_show_all(impl_->window);
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
        impl_->load_mode  = LoadMode::Html;
        impl_->html_src   = std::string(html);
        impl_->file_root.clear();
        impl_->file_entry.clear();
    }
    webkit_web_view_load_uri(impl_->webview, "ui-ipc://app/");
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
    webkit_web_view_load_uri(impl_->webview, url.c_str());
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
    webkit_web_view_load_uri(impl_->webview, std::string(url).c_str());
}

// ── Eval ──────────────────────────────────────────────────────────────────────

void WebView::eval(std::string_view js)
{
    logger::Debug("eval: {} chars of JS", js.size());
    webkit_web_view_evaluate_javascript(
        impl_->webview,
        std::string(js).c_str(), -1,
        nullptr, nullptr, nullptr,
        [](GObject* obj, GAsyncResult* res, gpointer) {
            GError* err    = nullptr;
            auto*   result = webkit_web_view_evaluate_javascript_finish(
                                 WEBKIT_WEB_VIEW(obj), res, &err);
            if (err) {
                logger::Warn("eval error: {}", err->message);
                g_error_free(err);
            }
            if (result) g_object_unref(result);
        },
        nullptr);
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
    // unsupported result type when _dispatch (an async function) is evaluated.
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
    // Fire on_ready from an idle so it always runs after the caller has
    // registered all callbacks — mirrors the ordering guarantee that the
    // Windows async controller-creation completion handler provides.
    if (impl_->on_ready_cb) {
        g_idle_add(
            [](gpointer data) -> gboolean {
                auto* cb = static_cast<ReadyCallback*>(data);
                (*cb)();
                delete cb;
                return G_SOURCE_REMOVE;
            },
            new ReadyCallback(impl_->on_ready_cb));
    }

    logger::Info("entering GTK main loop");
    gtk_main();
    logger::Info("GTK main loop exited");
}

void WebView::terminate()
{
    logger::Info("terminate called — posting gtk_main_quit");
    gtk_main_quit();
}

// ── Window ────────────────────────────────────────────────────────────────────

void WebView::set_title(std::string_view title)
{
    logger::Debug("set_title: '{}'", title);
    gtk_window_set_title(GTK_WINDOW(impl_->window), std::string(title).c_str());
}

void WebView::set_size(int width, int height)
{
    logger::Debug("set_size: {}x{}", width, height);
    gtk_window_resize(GTK_WINDOW(impl_->window), width, height);
}

} // namespace ui