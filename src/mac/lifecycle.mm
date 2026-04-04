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

} // namespace ui