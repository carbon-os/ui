#include <ui/webview.h>

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <random>
#include <sstream>

namespace ui {

// ── Impl ──────────────────────────────────────────────────────────────────────
// Defined at namespace scope (forward-declared in webview.h as WebViewImpl)
// so that the static C callbacks below can name the type without hitting
// a private-nested-type access error.

struct WebViewImpl {
    GtkWidget*      window  = nullptr;
    WebKitWebView*  webview = nullptr;
    WebView::CloseCallback   on_close_cb;

    // channel → callback (JS → C++)
    std::mutex                                           channels_mutex;
    std::unordered_map<std::string, WebView::MessageCallback> channels;

    // token → {channel, data}  (C++ → JS pending binary)
    struct Pending {
        std::string          channel;
        std::vector<uint8_t> data;
    };
    std::mutex                               pending_mutex;
    std::unordered_map<std::string, Pending> pending;
};

// ── Token gen ─────────────────────────────────────────────────────────────────

static std::string make_token() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream ss;
    ss << std::hex << rng() << rng();
    return ss.str();
}

// ── Custom scheme: ui-ipc://<channel>/<token> ─────────────────────────────────

static void on_uri_scheme_request(WebKitURISchemeRequest* request,
                                   gpointer               user_data)
{
    auto* impl = static_cast<WebViewImpl*>(user_data);

    // uri format: ui-ipc://<channel>/<token>
    std::string uri  = webkit_uri_scheme_request_get_uri(request);
    std::string path = uri.substr(9); // strip "ui-ipc://"

    auto slash = path.find('/');
    if (slash == std::string::npos) {
        webkit_uri_scheme_request_finish_error(request,
            g_error_new(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "bad uri"));
        return;
    }

    std::string channel = path.substr(0, slash);
    std::string token   = path.substr(slash + 1);

    // look up and consume pending buffer
    std::vector<uint8_t> data;
    {
        std::lock_guard lock(impl->pending_mutex);
        auto it = impl->pending.find(token);
        if (it == impl->pending.end()) {
            webkit_uri_scheme_request_finish_error(request,
                g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "token expired"));
            return;
        }
        data = std::move(it->second.data);
        impl->pending.erase(it);
    }

    GBytes*       bytes  = g_bytes_new(data.data(), data.size());
    GInputStream* stream = g_memory_input_stream_new_from_bytes(bytes);
    g_bytes_unref(bytes);

    webkit_uri_scheme_request_finish(
        request, stream,
        static_cast<gint64>(data.size()),
        "application/octet-stream"
    );
    g_object_unref(stream);
}

// ── JS shim ───────────────────────────────────────────────────────────────────
//
// JS API:
//
//   window.__ui.on("channel", (msg) => {})     ← listen, msg is string or ArrayBuffer
//   window.__ui.off("channel")                 ← remove listener
//   window.__ui.post("channel", data)          ← send to C++, data = string | ArrayBuffer
//
// Internal:
//   window.__ui._dispatch("channel", "token")  ← called by C++ eval for binary
//   window.__ui._dispatchText("channel", "txt")← called by C++ eval for text
//
static constexpr const char* k_shim = R"js(
window.__ui = (() => {
    const _listeners = {};

    return {
        on(channel, cb) {
            _listeners[channel] = cb;
        },

        off(channel) {
            delete _listeners[channel];
        },

        // C++ → JS binary: fetch real ArrayBuffer from custom scheme
        async _dispatch(channel, token) {
            const res = await fetch('ui-ipc://' + channel + '/' + token);
            if (!res.ok) return;
            const buf = await res.arrayBuffer();
            const cb  = _listeners[channel];
            if (cb) cb(buf);
        },

        // C++ → JS text: direct dispatch, no fetch needed
        _dispatchText(channel, text) {
            const cb = _listeners[channel];
            if (cb) cb(text);
        },

        // JS → C++: postMessage tagged with channel
        post(channel, data) {
            if (data instanceof ArrayBuffer || ArrayBuffer.isView(data)) {
                window.webkit.messageHandlers.ui.postMessage({ channel, data });
            } else {
                window.webkit.messageHandlers.ui.postMessage({ channel, data: String(data) });
            }
        }
    };
})();
)js";

// ── Script message handler (JS → C++) ────────────────────────────────────────
// webkit2gtk-4.1: the signal passes WebKitJavascriptResult*, not JSCValue* directly.
// Call webkit_javascript_result_get_js_value() to unwrap it.

static void on_script_message(WebKitUserContentManager*,
                               WebKitJavascriptResult* js_result,
                               gpointer                user_data)
{
    auto* impl  = static_cast<WebViewImpl*>(user_data);
    JSCValue* value = webkit_javascript_result_get_js_value(js_result);

    // expect an object: { channel: string, data: string | ArrayBuffer }
    if (!jsc_value_is_object(value)) return;

    JSCValue* jchannel = jsc_value_object_get_property(value, "channel");
    JSCValue* jdata    = jsc_value_object_get_property(value, "data");

    if (!jsc_value_is_string(jchannel)) {
        g_object_unref(jchannel);
        g_object_unref(jdata);
        return;
    }

    char* raw_channel = jsc_value_to_string(jchannel);
    std::string channel(raw_channel);
    g_free(raw_channel);
    g_object_unref(jchannel);

    // find the registered callback for this channel
    WebView::MessageCallback cb;
    {
        std::lock_guard lock(impl->channels_mutex);
        auto it = impl->channels.find(channel);
        if (it == impl->channels.end()) {
            g_object_unref(jdata);
            return;
        }
        cb = it->second;
    }

    if (jsc_value_is_string(jdata)) {
        char* raw = jsc_value_to_string(jdata);
        cb(Message(std::string(raw)));
        g_free(raw);

    } else if (jsc_value_is_array_buffer(jdata)) {
        gsize    size = 0;
        gpointer data = jsc_value_array_buffer_get_data(jdata, &size);
        cb(Message(std::vector<uint8_t>(
            static_cast<uint8_t*>(data),
            static_cast<uint8_t*>(data) + size
        )));

    } else if (jsc_value_is_typed_array(jdata)) {
        gsize    size = 0;
        gpointer data = jsc_value_typed_array_get_data(jdata, &size);
        cb(Message(std::vector<uint8_t>(
            static_cast<uint8_t*>(data),
            static_cast<uint8_t*>(data) + size
        )));
    }

    g_object_unref(jdata);
}

