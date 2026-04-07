#include "impl.h"
#include "wstring.h"

#include <ui/webview.h>
#include <shared/helpers.h>
#include <logger/logger.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ui {

static constexpr UINT kMsgFlush = WM_APP + 1;

// Called only on the UI thread (from wnd_proc's WM_APP+1 handler and from
// the inbound message callback).  Drains post_queue into WebView2.
void WebView::drain_post_queue()
{
    std::unique_lock lock(impl_->post_mutex);
    while (!impl_->post_queue.empty()) {
        OutboundFrame frame = std::move(impl_->post_queue.front());
        impl_->post_queue.pop();
        lock.unlock();

        if (!frame.binary) {
            logger::Info("drain: text → channel='{}' ({} chars)",
                         frame.channel, frame.text.size());

            std::string json =
                "{\"type\":\"host_ipc_message\","
                "\"channel\":\"" + shared::json_escape(frame.channel) + "\","
                "\"text\":\""    + shared::json_escape(frame.text)    + "\"}";
            impl_->webview->PostWebMessageAsJson(win::to_wide(json).c_str());

        } else {
            uint64_t    token    = impl_->next_token.fetch_add(1, std::memory_order_relaxed);
            std::string slot_key = frame.channel + ":" + std::to_string(token);

            {
                std::lock_guard slock(impl_->slots_mutex);
                impl_->slots[slot_key] = std::move(frame.data);
            }

            logger::Info("drain: binary → channel='{}' token={} ({} bytes)",
                         frame.channel, token,
                         impl_->slots[slot_key].size());

            std::string json =
                "{\"type\":\"host_ipc_message\","
                "\"channel\":\"" + shared::json_escape(frame.channel) + "\","
                "\"token\":\""   + std::to_string(token)              + "\"}";
            impl_->webview->PostWebMessageAsJson(win::to_wide(json).c_str());
        }

        lock.lock();
    }
}

// ── Public API — safe to call from any thread ─────────────────────────────────

void WebView::post_message(std::string_view channel, std::string_view text)
{
    {
        std::lock_guard lock(impl_->post_mutex);
        OutboundFrame f;
        f.channel = channel;
        f.binary  = false;
        f.text    = std::string(text);
        impl_->post_queue.push(std::move(f));
    }
    PostMessageW(impl_->hwnd, kMsgFlush, 0, 0);
}

void WebView::post_message(std::string_view channel, const std::vector<uint8_t>& data)
{
    {
        std::lock_guard lock(impl_->post_mutex);
        OutboundFrame f;
        f.channel = channel;
        f.binary  = true;
        f.data    = data;
        impl_->post_queue.push(std::move(f));
    }
    PostMessageW(impl_->hwnd, kMsgFlush, 0, 0);
}

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

} // namespace ui