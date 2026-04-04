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

void WebView::post_message(std::string_view channel, std::string_view text)
{
    logger::Info("post_message (text) → channel='{}' ({} chars)", channel, text.size());

    std::string json =
        "{\"type\":\"host_ipc_message\","
        "\"channel\":\"" + shared::json_escape(channel) + "\","
        "\"text\":\""    + shared::json_escape(text)    + "\"}";
    impl_->webview->PostWebMessageAsJson(win::to_wide(json).c_str());
}

void WebView::post_message(std::string_view channel, const std::vector<uint8_t>& data)
{
    uint64_t    token    = impl_->next_token.fetch_add(1, std::memory_order_relaxed);
    std::string slot_key = std::string(channel) + ":" + std::to_string(token);

    {
        std::lock_guard lock(impl_->slots_mutex);
        impl_->slots[slot_key] = data;
    }

    logger::Info("post_message (binary) → channel='{}' token={} ({} bytes)",
                 channel, token, data.size());

    std::string json =
        "{\"type\":\"host_ipc_message\","
        "\"channel\":\"" + shared::json_escape(channel)  + "\","
        "\"token\":\""   + std::to_string(token)         + "\"}";
    impl_->webview->PostWebMessageAsJson(win::to_wide(json).c_str());
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