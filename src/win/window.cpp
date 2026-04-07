#include "impl.h"
#include "wstring.h"

#include <ui/webview.h>
#include <logger/logger.h>

#include <string>

namespace ui {

static constexpr UINT kMsgFlush = WM_APP + 1;

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* impl = reinterpret_cast<WebViewImpl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == kMsgFlush) {
        // Background thread enqueued one or more frames — drain them all now
        // that we're on the UI thread.  Coalesce any extra WM_APP+1 messages
        // that may have stacked up while we were already draining.
        if (impl && impl->webview) {
            MSG extra;
            while (PeekMessageW(&extra, hwnd, kMsgFlush, kMsgFlush, PM_REMOVE))
                ; // discard redundant wake-ups before draining once
            // WebView::drain_post_queue is a private method; expose via a
            // free friend or a small forwarding helper defined in ipc.cpp.
            // Simplest: call through the stored WebView pointer on the impl.
            // We reach it via the GWLP_USERDATA chain set in webview.cpp.
            // (See note below — we store the WebView* alongside impl.)
            if (impl->owner)
                impl->owner->drain_post_queue();
        }
        return 0;
    }

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

// ── Window methods ────────────────────────────────────────────────────────────

void WebView::set_title(std::string_view title)
{
    logger::Debug("set_title: '{}'", title);
    SetWindowTextW(impl_->hwnd, win::to_wide(title).c_str());
}

void WebView::set_size(int width, int height)
{
    logger::Debug("set_size: {}x{}", width, height);
    SetWindowPos(impl_->hwnd, nullptr, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOZORDER);
}

} // namespace ui