// ── WebView ───────────────────────────────────────────────────────────────────

WebView::WebView(WebViewConfig config) : impl_(new WebViewImpl()) {
    gtk_init(nullptr, nullptr);

    impl_->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(impl_->window), config.title.c_str());
    gtk_window_set_default_size(GTK_WINDOW(impl_->window), config.width, config.height);

    // register scheme before webview creation
    WebKitWebContext* ctx = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(
        ctx, "ui-ipc",
        on_uri_scheme_request,
        impl_, nullptr
    );

    auto* manager = webkit_user_content_manager_new();

    g_signal_connect(manager, "script-message-received::ui",
                     G_CALLBACK(on_script_message), impl_);

    // webkit2gtk-4.1: 2-argument form only, no world-name parameter
    webkit_user_content_manager_register_script_message_handler(manager, "ui");

    // webkit2gtk-4.1: argument order is (source, injected_frames, injection_time, ...)
    auto* script = webkit_user_script_new(
        k_shim,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        nullptr, nullptr
    );
    webkit_user_content_manager_add_script(manager, script);
    webkit_user_script_unref(script);

    impl_->webview = WEBKIT_WEB_VIEW(
        webkit_web_view_new_with_user_content_manager(manager)
    );

    if (config.debug) {
        auto* settings = webkit_web_view_get_settings(impl_->webview);
        webkit_settings_set_enable_developer_extras(settings, TRUE);
    }

    gtk_container_add(GTK_CONTAINER(impl_->window), GTK_WIDGET(impl_->webview));

    g_signal_connect(impl_->window, "delete-event",
        G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer user_data) -> gboolean {
            auto* impl = static_cast<WebViewImpl*>(user_data);
            if (impl->on_close_cb) {
                return impl->on_close_cb() ? FALSE : TRUE;
            }
            gtk_main_quit();
            return FALSE;
        }),
        impl_
    );

    gtk_widget_show_all(impl_->window);
}

WebView::~WebView() { delete impl_; }

// ── Navigation ────────────────────────────────────────────────────────────────

void WebView::load_url(std::string_view url) {
    webkit_web_view_load_uri(impl_->webview, std::string(url).c_str());
}

void WebView::load_html(std::string_view html) {
    webkit_web_view_load_html(impl_->webview, std::string(html).c_str(), nullptr);
}

// ── Eval ─────────────────────────────────────────────────────────────────────

void WebView::eval(std::string_view js) {
    webkit_web_view_evaluate_javascript(
        impl_->webview,
        std::string(js).c_str(), -1,
        nullptr, nullptr, nullptr,
        [](GObject* obj, GAsyncResult* res, gpointer) {
            GError* err = nullptr;
            auto* result = webkit_web_view_evaluate_javascript_finish(
                WEBKIT_WEB_VIEW(obj), res, &err
            );
            if (err) {
                g_warning("ui::WebView eval: %s", err->message);
                g_error_free(err);
            }
            if (result) g_object_unref(result);
        },
        nullptr
    );
}

// ── IPC: C++ → JS ────────────────────────────────────────────────────────────

void WebView::post_message(std::string_view channel, std::string_view text) {
    // TODO: escape text properly before shipping
    std::string js =
        "window.__ui._dispatchText('"
        + std::string(channel) + "','"
        + std::string(text)    + "')";
    eval(js);
}

void WebView::post_message(std::string_view channel,
                            const std::vector<uint8_t>& data)
{
    std::string token = make_token();
    {
        std::lock_guard lock(impl_->pending_mutex);
        impl_->pending[token] = { std::string(channel), data };
    }

    // JS fetches ui-ipc://<channel>/<token> → real native ArrayBuffer
    std::string js =
        "window.__ui._dispatch('"
        + std::string(channel) + "','"
        + token                + "')";
    eval(js);
}

// ── IPC: JS → C++ ────────────────────────────────────────────────────────────

void WebView::on_message(std::string_view channel, MessageCallback cb) {
    std::lock_guard lock(impl_->channels_mutex);
    impl_->channels[std::string(channel)] = std::move(cb);
}

void WebView::off_message(std::string_view channel) {
    std::lock_guard lock(impl_->channels_mutex);
    impl_->channels.erase(std::string(channel));
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void WebView::on_close(CloseCallback cb) { impl_->on_close_cb = std::move(cb); }
void WebView::run()       { gtk_main(); }
void WebView::terminate() { gtk_main_quit(); }

// ── Window ────────────────────────────────────────────────────────────────────

void WebView::set_title(std::string_view title) {
    gtk_window_set_title(GTK_WINDOW(impl_->window), std::string(title).c_str());
}

void WebView::set_size(int width, int height) {
    gtk_window_resize(GTK_WINDOW(impl_->window), width, height);
}

} // namespace ui