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

} // namespace ui