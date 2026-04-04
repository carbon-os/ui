#include "impl.h"

#include <ui/webview.h>
#include <logger/logger.h>

namespace ui {

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

} // namespace ui