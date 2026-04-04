#include "impl.h"
#include "scheme.h"

#include <ui/webview.h>
#include <shared/shim.h>
#include <logger/logger.h>

#include <string>

namespace ui {

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

    WebKitWebContext* ctx = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(ctx, "ui-ipc",
        on_uri_scheme_request, impl_, nullptr);
    logger::Debug("custom scheme 'ui-ipc' registered");

    auto* manager = webkit_user_content_manager_new();

    auto* script = webkit_user_script_new(
        shared::k_shim,
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

} // namespace ui